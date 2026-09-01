// F01 ablation seam regression (#260 seam + #261 production gate).
//
// After #261 made the stats-gated evaluation the PRODUCTION behavior, this
// test pins that the internal-testing seam keeps BOTH TAX-0D F01 arms
// reproducible in the sluice_async_internal_testing build:
//
//   R0 (flag unset) — backend_->outstanding() evaluated unconditionally,
//                     including stats == nullptr (the pre-#261 baseline the
//                     TAX-0D A/B measurement used)
//   R1 (flag set)   — evaluation skipped when stats == nullptr (identical
//                     to production since #261); with stats present the
//                     accounting is unchanged
//
// The ablation modes struct is process-global state; every case saves and
// restores the flag via a scoped guard so one failing case cannot poison
// the others. F02 (reap-seq stamp) has its own research-only limitation and
// is not exercised here.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include "tax0_ablation_seams.hpp"

#include <cstddef>
#include <memory>
#include <utility>

using namespace sluice::async;

namespace {

// outstanding() call counter on the documented fake test vehicle; the
// override is the observation point for whether the F01 site evaluated the
// argument.
class OutstandingCountingFake final : public FakeAsyncBackend {
public:
    std::size_t outstanding() const noexcept override {
        ++outstanding_calls_;
        return FakeAsyncBackend::outstanding();
    }
    std::size_t outstanding_calls() const noexcept { return outstanding_calls_; }

private:
    mutable std::size_t outstanding_calls_ = 0;
};

// RAII save/restore for the process-global ablation flag.
class F01FlagGuard {
public:
    F01FlagGuard() noexcept
        : saved_(sluice::async::detail::tax0_ablation_modes()
                     .f01_gate_outstanding_eval) {}
    ~F01FlagGuard() {
        sluice::async::detail::tax0_ablation_modes().f01_gate_outstanding_eval =
            saved_;
    }
    F01FlagGuard(const F01FlagGuard&) = delete;
    F01FlagGuard& operator=(const F01FlagGuard&) = delete;

private:
    bool saved_;
};

}  // namespace

SLUICE_TEST_CASE(tax0_f01_r0_unconditional_with_stats_disabled) {
    F01FlagGuard guard;
    auto& modes = sluice::async::detail::tax0_ablation_modes();
    modes.f01_gate_outstanding_eval = false;  // R0

    auto backend = std::make_unique<OutstandingCountingFake>();
    OutstandingCountingFake* raw = backend.get();
    AsyncIoContext ctx(std::move(backend), nullptr);  // stats disabled

    std::byte b[8]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b, 8, 0}, c).has_value());
    // R0 evaluates the argument even though stats cannot observe it — this
    // is what the TAX-0D A/B measured against the R1 arm.
    SLUICE_CHECK(raw->outstanding_calls() == 1);

    raw->complete_oldest_with_bytes(8);
    SLUICE_CHECK(ctx.poll() == 1);
    c.reset();
}

SLUICE_TEST_CASE(tax0_f01_r1_gated_with_stats_disabled) {
    F01FlagGuard guard;
    auto& modes = sluice::async::detail::tax0_ablation_modes();
    modes.f01_gate_outstanding_eval = true;  // R1

    auto backend = std::make_unique<OutstandingCountingFake>();
    OutstandingCountingFake* raw = backend.get();
    AsyncIoContext ctx(std::move(backend), nullptr);  // stats disabled

    std::byte b[8]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b, 8, 0}, c).has_value());
    // R1 skips the evaluation when stats == nullptr — the arm that became
    // production behavior in #261.
    SLUICE_CHECK(raw->outstanding_calls() == 0);

    raw->complete_oldest_with_bytes(8);
    SLUICE_CHECK(ctx.poll() == 1);
    c.reset();
}

SLUICE_TEST_CASE(tax0_f01_r1_accounting_intact_with_stats_enabled) {
    F01FlagGuard guard;
    auto& modes = sluice::async::detail::tax0_ablation_modes();
    modes.f01_gate_outstanding_eval = true;  // R1

    auto backend = std::make_unique<OutstandingCountingFake>();
    OutstandingCountingFake* raw = backend.get();
    sluice::AsyncStats stats{};
    AsyncIoContext ctx(std::move(backend), &stats);

    std::byte b[8]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx.submit_read(ReadOp{0, b, 8, 0}, c).has_value());
    // The gate keys on stats absence only: with stats attached the sampling
    // and the accounting are exactly as in R0/production.
    SLUICE_CHECK(raw->outstanding_calls() == 1);
    SLUICE_CHECK(stats.max_outstanding == 1);

    raw->complete_oldest_with_bytes(8);
    SLUICE_CHECK(ctx.poll() == 1);
    c.reset();
}

SLUICE_MAIN()
