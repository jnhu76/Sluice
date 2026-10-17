#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using namespace sluice::async;

using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;

struct DriverClaim {
    std::optional<ProgressOwner> owner;
    explicit DriverClaim(AsyncIoContext& ctx) {
        if (auto claimed = ctx.claim_progress_owner(); claimed.has_value())
            owner = std::move(claimed).value();
    }
};

template <class Gate> void wait_gate_paused(Gate& gate) {
    std::atomic<bool>& paused = gate.paused;
    bool seen = paused.load(std::memory_order_acquire);
    while (!seen) {
        paused.wait(seen, std::memory_order_acquire);
        seen = paused.load(std::memory_order_acquire);
    }
}

template <class Gate> void resume_gate(Gate& gate) noexcept {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

std::optional<AsyncIoContext::ProgressWaitOutcome> wait_one_value(AsyncIoContext& ctx,
                                                                  std::chrono::nanoseconds bound) {
    const auto r = ctx.wait_one(bound);
    if (!r.has_value())
        return std::nullopt;
    return r.value();
}

// The discriminated interleaving: a zero-length read publishes inline on the
// submitting thread and leaves an owed delivery event plus a standing
// notification. Pausing the pass between its delivery sweep and its report
// makes "the sweep already missed the owed event" a deterministic state, not
// a scheduling race.
struct OwedResidueFixture {
    std::unique_ptr<UringAsyncBackend> backend = std::make_unique<UringAsyncBackend>();
    UringAsyncBackend* raw = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));
    FILE* backing = ::tmpfile();
    UringAsyncBackend::DeliverySweepEpiloguePauseGate gate{};

    bool arm() {
        if (backing == nullptr)
            return false;
        raw->set_delivery_sweep_epilogue_pause_gate(&gate);
        return true;
    }

    bool submit_zero_op(Request<std::size_t>& out) {
        auto submitted = ctx.submit_read(ReadOp{
            NativeFileRef(::fileno(backing), sluice::FileAccess::read_only), buffer.data(), 0, 0});
        if (!submitted.has_value())
            return false;
        out = std::move(submitted.value());
        return true;
    }

    void settle_gate() {
        raw->set_delivery_sweep_epilogue_pause_gate(nullptr);
        resume_gate(gate);
    }

    ~OwedResidueFixture() {
        if (backing != nullptr)
            ::fclose(backing);
    }

  private:
    std::vector<std::byte> buffer{4, std::byte{0}};
};

// Publication precedes the owed delivery, so request readiness cannot observe
// the undelivered state; the backend sink's delivery count is the oracle for
// the owed event itself.
bool drive_until_delivered(OwedResidueFixture& fx, std::size_t& delivered) {
    DriverClaim claim{fx.ctx};
    for (int i = 0; i < 2000 && fx.raw->sink_deliveries() == 0; ++i) {
        const auto pass = fx.ctx.poll_progress();
        if (!pass.has_value())
            return false;
        delivered += pass.value().completed;
        if (fx.raw->sink_deliveries() == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return fx.raw->sink_deliveries() == 1;
}

// The owner thread released its claim at join; the follow-up passes run under
// a fresh claim on the calling thread so a rejected drive cannot be mistaken
// for a residue verdict.
bool settle_pass_clean(OwedResidueFixture& fx) {
    DriverClaim claim{fx.ctx};
    const auto settle = fx.ctx.poll_progress();
    return settle.has_value() && settle.value().completed == 0 &&
           !settle.value().immediate_work_remains && fx.raw->internal_work_retired();
}

// Test A (residue accuracy): the pass whose sweep missed the owed event must
// still report it. An owed delivery is actionable by the very next pass with
// no external event in between, so it belongs in immediate_work_remains; a
// false verdict here is exactly the false-quiescence report that wait_one
// turns into a zero-completion progress outcome.
bool a_zero_op_after_sweep_is_immediate_residue() {
    OwedResidueFixture fx;
    if (!fx.arm())
        return false;
    Request<std::size_t> zero;

    std::optional<ProgressPass> observed;
    std::thread driver([&] {
        DriverClaim claim{fx.ctx};
        const auto r = fx.ctx.poll_progress();
        if (r.has_value())
            observed = r.value();
    });
    wait_gate_paused(fx.gate);
    const bool landed = fx.submit_zero_op(zero);
    fx.settle_gate();
    driver.join();

    const bool scenario = landed && observed.has_value() && observed->completed == 0 &&
                          !observed->accepted_work_remains && !observed->health_failed &&
                          zero.ready() && fx.raw->sink_deliveries() == 0;
    if (!scenario)
        return false;

    const bool residue_reported = observed->immediate_work_remains;
    if (!residue_reported) {
        std::fprintf(stderr,
                     "test a: owed delivery standing, immediate_work_remains observed as false\n");
        return false;
    }

    std::size_t delivered = 0;
    if (!drive_until_delivered(fx, delivered) || delivered != 1)
        return false;
    const auto observation = zero.try_result();
    const bool outcome = observation.readiness == RequestReadiness::ready &&
                         observation.result.has_value() && observation.result.value() == 0;
    const bool retired = settle_pass_clean(fx);
    zero.discard();
    return outcome && retired;
}

// Test B (public wait semantics): through wait_one, the same interleaving
// must not hand the caller a zero-completion progress verdict over an
// obligation that landed inside the wait's own pass. With the residue
// reported, the wait's token-changed continuation serves the owed delivery
// before returning.
bool b_wait_one_serves_owed_delivery_within_the_wait() {
    OwedResidueFixture fx;
    if (!fx.arm())
        return false;
    Request<std::size_t> zero;

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread owner([&] {
        DriverClaim claim{fx.ctx};
        result = wait_one_value(fx.ctx, std::chrono::milliseconds{8000});
    });
    wait_gate_paused(fx.gate);
    const bool landed = fx.submit_zero_op(zero);
    fx.settle_gate();
    owner.join();

    const bool served = landed && result.has_value() && result->kind == WaitKind::progress &&
                        result->completed == 1;
    const bool settled = settle_pass_clean(fx);
    const auto observation = zero.try_result();
    const bool outcome = zero.ready() && observation.readiness == RequestReadiness::ready &&
                         observation.result.has_value() && observation.result.value() == 0;
    zero.discard();
    return served && outcome && settled;
}

// Test C (progress preservation): independent of the report verdict, the
// obligation must survive the missed sweep — later passes deliver it exactly
// once, the control reference retires with the delivery, and the backend
// settles quiescent. This holds with the standing notification even when the
// report is wrong, which is why A and B, not C, are the discriminators
// between honest and false quiescence.
bool c_zero_op_after_sweep_still_delivers_exactly_once_and_retires() {
    OwedResidueFixture fx;
    if (!fx.arm())
        return false;
    Request<std::size_t> zero;

    std::optional<ProgressPass> racing;
    std::thread driver([&] {
        DriverClaim claim{fx.ctx};
        const auto r = fx.ctx.poll_progress();
        if (r.has_value())
            racing = r.value();
    });
    wait_gate_paused(fx.gate);
    const bool landed = fx.submit_zero_op(zero);
    fx.settle_gate();
    driver.join();

    const bool scenario = landed && racing.has_value() && racing->completed == 0 && zero.ready() &&
                          fx.raw->sink_deliveries() == 0;
    if (!scenario)
        return false;

    std::size_t delivered = 0;
    if (!drive_until_delivered(fx, delivered) || delivered != 1)
        return false;
    const auto observation = zero.try_result();
    const bool outcome = observation.readiness == RequestReadiness::ready &&
                         observation.result.has_value() && observation.result.value() == 0;
    const bool retired = settle_pass_clean(fx);
    zero.discard();
    return outcome && retired;
}

}

int main() {
    ::alarm(120);
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"a_zero_op_after_sweep_is_immediate_residue", a_zero_op_after_sweep_is_immediate_residue},
        {"b_wait_one_serves_owed_delivery_within_the_wait",
         b_wait_one_serves_owed_delivery_within_the_wait},
        {"c_zero_op_after_sweep_still_delivers_exactly_once_and_retires",
         c_zero_op_after_sweep_still_delivers_exactly_once_and_retires},
    };

    std::size_t passed = 0;
    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
        ++passed;
        std::printf("ok %s\n", t.name);
        std::fflush(stdout);
    }
    std::printf("all %zu uring owed-residue tests passed\n", passed);
    return 0;
}
