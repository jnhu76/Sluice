#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/test_access.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bench::r3 {

using sluice::async::AsyncIoContext;
using sluice::async::ReadOp;
using sluice::async::Request;
using sluice::async::UringAsyncBackend;
using sluice::IoError;

enum class Arm : std::uint8_t {
    a0 = 0,
    a1 = 1,
    b = 2,
    r = 3,
    s = 4,
    s_no_inflight = 5,
    s_no_pressure = 6,
    q2 = 7,
    q8 = 8,
    shadow = 9,
};

inline Arm arm_from_env() {
    const char* value = std::getenv("SLUICE_595_R3_ARM");
    if (value == nullptr)
        return Arm::a0;
    const auto is = [value](const char* token) {
        for (std::size_t i = 0; token[i] != '\0'; ++i) {
            if (value[i] != token[i])
                return false;
        }
        return value[std::char_traits<char>::length(token)] == '\0';
    };
    if (is("A0") || is("a0"))
        return Arm::a0;
    if (is("A1") || is("a1"))
        return Arm::a1;
    if (is("B") || is("b"))
        return Arm::b;
    if (is("R") || is("r"))
        return Arm::r;
    if (is("S") || is("s"))
        return Arm::s;
    if (is("SNI") || is("sni"))
        return Arm::s_no_inflight;
    if (is("SNP") || is("snp"))
        return Arm::s_no_pressure;
    if (is("Q2") || is("q2"))
        return Arm::q2;
    if (is("Q8") || is("q8"))
        return Arm::q8;
    if (is("SHADOW") || is("shadow"))
        return Arm::shadow;
    return Arm::a0;
}

inline const char* arm_name(Arm arm) {
    switch (arm) {
    case Arm::a1:
        return "A1";
    case Arm::b:
        return "B";
    case Arm::r:
        return "R";
    case Arm::s:
        return "S";
    case Arm::s_no_inflight:
        return "SNI";
    case Arm::s_no_pressure:
        return "SNP";
    case Arm::q2:
        return "Q2";
    case Arm::q8:
        return "Q8";
    case Arm::shadow:
        return "SHADOW";
    case Arm::a0:
    default:
        return "A0";
    }
}

inline bool research_cases_enabled() {
    const char* value = std::getenv("SLUICE_595_R3_CASES");
    return value != nullptr && value[0] != '0';
}

struct Counters {
    std::uint64_t entry_calls = 0;
    std::uint64_t group_calls = 0;
    std::uint64_t single_calls = 0;
    std::uint64_t accepted = 0;
    std::uint64_t prefix_max = 0;
    std::uint64_t suffix_single_calls = 0;
    std::uint64_t flush_attempts = 0;
    std::uint64_t ineligible_fallbacks = 0;
    std::uint64_t gate_stops = 0;
    std::uint64_t rejected_calls = 0;
    std::uint64_t untouched = 0;

    std::uint64_t r4_batch_presentations = 0;
    std::uint64_t r4_cap_reentries = 0;
    std::uint64_t r4_chunk_reentries = 0;
    std::uint64_t r4_immediate_decisions = 0;
    std::uint64_t r4_authority_exits = 0;
    std::uint64_t r4_quantum1 = 0;
    std::uint64_t r4_quantum2 = 0;
    std::uint64_t r4_quantum8 = 0;
    std::uint64_t r4_active_zero_decisions = 0;
    std::uint64_t r4_pressure_decisions = 0;
    std::uint64_t r4_shadow_decisions = 0;
    std::uint64_t r4_shadow_outstanding_sum = 0;
    std::uint64_t r4_shadow_window_sum = 0;
};

// Snapshot of the most recently observed owner progress pass, carried to
// the submit thread by the harness (its existing shared-mutex channel);
// the adapter itself never polls. owner_pass_sampled is a monotonic
// liveness witness (a pass was observed at least once), not freshness:
// the values may lag the submitter by any number of driver cycles, and
// last_pass_completed counts results the observed pass already published
// (work done), not completions waiting to be processed.
struct DecisionFacts {
    std::uint64_t last_pass_completed = 0;
    bool last_pass_immediate_work = false;
    bool owner_pass_sampled = false;
    // Caller-side unlocked mirror of accepted-but-uncollected requests;
    // observation for the shadow arm only, never a decision authority.
    std::size_t caller_window = 0;
};

struct BudgetVariant {
    bool read_inflight = false;
    bool read_owner_pass_activity = false;
    std::size_t pinned_quantum = 0;
    bool shadow = false;
};

struct ArmPolicy {
    bool reuse_on_cap = false;
    bool budgeted = false;
    BudgetVariant budget{};
};

inline ArmPolicy arm_policy(Arm arm) {
    switch (arm) {
    case Arm::r:
        return {true, false, {}};
    case Arm::shadow:
        return {true, true, {false, false, 0, true}};
    case Arm::s:
        return {true, true, {true, true, 0, false}};
    case Arm::s_no_inflight:
        return {true, true, {false, true, 0, false}};
    case Arm::s_no_pressure:
        return {true, true, {true, false, 0, false}};
    case Arm::q2:
        return {true, true, {false, false, 2, false}};
    case Arm::q8:
        return {true, true, {false, false, 8, false}};
    case Arm::b:
    case Arm::a0:
    case Arm::a1:
    default:
        return {false, false, {}};
    }
}

// Mirror of the frozen batch shape (UringAsyncBackend::batch_eligible_read):
// the adapter must know how far a span could batch so the budget can bound
// the presented prefix; the entry re-derives it authoritatively.
constexpr std::size_t kBatchShapeReadLen = 4096;

inline std::size_t batch_shaped_prefix(std::span<const ReadOp> ops) noexcept {
    std::size_t count = 0;
    while (count < ops.size() && ops[count].len == kBatchShapeReadLen &&
           ops[count].dst != nullptr) {
        ++count;
    }
    return count;
}

namespace budget {

enum class Action : std::uint8_t { batch, immediate, authority };
enum class QuantumEffect : std::uint8_t { none, base, clamp2 };

// Every quantum the table can emit (base 8, clamp 2, pinned 2 or 8) is
// <= ready_batch_cap, so an eligible prefix bounded to this value decides
// identically to the full-span scan. Raise it before pinning a larger
// quantum.
constexpr std::size_t kEligiblePrefixScanBound = UringAsyncBackend::ready_batch_cap;

struct Input {
    std::size_t remaining = 0;
    std::size_t eligible_prefix_bounded = 0;
    std::size_t active_data = 0;
    bool inflight_hint_active = false;
    bool owner_pass_activity_hint_active = false;
    bool owner_pass_had_activity = false;
    bool prior_stop_control_or_recovery = false;
    bool residue_advertised = false;
    std::size_t pinned_quantum = 0;
};

struct Row {
    const char* name;
    bool (*matches)(const Input&) noexcept;
    Action action;
    QuantumEffect quantum;
};

constexpr std::array<Row, 6> kTable{{
    {"remaining_single",
     [](const Input& in) noexcept { return in.remaining == 1; }, Action::immediate,
     QuantumEffect::none},
    {"front_ineligible",
     [](const Input& in) noexcept { return in.eligible_prefix_bounded == 0; },
     Action::immediate, QuantumEffect::none},
    {"authority_signal",
     [](const Input& in) noexcept {
         return in.prior_stop_control_or_recovery || in.residue_advertised;
     },
     Action::authority, QuantumEffect::none},
    {"quantum_base", [](const Input&) noexcept { return true; }, Action::batch,
     QuantumEffect::base},
    {"inflight_zero",
     [](const Input& in) noexcept {
         return in.inflight_hint_active && in.active_data == 0;
     },
     Action::batch, QuantumEffect::clamp2},
    {"owner_pass_activity",
     [](const Input& in) noexcept {
         return in.owner_pass_activity_hint_active && in.owner_pass_had_activity;
     },
     Action::batch, QuantumEffect::clamp2},
}};

struct Decision {
    Action action = Action::batch;
    std::size_t quantum = 8;
    const char* row = "quantum_base";
};

inline Decision evaluate(const Input& in) noexcept {
    Decision decision;
    const char* quantum_row = nullptr;
    for (const Row& row : kTable) {
        if (!row.matches(in)) {
            continue;
        }
        if (decision.action == Action::batch && row.action != Action::batch) {
            decision.action = row.action;
            decision.row = row.name;
        }
        switch (row.quantum) {
        case QuantumEffect::base:
            decision.quantum = 8;
            quantum_row = row.name;
            break;
        case QuantumEffect::clamp2:
            decision.quantum = std::min<std::size_t>(decision.quantum, 2);
            quantum_row = row.name;
            break;
        case QuantumEffect::none:
            break;
        }
    }
    if (decision.action == Action::batch) {
        if (in.pinned_quantum != 0) {
            decision.quantum = in.pinned_quantum;
            decision.row = "quantum_pinned";
        } else if (quantum_row != nullptr) {
            decision.row = quantum_row;
        }
    } else if (decision.action == Action::immediate) {
        decision.quantum = 1;
    } else {
        decision.quantum = 0;
    }
    return decision;
}

} // namespace budget

// Equals min(batch_shaped_prefix(ops), budget::kEligiblePrefixScanBound): a
// stop before the bound is exactly where the full scan stops.
inline std::size_t batch_shaped_prefix_bounded(std::span<const ReadOp> ops) noexcept {
    const std::size_t limit = std::min(ops.size(), budget::kEligiblePrefixScanBound);
    std::size_t count = 0;
    while (count < limit && ops[count].len == kBatchShapeReadLen &&
           ops[count].dst != nullptr) {
        ++count;
    }
    return count;
}

struct DecisionRecord {
    std::size_t remaining = 0;
    std::size_t eligible_prefix = 0;
    std::size_t present_len = 0;
    std::size_t quantum = 0;
    const char* action_row = "";
    bool active_read = false;
    std::size_t active_data = 0;
    bool owner_pass_activity_read = false;
    bool owner_pass_had_activity = false;
    bool owner_pass_sampled = false;
    std::uint64_t last_pass_completed = 0;
    bool last_pass_immediate_work = false;
    std::size_t caller_window = 0;
    std::size_t accepted = 0;
    std::size_t untouched_begin = 0;
    std::size_t untouched = 0;
    UringAsyncBackend::ReadyBatchStop stop = UringAsyncBackend::ReadyBatchStop::input_exhausted;
    std::size_t flush_attempts = 0;
    bool residue_advertised = false;
    bool cap_cause = false;
    bool chunk_cause = false;
};

inline const char* stop_name(UringAsyncBackend::ReadyBatchStop stop) noexcept {
    switch (stop) {
    case UringAsyncBackend::ReadyBatchStop::input_exhausted:
        return "input_exhausted";
    case UringAsyncBackend::ReadyBatchStop::rejected:
        return "rejected";
    case UringAsyncBackend::ReadyBatchStop::cap:
        return "cap";
    case UringAsyncBackend::ReadyBatchStop::sq_budget:
        return "sq_budget";
    case UringAsyncBackend::ReadyBatchStop::control:
        return "control";
    case UringAsyncBackend::ReadyBatchStop::recovery:
        return "recovery";
    case UringAsyncBackend::ReadyBatchStop::ineligible:
        return "ineligible";
    }
    return "unknown";
}

inline void format_decision_record(const DecisionRecord& r, std::string& out) {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "ready=%zu eligible=%zu present=%zu quantum=%zu row=%s "
                  "active_read=%d active=%zu owner_pass_activity_read=%d "
                  "owner_pass_had_activity=%d owner_pass_sampled=%d "
                  "pass_completed=%llu pass_immediate=%d "
                  "caller_window=%zu accepted=%zu stop=%s untouched_begin=%zu "
                  "untouched=%zu flush=%zu residue=%d sq_budget_bound=%d "
                  "cause=%s",
                  r.remaining, r.eligible_prefix, r.present_len, r.quantum, r.action_row,
                  r.active_read ? 1 : 0, r.active_data,
                  r.owner_pass_activity_read ? 1 : 0,
                  r.owner_pass_had_activity ? 1 : 0, r.owner_pass_sampled ? 1 : 0,
                  static_cast<unsigned long long>(r.last_pass_completed),
                  r.last_pass_immediate_work ? 1 : 0, r.caller_window, r.accepted,
                  stop_name(r.stop), r.untouched_begin, r.untouched, r.flush_attempts,
                  r.residue_advertised ? 1 : 0,
                  r.stop == UringAsyncBackend::ReadyBatchStop::sq_budget ? 1 : 0,
                  r.cap_cause ? "cap_reentry" : (r.chunk_cause ? "chunk" : "first"));
    out = buf;
}

class Adapter {
  public:
    Adapter(AsyncIoContext& ctx, Arm arm) : ctx_(ctx), arm_(arm) {
        backend_ = dynamic_cast<UringAsyncBackend*>(
            sluice::async::detail::TestAccess::context_backend(ctx));
    }

    Arm arm() const noexcept { return arm_; }

    bool batching() const noexcept {
        return arm_ != Arm::a0 && arm_ != Arm::a1 && backend_ != nullptr;
    }

    bool budgeted() const noexcept { return arm_policy(arm_).budgeted; }

    // True when the arm consumes the pass-fact channel; the harness consults
    // this so frozen arms never pay for the hint read.
    bool reads_owner_pass_facts() const noexcept {
        const BudgetVariant budget = arm_policy(arm_).budget;
        return budget.read_owner_pass_activity || budget.shadow;
    }

    struct TraceSink {
        std::vector<DecisionRecord> records;

        std::size_t size() const noexcept { return records.size(); }
        const DecisionRecord& operator[](std::size_t index) const noexcept {
            return records[index];
        }
        std::vector<DecisionRecord>::const_iterator begin() const noexcept {
            return records.begin();
        }
        std::vector<DecisionRecord>::const_iterator end() const noexcept {
            return records.end();
        }
    };

    void set_trace_sink(TraceSink* sink) noexcept { trace_ = sink; }

    struct Outcome {
        std::optional<IoError> error;
        Counters counters;
    };

    Outcome submit(std::span<const ReadOp> ready,
                   std::vector<Request<std::size_t>>& accepted) {
        return submit(ready, accepted, DecisionFacts{});
    }

    Outcome submit(std::span<const ReadOp> ready, std::vector<Request<std::size_t>>& accepted,
                   const DecisionFacts& facts) {
        Outcome out;
        accepted.clear();
        if (arm_ == Arm::a0 || backend_ == nullptr) {
            submit_context_singles(ready, accepted, out);
            return out;
        }

        if (arm_ == Arm::a1) {
            for (const ReadOp& op : ready) {
                auto single =
                    sluice::async::detail::TestAccess::context_submit_ready_reads(
                        ctx_, std::span<const ReadOp>(&op, 1));
                account(single, 1, out);
                append(single, accepted);
                if (single.accepted != 1) {
                    out.error = single.error;
                    ++out.counters.rejected_calls;
                    break;
                }
            }
            return out;
        }

        // The entry reports untouched members as a tail of the presented span,
        // so the accepted prefix plus the skipped rejected member never
        // overlaps the re-entry region.
        const ArmPolicy policy = arm_policy(arm_);
        const BudgetVariant variant = policy.budget;
        std::size_t begin = 0;
        bool prior_control_or_recovery = false;
        bool prior_residue = false;
        bool prior_cap_cause = false;
        std::optional<DecisionRecord> pending_record;
        while (begin < ready.size()) {
            const std::span<const ReadOp> rest = ready.subspan(begin);
            std::size_t present_len = rest.size();
            bool authority_exit = false;
            DecisionRecord record;
            record.remaining = rest.size();
            if (trace_ != nullptr) {
                record.eligible_prefix = batch_shaped_prefix(rest);
            }
            record.owner_pass_sampled = facts.owner_pass_sampled;
            record.last_pass_completed = facts.last_pass_completed;
            record.last_pass_immediate_work = facts.last_pass_immediate_work;
            record.caller_window = facts.caller_window;
            record.cap_cause = prior_cap_cause;
            record.chunk_cause = !prior_cap_cause && begin != 0;
            if (policy.budgeted) {
                budget::Input input;
                input.remaining = rest.size();
                input.eligible_prefix_bounded = batch_shaped_prefix_bounded(rest);
                input.prior_stop_control_or_recovery = prior_control_or_recovery;
                input.residue_advertised = prior_residue;
                input.pinned_quantum = variant.pinned_quantum;
                // Shadow evaluates the full state-aware table (its logged
                // quantum is the counterfactual S decision) and then overrides
                // only the presentation.
                input.inflight_hint_active = variant.read_inflight || variant.shadow;
                input.owner_pass_activity_hint_active =
                    variant.read_owner_pass_activity || variant.shadow;
                if (variant.read_inflight || variant.shadow) {
                    record.active_read = true;
                    record.active_data = ctx_.outstanding();
                    input.active_data = record.active_data;
                }
                if (variant.read_owner_pass_activity || variant.shadow) {
                    record.owner_pass_activity_read = true;
                    record.owner_pass_had_activity =
                        facts.owner_pass_sampled &&
                        (facts.last_pass_completed > 0 ||
                         facts.last_pass_immediate_work);
                    input.owner_pass_had_activity = record.owner_pass_had_activity;
                }
                const budget::Decision decision = budget::evaluate(input);
                record.quantum = decision.quantum;
                record.action_row = decision.row;
                if (variant.shadow) {
                    ++out.counters.r4_shadow_decisions;
                    out.counters.r4_shadow_outstanding_sum += record.active_data;
                    out.counters.r4_shadow_window_sum += facts.caller_window;
                } else {
                    switch (decision.action) {
                    case budget::Action::immediate:
                        present_len = 1;
                        ++out.counters.r4_immediate_decisions;
                        break;
                    case budget::Action::authority:
                        authority_exit = true;
                        ++out.counters.r4_authority_exits;
                        break;
                    case budget::Action::batch:
                        present_len =
                            std::min(decision.quantum, input.eligible_prefix_bounded);
                        if (present_len == 0)
                            present_len = 1;
                        break;
                    }
                }
                if (record.active_read && record.active_data == 0) {
                    ++out.counters.r4_active_zero_decisions;
                }
                if (record.owner_pass_activity_read && record.owner_pass_had_activity) {
                    ++out.counters.r4_pressure_decisions;
                }
                if (decision.quantum == 1) {
                    ++out.counters.r4_quantum1;
                } else if (decision.quantum == 2) {
                    ++out.counters.r4_quantum2;
                } else {
                    ++out.counters.r4_quantum8;
                }
                record.present_len = present_len;
                pending_record = record;
                if (authority_exit) {
                    break;
                }
            } else {
                record.action_row = "full_span";
                record.present_len = present_len;
                pending_record = record;
            }

            auto batch =
                sluice::async::detail::TestAccess::context_submit_ready_reads(
                    ctx_, rest.first(present_len));
            if (!batch.batch_backend) {
                submit_context_singles(rest, accepted, out);
                return out;
            }
            account(batch, present_len, out);
            append(batch, accepted);
            ++out.counters.r4_batch_presentations;
            if (trace_ != nullptr && pending_record.has_value()) {
                pending_record->accepted = batch.accepted;
                pending_record->untouched_begin = batch.untouched_begin;
                pending_record->untouched = batch.untouched;
                pending_record->stop = batch.stop;
                pending_record->flush_attempts = batch.flush_attempts;
                pending_record->residue_advertised = batch.residue_advertised;
                trace_->records.push_back(*pending_record);
            }
            pending_record.reset();
            if (batch.stop == UringAsyncBackend::ReadyBatchStop::rejected) {
                out.error = batch.error;
                ++out.counters.rejected_calls;
                return out;
            }
            begin += batch.untouched_begin;
            prior_control_or_recovery =
                batch.stop == UringAsyncBackend::ReadyBatchStop::control ||
                batch.stop == UringAsyncBackend::ReadyBatchStop::recovery;
            prior_residue = batch.residue_advertised;
            prior_cap_cause = batch.stop == UringAsyncBackend::ReadyBatchStop::cap &&
                              batch.accepted != 0;
            const bool progressed = batch.accepted != 0;
            const bool more = begin < ready.size();
            const bool cap_reuse =
                policy.reuse_on_cap && progressed &&
                batch.stop == UringAsyncBackend::ReadyBatchStop::cap && more;
            const bool chunk_continue = policy.budgeted && !variant.shadow && progressed &&
                                        batch.stop ==
                                            UringAsyncBackend::ReadyBatchStop::input_exhausted &&
                                        more;
            if (cap_reuse) {
                ++out.counters.r4_cap_reentries;
            } else if (chunk_continue) {
                ++out.counters.r4_chunk_reentries;
            }
            if (!cap_reuse && !chunk_continue) {
                break;
            }
        }

        for (std::size_t index = begin; index < ready.size(); ++index) {
            const ReadOp& op = ready[index];
            auto single = sluice::async::detail::TestAccess::context_submit_ready_reads(
                ctx_, std::span<const ReadOp>(&op, 1));
            account(single, 1, out);
            ++out.counters.suffix_single_calls;
            append(single, accepted);
            if (single.accepted != 1) {
                out.error = single.error;
                ++out.counters.rejected_calls;
                break;
            }
        }
        return out;
    }

  private:
    void submit_context_singles(std::span<const ReadOp> ready,
                                std::vector<Request<std::size_t>>& accepted, Outcome& out) {
        for (const ReadOp& op : ready) {
            auto submitted = ctx_.submit_read(op);
            if (!submitted.has_value()) {
                out.error = submitted.error();
                break;
            }
            accepted.push_back(std::move(submitted).value());
        }
    }

    void account(const sluice::async::detail::TestAccess::ReadyReadsBatch& batch,
                 std::size_t group_size, Outcome& out) {
        ++out.counters.entry_calls;
        if (group_size > 1)
            ++out.counters.group_calls;
        else
            ++out.counters.single_calls;
        out.counters.accepted += batch.accepted;
        out.counters.untouched += batch.untouched;
        out.counters.flush_attempts += batch.flush_attempts;
        if (batch.accepted > out.counters.prefix_max)
            out.counters.prefix_max = batch.accepted;
        if (batch.stop == UringAsyncBackend::ReadyBatchStop::ineligible) {
            ++out.counters.ineligible_fallbacks;
        } else if (batch.stop == UringAsyncBackend::ReadyBatchStop::control ||
                   batch.stop == UringAsyncBackend::ReadyBatchStop::recovery ||
                   batch.stop == UringAsyncBackend::ReadyBatchStop::sq_budget) {
            ++out.counters.gate_stops;
        }
    }

    static void append(sluice::async::detail::TestAccess::ReadyReadsBatch& batch,
                       std::vector<Request<std::size_t>>& accepted) {
        for (std::size_t index = 0; index < batch.accepted; ++index)
            accepted.push_back(std::move(batch.requests[index]));
    }

    AsyncIoContext& ctx_;
    Arm arm_ = Arm::a0;
    UringAsyncBackend* backend_ = nullptr;
    TraceSink* trace_ = nullptr;
};

}
