// AUD-002 regression: the admission probe in submit_request must observe
// backend poison under the lock domain that owns fatal_error_. The shutdown
// driver's progress pass runs without the context access mutex, so its poison
// can overlap another thread's admission; an unlocked probe is a data race on
// a non-trivial std::optional<IoError> and can return a torn public submit
// error.
//
// The schedule is deterministic: setup leaves one prepared-only transport
// entry behind, the shutdown driver parks inside the transport attempt just
// before the poison write, the submitter parks at the submit-entry gate while
// holding access_mtx_, and both are then released together. The probe read
// and the poison write are unordered, so the TSan build reports the race
// against the unlocked probe and is silent once the probe takes
// dispatch_mtx_. The sequenced case pins the public semantics: a poisoned
// backend reports exactly the recorded health error to later admission.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <liburing.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::AsyncStats;
using sluice::File;
using sluice::IoError;
using sluice::Result;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::string make_temp_file() {
    char path[] = "/tmp/sluice_aud002_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    const char content[] = "sluice poison admission probe\n";
    const ssize_t n = ::write(fd, content, sizeof(content) - 1);
    if (n != static_cast<ssize_t>(sizeof(content) - 1)) {
        ::close(fd);
        return {};
    }
    ::close(fd);
    return path;
}

// The first submit call reports nothing submitted while leaving the prepared
// hand-off intact (an unconsumed transport entry). Later calls park just
// before the hard transport failure that triggers poison, so the caller can
// place the submitter while the poison write is still ahead.
struct GatedPoisonSubmit {
    std::atomic<int> calls{0};
    std::atomic<bool> armed{false};
    std::atomic<bool> release{false};
};

int gated_failing_submit(void* context, ::io_uring* ring) noexcept {
    (void)ring;
    auto* state = static_cast<GatedPoisonSubmit*>(context);
    const int call = state->calls.fetch_add(1, std::memory_order_relaxed);
    if (call == 0)
        return 0;
    state->armed.store(true, std::memory_order_release);
    state->armed.notify_all();
    while (!state->release.load(std::memory_order_acquire))
        std::this_thread::yield();
    return -EIO;
}

struct Fixture {
    std::unique_ptr<UringAsyncBackend> backend;
    std::unique_ptr<AsyncIoContext> ctx;
    AsyncStats stats;
    std::string path;

    // Returns false when the io_uring profile is unavailable on this host.
    bool create(GatedPoisonSubmit& submit_state, UringAsyncBackend** raw_out) {
        path = make_temp_file();
        if (path.empty())
            return false;
        UringBackendSubmitTestHooks hooks;
        hooks.submit = gated_failing_submit;
        hooks.context = &submit_state;
        try {
            backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 8}, hooks);
        } catch (...) {
            return false;
        }
        if (!backend->available())
            return false;
        UringAsyncBackend* raw = backend.get();
        ctx = std::make_unique<AsyncIoContext>(detail::make_context(std::move(backend), &stats));
        *raw_out = raw;
        return true;
    }

    ~Fixture() { ::unlink(path.c_str()); }
};

bool wait_flag(const std::atomic<bool>& v) {
    for (int i = 0; i < 20000000 && !v.load(std::memory_order_acquire); ++i)
        std::this_thread::yield();
    return v.load(std::memory_order_acquire);
}

void wait_gate_paused(UringAsyncBackend::SubmitEntryPauseGate& gate) {
    while (!gate.paused.load(std::memory_order_acquire))
        std::this_thread::yield();
}

void resume_gate(UringAsyncBackend::SubmitEntryPauseGate& gate) {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

// Case 1: the race schedule. The TSan verdict is the oracle; the functional
// assertions only pin that the interleaved admission is refused and the
// abandoned transport settles through the poison terminal.
bool admission_probe_unordered_with_owner_poison() {
    GatedPoisonSubmit submit_state;
    Fixture fixture;
    UringAsyncBackend* raw = nullptr;
    if (!fixture.create(submit_state, &raw))
        return true;
    AsyncIoContext& ctx = *fixture.ctx;

    File file = std::move(File::open(fixture.path, sluice::FileOpen{}).value());
    std::vector<std::byte> buffer_a(4, std::byte{0});
    std::vector<std::byte> buffer_b(4, std::byte{0});

    auto submitted_a = ctx.submit_read(ReadOp{NativeFileRef(file), buffer_a.data(), 4, 0});
    check(submitted_a.has_value(), "setup submit A succeeds");
    if (!submitted_a.has_value())
        return false;
    Request<std::size_t> request_a = std::move(submitted_a).value();
    check(raw->transport_ledger_size_for_test() == 1 && raw->sq_ready_for_test() == 1,
          "A holds the prepared-only transport entry");

    UringAsyncBackend::SubmitEntryPauseGate gate;
    raw->set_submit_entry_pause_gate(&gate);
    UringAsyncBackend::SubmitStageFailureInjection injection;
    injection.fail_reserve.store(true, std::memory_order_release);
    raw->set_submit_stage_failure_injection(&injection);

    // The shutdown driver poisons from its first progress pass; its passes
    // hold no access mutex, so its poison write and another thread's probe
    // can interleave. The transport hook parks it just before the write.
    std::thread driver([&] {
        const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
        check(outcome.has_value(), "the shutdown driver returns an outcome");
    });
    check(wait_flag(submit_state.armed), "the driver reaches the poison-preceding submit");

    std::optional<IoError> submit_error;
    std::thread submitter([&] {
        auto submitted = ctx.submit_read(ReadOp{NativeFileRef(file), buffer_b.data(), 4, 0});
        if (!submitted.has_value())
            submit_error = submitted.error();
    });
    wait_gate_paused(gate);

    // Release the driver and the submitter together. Do not depend on the
    // driver's completion here: releasing the gate must not carry the poison
    // write into the probe on either side of the gate.
    submit_state.release.store(true, std::memory_order_release);
    resume_gate(gate);

    submitter.join();
    driver.join();
    raw->set_submit_entry_pause_gate(nullptr);
    raw->set_submit_stage_failure_injection(nullptr);

    check(submit_error.has_value(), "the interleaved admission is refused");
    check(raw->submit_flushes_for_test() == 2,
          "setup dispatch and the driver pass each reach the transport");

    check(request_a.ready(), "the abandoned transport settles through poison");
    const auto observed = request_a.try_result();
    check(!observed.result.has_value() &&
              observed.result.error().code == IoError::Code::backend_error &&
              observed.result.error().os_errno == EIO,
          "the settled terminal carries the recorded transport failure");
    check(fixture.stats.completion_errors == 1,
          "poison retirement counts the accepted operation once");
    (void)request_a.take_result();
    return true;
}

// Case 2: sequenced public semantics. Once poison is visible, admission must
// report exactly the recorded health error and never fabricate a usable
// request.
bool poisoned_admission_reports_the_recorded_error() {
    GatedPoisonSubmit submit_state;
    submit_state.release.store(true, std::memory_order_release);
    Fixture fixture;
    UringAsyncBackend* raw = nullptr;
    if (!fixture.create(submit_state, &raw))
        return true;
    AsyncIoContext& ctx = *fixture.ctx;

    File file = std::move(File::open(fixture.path, sluice::FileOpen{}).value());
    std::vector<std::byte> buffer_a(4, std::byte{0});
    std::vector<std::byte> buffer_b(4, std::byte{0});

    auto submitted_a = ctx.submit_read(ReadOp{NativeFileRef(file), buffer_a.data(), 4, 0});
    check(submitted_a.has_value(), "setup submit A succeeds");
    if (!submitted_a.has_value())
        return false;
    Request<std::size_t> request_a = std::move(submitted_a).value();

    {
        std::thread owner([&] {
            auto owner_token = ctx.claim_progress_owner();
            check(owner_token.has_value(), "the owner claims the progress domain");
            const auto pass = ctx.poll();
            check(pass.has_value(), "the owner poll poisons the transport");
        });
        owner.join();
    }

    auto submitted_b = ctx.submit_read(ReadOp{NativeFileRef(file), buffer_b.data(), 4, 0});
    check(!submitted_b.has_value(), "admission after poison is refused");
    if (!submitted_b.has_value()) {
        check(submitted_b.error().code == IoError::Code::backend_error &&
                  submitted_b.error().os_errno == EIO,
              "the refusal carries the recorded health error verbatim");
    }

    check(request_a.ready(), "the abandoned transport settles through poison");
    const auto observed = request_a.try_result();
    check(!observed.result.has_value() &&
              observed.result.error().code == IoError::Code::backend_error &&
              observed.result.error().os_errno == EIO,
          "the settled terminal carries the recorded transport failure");
    (void)request_a.take_result();
    return true;
}

} // namespace

int main() {
    (void)admission_probe_unordered_with_owner_poison();
    (void)poisoned_admission_reports_the_recorded_error();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("PASS: uring submit/poison admission probe\n");
    return 0;
}
