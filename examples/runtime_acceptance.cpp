// runtime_acceptance — public-only acceptance consumer for ApplicationRuntime.
//
// Exercises the full lifecycle against INSTALLED/PUBLIC headers only:
//   - RuntimeBuilder validation (no backend → error)
//   - RuntimeBuilder::build() → Result<unique_ptr<ApplicationRuntime>>
//   - start() → Running
//   - submit() a task that observes CancelToken
//   - request_stop() → cooperative cancellation
//   - drain() → all tasks complete
//   - join() → Stopped, resources released
//   - shutdown() idempotent post-Stopped
//
// Negative constraints:
//   - no tests/ include path
//   - no SLUICE_ASYNC_INTERNAL_TESTING
//   - no private source inclusion
//
// This is NOT an application — it is a compile + run acceptance that the
// public E16 ApplicationRuntime surface is usable end-to-end from a consumer
// that sees only the installed headers.
#include <sluice/async/application_runtime.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int fail(const std::string& msg) {
    std::fprintf(stderr, "E16 ACCEPTANCE FAIL: %s\n", msg.c_str());
    return 1;
}

}  // namespace

int main() {
    using namespace sluice::async;
    using sluice::IoError;

    // ---- Builder validation: no backend → invalid_state ----
    {
        RuntimeBuilder builder;
        auto r = builder.build();
        if (r.has_value()) return fail("build() without backend should fail");
        if (r.error().code != IoError::Code::invalid_state)
            return fail("build() error code should be invalid_state");
    }

    // ---- Full lifecycle: build → start → submit → stop → drain → join ----
    {
        auto backend = std::make_unique<FakeAsyncBackend>();
        RuntimeBuilder builder;
        builder.backend(std::move(backend)).workers(1);

        auto build_r = builder.build();
        if (!build_r.has_value()) return fail("build() with backend should succeed");
        auto& rt = *build_r.value();

        // start
        auto sr = rt.start();
        if (!sr.has_value()) return fail("start()");

        // submit a task that increments a counter
        std::atomic<int> task_ran{0};
        auto sub_r = rt.submit([&task_ran](RuntimeTaskContext& tctx) {
            // Observe cancel token is accessible
            (void)tctx.cancel_token();
            task_ran.fetch_add(1, std::memory_order::relaxed);
        });
        if (!sub_r.has_value()) return fail("submit()");

        // request_stop
        rt.request_stop();

        // submit after stop → rejected
        auto sub2 = rt.submit([](RuntimeTaskContext&) {});
        if (sub2.has_value()) return fail("submit() after stop should fail");

        // drain
        auto dr = rt.drain();
        if (!dr.has_value()) return fail("drain()");
        if (task_ran.load(std::memory_order::acquire) != 1)
            return fail("task should have executed exactly once");

        // join
        auto jr = rt.join();
        if (!jr.has_value()) return fail("join()");

        // shutdown is idempotent post-Stopped
        auto sh = rt.shutdown();
        if (!sh.has_value()) return fail("shutdown() post-Stopped");
    }

    // ---- shutdown() from Constructed (no start) ----
    {
        auto backend = std::make_unique<FakeAsyncBackend>();
        RuntimeBuilder builder;
        builder.backend(std::move(backend));
        auto build_r = builder.build();
        if (!build_r.has_value()) return fail("build() #2");
        auto& rt = *build_r.value();

        auto sh = rt.shutdown();
        if (!sh.has_value()) return fail("shutdown() from Constructed");
    }

    std::printf("E16 ACCEPTANCE PASS: RuntimeBuilder + ApplicationRuntime "
                "public surface is usable end-to-end\n");
    return 0;
}
