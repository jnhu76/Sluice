// SQ-side machinery of UringAsyncBackend: accepted-work dispatch, transport
// ledger accounting against the kernel SQ head, poison/Class-A recovery, the
// cookie/router lifecycle, and cancel controls. The profile setup, admission
// path, publication and public surface live in uring_backend.cpp; the CQ side
// lives in uring_completion.cpp.

#include "uring_internal.hpp"

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstdint>
#include <cstdio>

namespace sluice::async {

#if defined(SLUICE_HAS_LIBURING)

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

std::size_t UringAsyncBackend::dispatch_size_for_test() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return dispatch_->size();
}

std::size_t UringAsyncBackend::transport_ledger_size_for_test() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return transport_ledger_ == nullptr ? 0 : transport_ledger_->size();
}

std::size_t UringAsyncBackend::sq_ready_for_test() const noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    return have_ring_ ? static_cast<std::size_t>(::io_uring_sq_ready(&ring_state_->ring)) : 0;
}

std::size_t UringAsyncBackend::live_control_sqes_for_test() const noexcept {
    return live_control_sqes_.load(std::memory_order_relaxed);
}
#endif

void UringAsyncBackend::dispatch_after_accept(detail::SlotHandle h) noexcept {
    bool injected_dispatch_failure = false;
    bool newly_poisoned = false;
    bool unsubmitted_after_dispatch = false;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        auto* inj = dispatch_failure_injection_.load(std::memory_order_acquire);
        if (inj != nullptr && inj->armed.load(std::memory_order_acquire)) {
            inj->fired.fetch_add(1, std::memory_order_relaxed);
            const detail::RequestKey key{core_->context(), h.slot, h.generation};
            detail::TerminalCandidate candidate;
            candidate.kind = detail::TerminalCandidateKind::physical_outcome;
            candidate.outcome = sluice::detail::IoOutcome::failure(
                IoError{IoError::Code::backend_error});
            if (core_->offer_terminal(key, candidate) != detail::TerminalVerdict::chosen) {
                detail::uring_core_handoff_fail_fast();
            }
            if (core_->release_execution(key) !=
                detail::ExecutionRelease::borrow_touch_fully_retired) {
                detail::uring_core_handoff_fail_fast();
            }
            publication_pending_->push_back(h);
            injected_dispatch_failure = true;
        } else
#endif
        {
            dispatch_->push_back(h);

            const bool poisoned_before = fatal_error_.has_value();
            for (;;) {
                if (dispatch_->empty())
                    break;
                const detail::SlotHandle front = dispatch_->front();
                if (!dispatch_one_locked(front))
                    break;
            }
            if (!fatal_error_.has_value()) {
                (void)submit_transport_locked();
                service_pending_controls_locked_();
            }
            newly_poisoned = !poisoned_before && fatal_error_.has_value();
        }
#if !defined(SLUICE_B1C_MUTANT_TRANSPORT_RETRY_UNADVERTISED)
        unsubmitted_after_dispatch =
            !fatal_error_.has_value() && dispatch_retry_remains_locked_();
#endif
    }
    if (injected_dispatch_failure || newly_poisoned || unsubmitted_after_dispatch) {
        signal_ready_progress();
    }
}

bool UringAsyncBackend::dispatch_one_locked(detail::SlotHandle h) noexcept {
    if (fatal_error_.has_value())
        return false;

    if (cookie_free_list_.empty()) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: router exhaustion "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
    if (sqe == nullptr) {
        (void)submit_transport_locked();
        if (fatal_error_.has_value())
            return false;
        sqe = ::io_uring_get_sqe(&ring_state_->ring);
        if (sqe == nullptr)
            return false;
    }

    const detail::RequestKey key{core_->context(), h.slot, h.generation};
    if (core_->claim_execution(key) != detail::ExecutionClaim::claimed) {
        // The SQE slot was taken but nothing was written into it and no
        // submission occurred, so returning it to the userspace tail keeps
        // the ring accounting exact.
        --ring_state_->ring.sq.sqe_tail;
        (void)dispatch_->remove_exact(h);
        return true;
    }

    detail::SlotIndex router_slot = cookie_free_list_.back();
    cookie_free_list_.pop_back();

    const std::uint64_t op_cookie = allocate_cookie_();
    const PreparedUringOp& prep = prepared_ops_[h.slot.value];
    switch (prep.kind) {
    case detail::OperationKind::read:
        ::io_uring_prep_read(sqe, prep.fd, const_cast<std::byte*>(prep.buffer), prep.native_length,
                             static_cast<off_t>(static_cast<std::int64_t>(prep.offset)));
        break;
    case detail::OperationKind::write:
        ::io_uring_prep_write(sqe, prep.fd, prep.buffer, prep.native_length,
                              static_cast<off_t>(static_cast<std::int64_t>(prep.offset)));
        break;
    case detail::OperationKind::sync_data:
        ::io_uring_prep_fsync(sqe, prep.fd, IORING_FSYNC_DATASYNC);
        break;
    case detail::OperationKind::sync_all:
        ::io_uring_prep_fsync(sqe, prep.fd, 0);
        break;
    case detail::OperationKind::file_info:
    case detail::OperationKind::size:
        ::io_uring_prep_statx(sqe, prep.fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
                              STATX_TYPE | STATX_MODE | STATX_SIZE | STATX_INO,
                              prep.statx_buffer);
        break;
    }

    ::io_uring_sqe_set_data64(sqe, op_cookie);
    RouterEntry& route = router_[router_slot.value];
    route = RouterEntry{};
    route.cookie = op_cookie;
    route.handle = h;
    route.in_use = true;
    live_cookies_.fetch_add(1, std::memory_order_relaxed);
    const auto& sq = ring_state_->ring.sq;
    const std::uint32_t physical_position =
        static_cast<std::uint32_t>((sq.sqe_tail - 1u) & sq.ring_mask);
    transport_ledger_->append(TransportLedger::Kind::operation, physical_position, op_cookie, h);

    if (!dispatch_->remove_exact(h)) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: dispatch_one_locked "
                             "remove_exact miss after claim (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    return true;
}

bool UringAsyncBackend::dispatch_retry_remains_locked_() const noexcept {
    if (!dispatch_->empty())
        return true;
    return transport_ledger_ != nullptr && !transport_ledger_->empty() &&
           !transport_ledger_->all_class_a_recovery_retired();
}

int UringAsyncBackend::submit_transport_locked() noexcept {
    if (fatal_error_.has_value() || transport_ledger_ == nullptr || transport_ledger_->empty())
        return 0;

    submit_flushes_.fetch_add(1, std::memory_order_relaxed);
    int rc = 0;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    if (ring_state_->test_hooks.submit != nullptr) {
        rc = ring_state_->test_hooks.submit(ring_state_->test_hooks.context, &ring_state_->ring);
    } else {
        rc = ::io_uring_submit(&ring_state_->ring);
    }
#else
    rc = ::io_uring_submit(&ring_state_->ring);
#endif
    account_transport_result_locked(rc, true);
    return rc;
}

void UringAsyncBackend::account_transport_result_locked(int rc,
                                                        bool had_pending_transport) noexcept {
    if (!had_pending_transport)
        return;

    if (rc > 0) {
        const std::size_t consumed = static_cast<std::size_t>(rc);
        if (consumed > transport_ledger_->size()) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: submit consumed more SQEs "
                                 "than the physical ledger contains (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        for (std::size_t i = 0; i < consumed; ++i) {
            const TransportLedger::Entry entry = transport_ledger_->pop_front();
            if (entry.kind == TransportLedger::Kind::cancel_control) {
                mark_consumed_control_locked_(entry.cookie, entry.handle);
            }
        }
        return;
    }
    if (rc == 0)
        return;

    const int err = -rc;
    if (err == EINTR || err == EAGAIN || err == EBUSY) {
#if !defined(SLUICE_E1_MUTANT_SUBMIT_BATCH_INVISIBLE)
        // A partial kernel fetch before the retryable error would otherwise
        // desync this ledger against the next retry's consumed count.
        (void)reconcile_ledger_with_kernel_locked_();
#endif
        return;
    }

#if !defined(SLUICE_E1_MUTANT_SUBMIT_BATCH_INVISIBLE)
    // Resolve each ledger entry against the kernel SQ head before poison:
    // entries the head already passed are kernel-visible and keep their real
    // completion path; the remainder are provably invisible because no
    // submit runs after poison.
    (void)reconcile_ledger_with_kernel_locked_();
#endif
    poison_and_recover_locked(IoError{IoError::Code::backend_error, err});
}

void UringAsyncBackend::mark_consumed_control_locked_(std::uint64_t cookie,
                                                      detail::SlotHandle handle) noexcept {
    const std::size_t router_index = find_live_router_cookie_(cookie);
    if (router_index == router_.size() ||
        router_[router_index].handle.slot.value != handle.slot.value ||
        router_[router_index].handle.generation.value != handle.generation.value ||
        router_[router_index].control_state != RouterEntry::ControlState::prepared) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: consumed control "
                             "lost its exact prepared router reference "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    router_[router_index].control_state = RouterEntry::ControlState::submitted;
    live_control_sqes_.fetch_add(1, std::memory_order_relaxed);
}

std::size_t UringAsyncBackend::reconcile_ledger_with_kernel_locked_() noexcept {
    if (transport_ledger_ == nullptr || transport_ledger_->empty())
        return 0;
    const auto& sq = ring_state_->ring.sq;
#if defined(SLUICE_E1_MUTANT_MASKED_SQ_CARDINALITY)
    const unsigned khead = *sq.khead;
    const unsigned tail = static_cast<unsigned>(sq.sqe_tail & sq.ring_mask);
    const unsigned unconsumed = static_cast<unsigned>((tail - khead) & sq.ring_mask);
#else
    // Cardinality is a difference of the free-running SQ counters (the
    // io_uring_sq_ready contract); masking either side before the subtraction
    // aliases a full ring onto an empty one.
    const unsigned unconsumed = ::io_uring_sq_ready(&ring_state_->ring);
#endif
    const std::size_t size = transport_ledger_->size();
    if (unconsumed > sq.ring_entries) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: kernel reports more "
                             "unconsumed SQ slots than the ring holds "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    if (unconsumed > size) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: kernel reports more "
                             "unconsumed SQ slots than the physical ledger contains "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    const std::size_t consumed = size - unconsumed;
    for (std::size_t i = 0; i < consumed; ++i) {
        const TransportLedger::Entry entry = transport_ledger_->pop_front();
        if (entry.kind == TransportLedger::Kind::cancel_control) {
            mark_consumed_control_locked_(entry.cookie, entry.handle);
        }
    }
    return consumed;
}

void UringAsyncBackend::poison_and_recover_locked(IoError error) noexcept {
    if (fatal_error_.has_value())
        return;
    fatal_error_ = error;
    core_->note_health_failure();

    // Every ledger entry still present here was resolved against the kernel
    // SQ head by the caller's reconciliation: the head has not passed it and
    // no submit runs after poison (dispatch, transport submission and new
    // admission are gated on this flag; the overflow flush enters with a
    // zero submit count), so it is provably kernel-invisible and Class-A
    // retirement is safe. Operations whose CQEs may still arrive left this
    // ledger during reconciliation and keep their real completion path.
    detail::SlotHandle local{};
    while (dispatch_->pop_front(local)) {
        const detail::RequestKey key{core_->context(), local.slot, local.generation};
        detail::TerminalCandidate candidate;
        candidate.kind = detail::TerminalCandidateKind::physical_outcome;
#if defined(SLUICE_E2_MUTANT_M5_POISON_FABRICATES_SUCCESS)
        candidate.outcome = sluice::detail::IoOutcome::success(0);
#else
        candidate.outcome = sluice::detail::IoOutcome::failure(error);
#endif
        if (core_->offer_terminal(key, candidate) != detail::TerminalVerdict::chosen) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: local poison retirement "
                                 "lost terminal authority (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        if (core_->release_execution(key) !=
            detail::ExecutionRelease::borrow_touch_fully_retired) {
            detail::uring_core_handoff_fail_fast();
        }
        publication_pending_->push_back(local);
        bump(stats_, &AsyncStats::completion_errors);
    }

#if defined(SLUICE_B1C_MUTANT_STRAND_POST_ACCEPT_SUBMIT_FAILURE)
    for (std::size_t i = 0; i < 0; ++i)
#else
    for (std::size_t i = 0; i < transport_ledger_->size(); ++i)
#endif
    {
        TransportLedger::Entry& physical = transport_ledger_->at(i);
        if (physical.class_a_recovery_retired) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: duplicate Class-A "
                                 "recovery retirement (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }

        const std::size_t router_index = find_live_router_cookie_(physical.cookie);
        if (router_index == router_.size() ||
            router_[router_index].handle.slot.value != physical.handle.slot.value ||
            router_[router_index].handle.generation.value != physical.handle.generation.value) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: Class-A "
                                 "recovery lost identity "
                                 "(invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        RouterEntry& route = router_[router_index];
        const detail::RequestKey key{core_->context(), route.handle.slot, route.handle.generation};

        if (physical.kind == TransportLedger::Kind::operation) {
            if (!route.terminal_delivered) {
                detail::TerminalCandidate candidate;
                candidate.kind = detail::TerminalCandidateKind::physical_outcome;
#if defined(SLUICE_E2_MUTANT_M5_POISON_FABRICATES_SUCCESS)
                candidate.outcome = sluice::detail::IoOutcome::success(0);
#else
                candidate.outcome = sluice::detail::IoOutcome::failure(error);
#endif
                if (core_->offer_terminal(key, candidate) != detail::TerminalVerdict::chosen) {
                    std::fprintf(stderr, "sluice::async::UringAsyncBackend: Class-A operation "
                                         "recovery lost terminal authority (invariant "
                                         "violation)\n");
                    std::fflush(stderr);
                    std::terminate();
                }
                if (core_->release_execution(key) !=
                    detail::ExecutionRelease::borrow_touch_fully_retired) {
                    detail::uring_core_handoff_fail_fast();
                }
                route.terminal_delivered = true;
                publication_pending_->push_back(route.handle);
                bump(stats_, &AsyncStats::completion_errors);
            }
        } else {
            if (route.control_state == RouterEntry::ControlState::submitted) {
                live_control_sqes_.fetch_sub(1, std::memory_order_relaxed);
            }
            if (route.control_state != RouterEntry::ControlState::none) {
                route.control_state = RouterEntry::ControlState::none;
                if (core_->release_control(key) != detail::ControlRelease::released) {
                    detail::uring_core_handoff_fail_fast();
                }
            }
        }
        // Retire only when the original outcome is already delivered; an
        // entry still waiting for its original CQE must stay routable or
        // that CQE would strand the request.
        if (route.terminal_delivered && route.control_state == RouterEntry::ControlState::none) {
            retire_router_entry_(router_index);
        }
        physical.class_a_recovery_retired = true;
    }

#if defined(SLUICE_E2_MUTANT_M9_POISON_RELEASES_RUNNING_BORROW)
    for (std::size_t router_index = 0; router_index < router_.size(); ++router_index) {
        RouterEntry& route = router_[router_index];
        if (!route.in_use || route.terminal_delivered)
            continue;
        const detail::RequestKey key{core_->context(), route.handle.slot, route.handle.generation};
        detail::TerminalCandidate fabricated;
        fabricated.kind = detail::TerminalCandidateKind::physical_outcome;
        fabricated.outcome = sluice::detail::IoOutcome::uncertain(error);
        if (core_->offer_terminal(key, fabricated) == detail::TerminalVerdict::chosen) {
            route.terminal_delivered = true;
            (void)core_->release_execution(key);
            publication_pending_->push_back(route.handle);
        }
    }
#endif
    // Routes whose completions are absent from the visible CQ stay pending
    // with their borrows: absence does not prove an operation finished (its
    // CQE may be parked in the unflushable overflow list while the operation
    // itself is still executing and still owns the caller's buffer), so only
    // a delivered CQE may retire them.
#if defined(SLUICE_E1_MUTANT_OVERFLOW_ABSENCE_SETTLES_INFLIGHT)
    if (::io_uring_cq_has_overflow(&ring_state_->ring)) {
        const auto& cq = ring_state_->ring.cq;
        const unsigned visible_ready = ::io_uring_cq_ready(&ring_state_->ring);
        const unsigned visible_head = *cq.khead;
        const auto completion_visible = [&](std::uint64_t raw_user_data) noexcept {
            for (unsigned i = 0; i < visible_ready; ++i) {
                const ::io_uring_cqe* cqe = &cq.cqes[(visible_head + i) & *cq.kring_mask];
                if (::io_uring_cqe_get_data64(cqe) == raw_user_data)
                    return true;
            }
            return false;
        };
        for (std::size_t router_index = 0; router_index < router_.size(); ++router_index) {
            RouterEntry& route = router_[router_index];
            if (!route.in_use || route.terminal_delivered)
                continue;
            const bool control_visible = completion_visible(make_control_cookie(route.cookie));
            const bool original_visible = completion_visible(route.cookie);
            if (control_visible && original_visible)
                continue;
            const detail::RequestKey key{core_->context(), route.handle.slot, route.handle.generation};
            if (!control_visible && route.control_state != RouterEntry::ControlState::none) {
                if (route.control_state == RouterEntry::ControlState::submitted) {
                    live_control_sqes_.fetch_sub(1, std::memory_order_relaxed);
                }
                route.control_state = RouterEntry::ControlState::none;
                if (core_->release_control(key) != detail::ControlRelease::released) {
                    detail::uring_core_handoff_fail_fast();
                }
            }
            if (!original_visible) {
                detail::TerminalCandidate candidate;
                candidate.kind = detail::TerminalCandidateKind::physical_outcome;
                candidate.outcome = sluice::detail::IoOutcome::uncertain(error);
                if (core_->offer_terminal(key, candidate) != detail::TerminalVerdict::chosen) {
                    std::fprintf(stderr,
                                 "sluice::async::UringAsyncBackend: stranded-completion "
                                 "settlement lost terminal authority (invariant violation)\n");
                    std::fflush(stderr);
                    std::terminate();
                }
                if (core_->release_execution(key) !=
                    detail::ExecutionRelease::borrow_touch_fully_retired) {
                    detail::uring_core_handoff_fail_fast();
                }
                route.terminal_delivered = true;
                publication_pending_->push_back(route.handle);
                bump(stats_, &AsyncStats::completion_errors);
            }
            if (route.terminal_delivered && route.control_state == RouterEntry::ControlState::none) {
                retire_router_entry_(router_index);
            }
        }
    }
#endif

    signal_ready_progress();
}

std::uint64_t UringAsyncBackend::allocate_cookie_() noexcept {
#if defined(SLUICE_B1C_MUTANT_COOKIE_REUSE)
    return 1;
#endif
    if (next_cookie_ == 0 || next_cookie_ >= CONTROL_TAG) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: operation-cookie "
                             "domain exhausted (would enter tagged control range / "
                             "wrap; invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    return next_cookie_++;
}

std::size_t UringAsyncBackend::find_live_router_index_(detail::SlotHandle h) const noexcept {
    for (std::size_t i = 0; i < router_.size(); ++i) {
        const RouterEntry& e = router_[i];
        if (e.in_use && e.handle.slot.value == h.slot.value &&
            e.handle.generation.value == h.generation.value) {
            return i;
        }
    }
    return router_.size();
}

std::size_t UringAsyncBackend::find_live_router_cookie_(std::uint64_t cookie) const noexcept {
    for (std::size_t i = router_.size(); i-- > 0;) {
        if (router_[i].in_use && router_[i].cookie == cookie)
            return i;
    }
    return router_.size();
}

void UringAsyncBackend::retire_router_entry_(std::size_t router_index) noexcept {
    if (router_index >= router_.size() || !router_[router_index].in_use) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: invalid router retirement "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    RouterEntry& entry = router_[router_index];
    if (entry.control_state != RouterEntry::ControlState::none) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: router retired with a live "
                             "control reference (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }
    entry = RouterEntry{};
    cookie_free_list_.push_back(detail::SlotIndex{static_cast<std::uint32_t>(router_index)});
    live_cookies_.fetch_sub(1, std::memory_order_relaxed);
}

detail::PublicCancel UringAsyncBackend::cancel_key(detail::RequestKey key) noexcept {
    detail::PublicCancel disposition;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);

        (void)dispatch_->remove_exact(detail::SlotHandle{key.slot, key.generation});
        disposition = core_->cancel(key);
        if (disposition == detail::PublicCancel::won_before_execution) {
            if (core_->release_execution(key) !=
                detail::ExecutionRelease::borrow_touch_fully_retired) {
                detail::uring_core_handoff_fail_fast();
            }
            publication_pending_->push_back(detail::SlotHandle{key.slot, key.generation});
        } else if (disposition == detail::PublicCancel::requested) {
            issue_running_cancel_locked_(detail::SlotHandle{key.slot, key.generation});
        }
    }
    if (disposition == detail::PublicCancel::won_before_execution) {
        bump(stats_, &AsyncStats::canceled_ops);
        signal_ready_progress();
    } else if (
#if defined(SLUICE_E1_MUTANT_CANCEL_PROGRESS_SIGNAL_DROPPED)
        false
#else
        disposition == detail::PublicCancel::requested
#endif
    ) {
        // A running cancel leaves actionable control state behind (a prepared
        // cancel SQE or a sticky intent), so the sleep handshake must observe
        // the obligation and come back to submit or service it.
        signal_ready_progress();
    }

    return disposition;
}

void UringAsyncBackend::issue_running_cancel_locked_(detail::SlotHandle h) noexcept {
    if (fatal_error_.has_value())
        return;

    const std::size_t idx = find_live_router_index_(h);
    if (idx == router_.size())
        return;
    RouterEntry& route = router_[idx];
    if (route.control_state != RouterEntry::ControlState::none) {
        route.control_pending = false;
        return;
    }

    const std::uint64_t target_cookie = route.cookie;
    if (target_cookie == 0 || target_cookie >= CONTROL_TAG) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: issue_running_cancel "
                             "found LIVE router entry with invalid cookie "
                             "(invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
    if (sqe == nullptr) {
        (void)submit_transport_locked();
        if (fatal_error_.has_value()) {
            return;
        }
        sqe = ::io_uring_get_sqe(&ring_state_->ring);
    }
    if (sqe == nullptr) {
        route.control_pending = sticky_intent_retained();
        return;
    }

#if !defined(SLUICE_B1C_MUTANT_REMOVE_CONTROL_PIN)
    const detail::RequestKey key{core_->context(), h.slot, h.generation};
    if (!core_->acquire_control(key)) {
        detail::uring_core_handoff_fail_fast();
    }
#endif
    ::io_uring_prep_cancel64(sqe, target_cookie, 0);
    ::io_uring_sqe_set_data64(sqe, make_control_cookie(target_cookie));
    route.control_state = RouterEntry::ControlState::prepared;
    route.control_pending = false;
    const auto& sq = ring_state_->ring.sq;
    const std::uint32_t physical_position =
        static_cast<std::uint32_t>((sq.sqe_tail - 1u) & sq.ring_mask);
    transport_ledger_->append(TransportLedger::Kind::cancel_control, physical_position,
                              target_cookie, h);
}

void UringAsyncBackend::service_pending_controls_locked_() noexcept {
    if (fatal_error_.has_value())
        return;
    for (std::size_t i = 0; i < router_.size(); ++i) {
        const RouterEntry& route = router_[i];
        if (route.in_use && route.control_state == RouterEntry::ControlState::none &&
            route.control_pending) {
            issue_running_cancel_locked_(route.handle);
        }
    }
}

#endif

}

