#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/test_access.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/error.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bench::r3 {

using sluice::async::AsyncIoContext;
using sluice::async::ReadOp;
using sluice::async::Request;
using sluice::async::UringAsyncBackend;
using sluice::IoError;

enum class Arm : std::uint8_t { a0 = 0, a1 = 1, b = 2 };

inline Arm arm_from_env() {
    const char* value = std::getenv("SLUICE_595_R3_ARM");
    if (value == nullptr)
        return Arm::a0;
    if ((value[0] == 'A' || value[0] == 'a') && value[1] == '1')
        return Arm::a1;
    if (value[0] == 'B' || value[0] == 'b')
        return Arm::b;
    return Arm::a0;
}

inline const char* arm_name(Arm arm) {
    switch (arm) {
    case Arm::a1:
        return "A1";
    case Arm::b:
        return "B";
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
};

class Adapter {
  public:
    Adapter(AsyncIoContext& ctx, Arm arm) : ctx_(ctx), arm_(arm) {
        backend_ = dynamic_cast<UringAsyncBackend*>(
            sluice::async::detail::TestAccess::context_backend(ctx));
    }

    Arm arm() const noexcept { return arm_; }

    bool batching() const noexcept { return arm_ == Arm::b && backend_ != nullptr; }

    struct Outcome {
        std::optional<IoError> error;
        Counters counters;
    };

    Outcome submit(std::span<const ReadOp> ready,
                   std::vector<Request<std::size_t>>& accepted) {
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

        auto batch = sluice::async::detail::TestAccess::context_submit_ready_reads(ctx_, ready);
        if (!batch.batch_backend) {
            submit_context_singles(ready, accepted, out);
            return out;
        }
        account(batch, ready.size(), out);
        append(batch, accepted);
        if (batch.stop == UringAsyncBackend::ReadyBatchStop::rejected) {
            out.error = batch.error;
            ++out.counters.rejected_calls;
            return out;
        }
        for (std::size_t index = batch.untouched_begin; index < ready.size(); ++index) {
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
};

}
