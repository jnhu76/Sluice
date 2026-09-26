#include <sluice/async/detail/request_core.hpp>

#include <exception>
#include <limits>

namespace sluice::async::detail {

namespace {

[[noreturn]] void request_core_invariant_fail_fast() noexcept { std::terminate(); }

}  // namespace

RequestCore::RequestCore(ContextIdentity context, std::size_t request_capacity)
    : context_(context), capacity_(request_capacity), slots_(request_capacity) {
    free_slots_.reserve(request_capacity);
    for (std::size_t i = request_capacity; i > 0; --i) {
        free_slots_.push_back(static_cast<std::uint32_t>(i - 1));
    }
}

RequestCore::~RequestCore() {
    for (const Slot& slot : slots_) {
        if (slot.phase == SlotPhase::reserved || slot.phase == SlotPhase::accepted) {
            request_core_invariant_fail_fast();
        }
    }
}

std::size_t RequestCore::capacity() const noexcept { return capacity_; }

ContextIdentity RequestCore::context() const noexcept { return context_; }

CoreOccupancy RequestCore::occupancy() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    CoreOccupancy occupancy;
    for (const Slot& slot : slots_) {
        if (slot.phase != SlotPhase::accepted) {
            continue;
        }
        ++occupancy.accepted_live;
        if (slot.binding_live) {
            ++occupancy.public_bindings;
        }
        if (!slot.published) {
            ++occupancy.outstanding;
        }
    }
    return occupancy;
}

RequestCore::Slot* RequestCore::resolve_reserved_(RequestReservation reservation) noexcept {
    if (reservation.slot.value >= capacity_)
        return nullptr;
    Slot& slot = slots_[reservation.slot.value];
    if (slot.phase != SlotPhase::reserved)
        return nullptr;
    if (slot.generation != reservation.generation)
        return nullptr;
    return &slot;
}

RequestCore::Slot* RequestCore::resolve_internal_(RequestKey id) noexcept {
    if (id.slot.value >= capacity_)
        return nullptr;
    if (id.context != context_)
        return nullptr;
    Slot& slot = slots_[id.slot.value];
    if (slot.phase != SlotPhase::accepted)
        return nullptr;
    if (slot.generation != id.generation)
        return nullptr;
    return &slot;
}

const RequestCore::Slot* RequestCore::resolve_internal_(RequestKey id) const noexcept {
    if (id.slot.value >= capacity_)
        return nullptr;
    if (id.context != context_)
        return nullptr;
    const Slot& slot = slots_[id.slot.value];
    if (slot.phase != SlotPhase::accepted)
        return nullptr;
    if (slot.generation != id.generation)
        return nullptr;
    return &slot;
}

RequestCore::Slot* RequestCore::resolve_public_(RequestKey id) noexcept {
    Slot* slot = resolve_internal_(id);
    if (slot == nullptr)
        return nullptr;
    if (!slot->binding_live)
        return nullptr;
    return slot;
}

const RequestCore::Slot* RequestCore::resolve_public_(RequestKey id) const noexcept {
    const Slot* slot = resolve_internal_(id);
    if (slot == nullptr)
        return nullptr;
    if (!slot->binding_live)
        return nullptr;
    return slot;
}

ReserveAttempt RequestCore::reserve() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!admission_open_) {
        return ReserveAttempt{ReserveStatus::admission_closed, {}};
    }
    if (free_slots_.empty()) {
        if (retired_count_ == capacity_) {
            return ReserveAttempt{ReserveStatus::exhausted, {}};
        }
        return ReserveAttempt{ReserveStatus::capacity, {}};
    }
    const std::uint32_t index = free_slots_.back();
    free_slots_.pop_back();
    Slot& slot = slots_[index];
    slot.phase = SlotPhase::reserved;
    slot.accepted = false;
    slot.binding_live = false;
    slot.terminal_chosen = false;
    slot.published = false;
    slot.publication_inflight = false;
    slot.execution_claimed = false;
    slot.cancel_intent = false;
    slot.zero_op = false;
    slot.observer_registered = false;
    slot.execution_refs = 0;
    slot.control_refs = 0;
    slot.descriptor = {};
    slot.borrow = {};
    slot.outcome = {};
    return ReserveAttempt{ReserveStatus::reserved,
                          RequestReservation{SlotIndex{index}, slot.generation}};
}

AcceptAttempt RequestCore::accept(RequestReservation reservation,
                                  const RequestDescriptor& descriptor,
                                  BorrowFacts borrow) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_reserved_(reservation);
    if (slot == nullptr) {
        return AcceptAttempt{AcceptStatus::bad_reservation, {}};
    }
    if (!admission_open_) {
        return AcceptAttempt{AcceptStatus::admission_closed, {}};
    }
    slot->phase = SlotPhase::accepted;
    slot->accepted = true;
    slot->binding_live = true;
    slot->execution_claimed = false;
    slot->cancel_intent = false;
    slot->descriptor = descriptor;
    slot->borrow = borrow;
    slot->execution_refs = 1;
    if (descriptor.zero_op) {
        slot->zero_op = true;
        slot->terminal_chosen = true;
        slot->outcome = IoOutcome::success(0);
        slot->execution_refs = 0;
    }
    const RequestKey id{context_, reservation.slot, reservation.generation};
    return AcceptAttempt{AcceptStatus::accepted, id};
}

bool RequestCore::rollback(RequestReservation reservation) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_reserved_(reservation);
    if (slot == nullptr) {
        return false;
    }
    release_slot_(*slot, reservation.slot.value);
    return true;
}

void RequestCore::close_admission() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    admission_open_ = false;
}

bool RequestCore::admission_open() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return admission_open_;
}

void RequestCore::note_health_failure() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    health_failed_ = true;
    admission_open_ = false;
}

bool RequestCore::health_failed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return health_failed_;
}

PublicLookup RequestCore::lookup(RequestKey id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const Slot* slot = resolve_public_(id);
    if (slot == nullptr) {
        return PublicLookup::not_found;
    }
    return slot->published ? PublicLookup::published : PublicLookup::outstanding;
}

PublicCancel RequestCore::cancel(RequestKey id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_public_(id);
    if (slot == nullptr) {
        return PublicCancel::not_found;
    }
    if (slot->terminal_chosen) {
        return PublicCancel::already_terminal;
    }
    if (slot->execution_claimed) {
        slot->cancel_intent = true;
        return PublicCancel::requested;
    }
    slot->terminal_chosen = true;
    slot->outcome = sluice::detail::canceled_before_dispatch();
    return PublicCancel::won_before_execution;
}

BindingRelease RequestCore::release_public_binding(RequestKey id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_public_(id);
    if (slot == nullptr) {
        return BindingRelease::stale;
    }
    const bool visible = slot->terminal_chosen && slot->execution_refs == 0 &&
                         (slot->publication_inflight || slot->published);
    if (!visible) {
        return BindingRelease::not_visible_yet;
    }
    slot->binding_live = false;
    try_reclaim_(*slot, id.slot.value);
    return BindingRelease::released;
}

BindingRelease RequestCore::discard_public_result(RequestKey id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_public_(id);
    if (slot == nullptr) {
        return BindingRelease::stale;
    }
#if defined(SLUICE_B2_MUTANT_DISCARD_ACCEPTS_INFLIGHT)
    const bool published = slot->publication_inflight || slot->published;
#else
    const bool published = slot->published;
#endif
    if (!published) {
        return BindingRelease::not_visible_yet;
    }
    slot->binding_live = false;
    try_reclaim_(*slot, id.slot.value);
    return BindingRelease::released;
}

PublicObservation RequestCore::observe_public_result(RequestKey id, IoOutcome* out) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const Slot* slot = resolve_public_(id);
    if (slot == nullptr) {
        return PublicObservation::stale;
    }
#if defined(SLUICE_B2_MUTANT_OBSERVE_IGNORES_PUBLICATION)
    const bool published = true;
#else
    const bool published = slot->published;
#endif
    if (!published) {
        return PublicObservation::pending;
    }
    if (out != nullptr) {
        *out = slot->outcome;
    }
#if defined(SLUICE_B2_MUTANT_OBSERVE_CONSUMES)
    const_cast<Slot*>(slot)->binding_live = false;
#endif
    return PublicObservation::ready;
}

PublicConsumption RequestCore::consume_public_result(RequestKey id, IoOutcome* out) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_public_(id);
    if (slot == nullptr) {
        return PublicConsumption::stale;
    }
    if (!slot->published) {
        return PublicConsumption::pending;
    }
    if (out != nullptr) {
        *out = slot->outcome;
    }
#if !defined(SLUICE_B2_MUTANT_CONSUME_KEEPS_BINDING)
    slot->binding_live = false;
    try_reclaim_(*slot, id.slot.value);
#endif
    return PublicConsumption::consumed;
}

TerminalVerdict RequestCore::offer_terminal(RequestKey id,
                                             const TerminalCandidate& candidate) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_internal_(id);
    if (slot == nullptr) {
        return TerminalVerdict::rejected_stale;
    }
    if (slot->terminal_chosen) {
        return TerminalVerdict::rejected_duplicate;
    }
    if (candidate.kind == TerminalCandidateKind::zero_effect_cancel && slot->execution_claimed) {
        return TerminalVerdict::rejected_inadmissible;
    }
    slot->terminal_chosen = true;
    slot->outcome = candidate.outcome;
    slot->cancel_intent = false;
    return TerminalVerdict::chosen;
}

ExecutionClaim RequestCore::claim_execution(RequestKey id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_internal_(id);
    if (slot == nullptr) {
        return ExecutionClaim::stale;
    }
    if (slot->execution_claimed || slot->terminal_chosen) {
        return ExecutionClaim::late;
    }
    slot->execution_claimed = true;
    return ExecutionClaim::claimed;
}

bool RequestCore::acquire_execution(RequestKey id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_internal_(id);
    if (slot == nullptr) {
        return false;
    }
    if (slot->terminal_chosen || slot->published) {
        return false;
    }
    ++slot->execution_refs;
    return true;
}

ExecutionRelease RequestCore::release_execution(RequestKey id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_internal_(id);
    if (slot == nullptr) {
        return ExecutionRelease::stale;
    }
    if (slot->execution_refs == 0) {
        return ExecutionRelease::underflow_rejected;
    }
    if (slot->execution_refs == 1 && !slot->terminal_chosen) {
        return ExecutionRelease::premature_rejected;
    }
    --slot->execution_refs;
    const bool fully_retired = slot->execution_refs == 0 && slot->terminal_chosen;
    try_reclaim_(*slot, id.slot.value);
    return fully_retired ? ExecutionRelease::borrow_touch_fully_retired : ExecutionRelease::released;
}

bool RequestCore::acquire_control(RequestKey id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_internal_(id);
    if (slot == nullptr) {
        return false;
    }
    if (slot->published && !slot->binding_live && slot->execution_refs == 0) {
        return false;
    }
    ++slot->control_refs;
    return true;
}

ControlRelease RequestCore::release_control(RequestKey id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_internal_(id);
    if (slot == nullptr) {
        return ControlRelease::stale;
    }
    if (slot->control_refs == 0) {
        return ControlRelease::underflow_rejected;
    }
    --slot->control_refs;
    try_reclaim_(*slot, id.slot.value);
    return ControlRelease::released;
}

PublicationGrant RequestCore::begin_publication(RequestKey id, PublicationPayload* out) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_internal_(id);
    if (slot == nullptr) {
        return PublicationGrant::stale;
    }
    if (slot->publication_inflight || slot->published) {
        return PublicationGrant::duplicate_publisher;
    }
    if (!slot->terminal_chosen || slot->execution_refs > 0 || !slot->binding_live) {
        return PublicationGrant::not_eligible;
    }
    slot->publication_inflight = true;
    if (out != nullptr) {
        out->id = id;
        out->op = slot->descriptor.op;
        out->offset = slot->descriptor.offset;
        out->requested_bytes = slot->descriptor.length;
        out->outcome = slot->outcome;
    }
    return PublicationGrant::granted;
}

PublicationCompletion RequestCore::complete_publication(RequestKey id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_internal_(id);
    if (slot == nullptr) {
        return PublicationCompletion::stale;
    }
    if (!slot->publication_inflight) {
        return PublicationCompletion::not_in_flight;
    }
    slot->publication_inflight = false;
    slot->published = true;
    try_reclaim_(*slot, id.slot.value);
    return PublicationCompletion::completed;
}

ObserverRegistration RequestCore::register_observer(RequestKey id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_public_(id);
    if (slot == nullptr) {
        return ObserverRegistration::not_found;
    }
    if (slot->observer_registered) {
        return ObserverRegistration::duplicate;
    }
    if (slot->published) {
        return ObserverRegistration::already_terminal;
    }
    slot->observer_registered = true;
    return ObserverRegistration::armed;
}

ObserverRetirement RequestCore::retire_observer(RequestKey id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot* slot = resolve_internal_(id);
    if (slot == nullptr) {
        return ObserverRetirement::not_found;
    }
    if (!slot->observer_registered) {
        return ObserverRetirement::not_registered;
    }
    slot->observer_registered = false;
    try_reclaim_(*slot, id.slot.value);
    return ObserverRetirement::retired;
}

bool RequestCore::reclaimable_(const Slot& slot) const noexcept {
    return slot.phase == SlotPhase::accepted && slot.published && !slot.binding_live &&
           slot.execution_refs == 0 && slot.control_refs == 0 && !slot.publication_inflight &&
           !slot.observer_registered;
}

void RequestCore::try_reclaim_(Slot& slot, std::size_t index) noexcept {
    if (!reclaimable_(slot)) {
        return;
    }
    release_slot_(slot, index);
}

void RequestCore::release_slot_(Slot& slot, std::size_t index) noexcept {
    slot.phase = SlotPhase::free;
    slot.accepted = false;
    slot.binding_live = false;
    slot.terminal_chosen = false;
    slot.published = false;
    slot.publication_inflight = false;
    slot.execution_claimed = false;
    slot.cancel_intent = false;
    slot.zero_op = false;
    slot.observer_registered = false;
    slot.execution_refs = 0;
    slot.control_refs = 0;
    slot.descriptor = {};
    slot.borrow = {};
    slot.outcome = {};
    if (slot.generation.value == std::numeric_limits<std::uint64_t>::max()) {
        slot.phase = SlotPhase::retired;
        ++retired_count_;
        return;
    }
    slot.generation = Generation{slot.generation.value + 1};
    free_slots_.push_back(static_cast<std::uint32_t>(index));
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

std::optional<RequestCore::SlotObservation> RequestCore::observe_slot(SlotIndex slot) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot.value >= capacity_) {
        return std::nullopt;
    }
    const Slot& s = slots_[slot.value];
    SlotObservation observation;
    observation.phase = s.phase;
    observation.generation = s.generation;
    observation.accepted = s.accepted;
    observation.binding_live = s.binding_live;
    observation.terminal_chosen = s.terminal_chosen;
    observation.published = s.published;
    observation.publication_inflight = s.publication_inflight;
    observation.execution_claimed = s.execution_claimed;
    observation.cancel_intent = s.cancel_intent;
    observation.observer_registered = s.observer_registered;
    observation.execution_refs = s.execution_refs;
    observation.control_refs = s.control_refs;
    observation.op = s.descriptor.op;
    observation.offset = s.descriptor.offset;
    observation.requested_bytes = s.descriptor.length;
    observation.has_outcome = s.terminal_chosen;
    observation.outcome = s.outcome;
    return observation;
}

CoreSnapshot RequestCore::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    CoreSnapshot snap;
    snap.free_slots = free_slots_.size();
    snap.retired_slots = retired_count_;
    snap.admission_open = admission_open_;
    snap.health_failed = health_failed_;
    for (const Slot& slot : slots_) {
        if (slot.phase == SlotPhase::reserved) {
            ++snap.reserved;
        }
        if (slot.phase == SlotPhase::accepted) {
            ++snap.accepted_live;
            if (slot.terminal_chosen) {
                ++snap.terminal_live;
            }
            if (slot.published) {
                ++snap.published_live;
            }
            if (slot.binding_live) {
                ++snap.public_bindings;
            }
            if (slot.publication_inflight) {
                ++snap.publication_refs;
            }
            snap.execution_refs += slot.execution_refs;
            snap.control_refs += slot.control_refs;
        }
    }
    return snap;
}

void RequestCore::force_generation_for_testing(SlotIndex slot, Generation generation) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot.value >= capacity_) {
        request_core_invariant_fail_fast();
    }
    slots_[slot.value].generation = generation;
}

#endif

}  // namespace sluice::async::detail
