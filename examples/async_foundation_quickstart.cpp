// async_foundation_quickstart — public-only async foundation quickstart.
//
// Demonstrates the minimal async-foundation flow against INSTALLED/PUBLIC
// headers only:
//   - construct an AsyncIoContext with a FakeAsyncBackend (deterministic)
//   - submit a read against a caller-owned Completion<std::size_t>
//   - poll for completion
//   - read the op result from the Completion
//
// Negative constraints:
//   - no tests/ include path
//   - no SLUICE_ASYNC_INTERNAL_TESTING
//   - no private source inclusion
//   - no Group/Scheduler/Fiber (those are out of scope for this quickstart).
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>

#include <cstddef>
#include <cstdio>
#include <memory>

int main() {
    // FakeAsyncBackend is a deterministic test backend: auto_bytes(n) makes
    // the next poll() complete each outstanding op with n bytes.
    auto backend = std::make_unique<sluice::async::FakeAsyncBackend>();
    sluice::async::FakeAsyncBackend* raw = backend.get();
    raw->auto_bytes(8);

    sluice::async::AsyncIoContext ctx(std::move(backend));

    // submit_read against a caller-owned Completion.
    sluice::async::Completion<std::size_t> c;
    std::byte buf[8]{};
    if (!ctx.submit_read(sluice::async::ReadOp{0, buf, 8, 0}, c).has_value())
        return 1;

    // poll() reaps completions non-blockingly; returns count reaped.
    if (ctx.poll() != 1) return 2;

    // The op result is read from the Completion after it is ready — NOT from
    // wait_one()/poll() return values.
    if (!c.ready()) return 3;
    auto r = c.result();
    if (!r.has_value() || r.value() != 8) return 4;

    std::printf("async quickstart: read %zu bytes\n", r.value());
    return 0;
}
