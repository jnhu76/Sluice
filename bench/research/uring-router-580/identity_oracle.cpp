// #580 R5 identity oracle (research-only). Deterministic scenarios over the
// real backend via the internal-testing seam, asserting the router-lookup
// identity rules that any lookup change must preserve. Runs against any arm
// (baseline / we2 / mutants) selected at compile time. Exit code 0 only when
// every named assertion passes; a mutant kill must come from a named
// assertion, never from a crash or timeout.
//
// Mutant gates (compile-time):
//   SLUICE_580_MUTANT_FALSE_MISS  - find_live_router_cookie_ reports a miss for
//                                   cookies >= 2 (M7 family: a live CQE is
//                                   silently discarded)
//   SLUICE_580_MUTANT_SLOT_ONLY   - find_live_router_index_ drops the
//                                   generation check (M3/M6 family)

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <liburing.h>
#include <unistd.h>

#include <cstdio>
#include <optional>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "uring_internal.hpp"
#include "uring_test_seams.hpp"

using namespace sluice;
using sluice::async::AsyncIoContext;
using sluice::async::UringAsyncBackend;
using sluice::async::UringConfig;

namespace {

int g_failed = 0;
int g_checked = 0;

void check(bool cond, const char* name) {
    ++g_checked;
    if (!cond) {
        ++g_failed;
        std::printf("FAILED assertion: %s\n", name);
    } else {
        std::printf("ok %s\n", name);
    }
    std::fflush(stdout);
}

constexpr std::uint64_t kControlTagBit = std::uint64_t{1} << 63;

struct Fixture {
    std::string path;
};

std::optional<File> make_fixture(Fixture& fix) {
    fix.path = "/tmp/sluice580_oracle_" + std::to_string(getpid()) + ".bin";
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::create_new;
    mode.contents = FileInitialContents::truncate;
    auto opened = File::open(fix.path, mode);
    if (!opened.has_value())
        return std::nullopt;
    std::vector<std::byte> buffer(4096);
    for (std::size_t i = 0; i < buffer.size(); ++i)
        buffer[i] = static_cast<std::byte>((i * 131u + 7u) & 0xffu);
    for (int block = 0; block < 8; ++block) {
        auto res = blocking::write_all_at(opened.value(), block * 4096,
                                          std::span<const std::byte>(buffer.data(), 4096));
        if (!res.has_value() || !res.value().complete())
            return std::nullopt;
    }
    (void)blocking::sync_data(opened.value());
    return std::optional<File>(std::move(opened).value());
}

bool bytes_ok(const std::byte* b, std::uint64_t offset) {
    for (std::size_t i = 0; i < 4096; ++i)
        if (b[i] != static_cast<std::byte>(((offset + i) * 131u + 7u) & 0xffu))
            return false;
    return true;
}

struct Made {
    AsyncIoContext ctx;
    UringAsyncBackend* raw;
};

Made make_backend(std::size_t capacity) {
    UringConfig config;
    config.request_capacity = capacity;
    config.queue_depth = static_cast<unsigned>(capacity);
    auto owned = std::make_unique<UringAsyncBackend>(config,
                                                     sluice::async::UringBackendSubmitTestHooks{});
    UringAsyncBackend* raw = owned.get();
    AsyncIoContext ctx = sluice::async::detail::make_context(std::move(owned));
    return Made{std::move(ctx), raw};
}

bool quiescent(UringAsyncBackend& backend) {
    return backend.live_cookies_for_test() == 0 &&
           backend.live_control_sqes_for_test() == 0 &&
           backend.transport_ledger_size_for_test() == 0 &&
           backend.dispatch_size_for_test() == 0;
}

template <class RequestLike>
bool await_ready(RequestLike& req, AsyncIoContext& ctx) {
    for (int i = 0; i < 200000 && !req.ready(); ++i)
        (void)ctx.poll();
    return req.ready();
}

bool settle(AsyncIoContext& ctx, UringAsyncBackend& raw) {
    for (int i = 0; i < 200000 && !quiescent(raw); ++i)
        (void)ctx.poll();
    return quiescent(raw);
}

// Scenario 1: after a request completes and its route retires, replaying its
// exact cookie is a silent miss that changes nothing.
void stale_cookie_dropped_after_completion() {
    Made made = make_backend(16);
    Fixture fix;
    auto f = make_fixture(fix);
    std::vector<std::byte> buffer(4096);
    async::ReadOp op;
    op.file = async::NativeFileRef{f.value()};
    op.dst = buffer.data();
    op.offset = 0;
    op.len = 4096;
    auto req = made.ctx.submit_read(op);
    check(req.has_value(), "s1 submit accepted");
    auto value = std::move(req).value();
    const std::uint64_t cookie = made.raw->peek_next_cookie_for_test() - 1;
    (void)await_ready(value, made.ctx);
    auto observed = value.take_result();
    check(observed.readiness == async::RequestReadiness::ready && observed.result.has_value() &&
              observed.result.value() == 4096,
          "s1 original completion delivered with exact identity");
    (void)settle(made.ctx, *made.raw);
    made.raw->inject_cqe_for_test(cookie, 4096);
    made.raw->inject_cqe_for_test(cookie, -5);
    check(quiescent(*made.raw), "s1 stale replay left no live route or residue");
    (void)unlink(fix.path.c_str());
}

// Scenario 2: a retired cookie must not attach to a successor route that
// reuses the same router slot.
void stale_cookie_dropped_after_slot_reuse() {
    Made made = make_backend(4);
    Fixture fix;
    auto f = make_fixture(fix);
    std::vector<std::byte> a(4096), b(4096);
    async::ReadOp first;
    first.file = async::NativeFileRef{f.value()};
    first.dst = a.data();
    first.offset = 0;
    first.len = 4096;
    auto req_a = made.ctx.submit_read(first);
    check(req_a.has_value(), "s2 first submit accepted");
    auto va = std::move(req_a).value();
    const std::uint64_t cookie_a = made.raw->peek_next_cookie_for_test() - 1;
    while (!va.ready())
        (void)made.ctx.poll();
    (void)va.take_result();
    (void)settle(made.ctx, *made.raw);

    async::ReadOp second;
    second.file = async::NativeFileRef{f.value()};
    second.dst = b.data();
    second.offset = 4096;
    second.len = 4096;
    auto req_b = made.ctx.submit_read(second);
    check(req_b.has_value(), "s2 successor submit accepted");
    auto vb = std::move(req_b).value();

    made.raw->inject_cqe_for_test(cookie_a, 8192);
    made.raw->inject_cqe_for_test(cookie_a, -5);

    const bool b_ready = await_ready(vb, made.ctx);
    check(b_ready, "s2 successor completed within the poll budget");
    bool s2_bytes_ok = false;
    if (b_ready) {
        auto ob = vb.take_result();
        s2_bytes_ok = ob.readiness == async::RequestReadiness::ready &&
                      ob.result.has_value() && ob.result.value() == 4096 &&
                      bytes_ok(b.data(), 4096);
    }
    check(b_ready && s2_bytes_ok,
          "s2 successor unaffected by retired-cookie replay on reused slot");
    (void)settle(made.ctx, *made.raw);
    check(made.raw->live_cookies_for_test() == 0, "s2 successor retired exactly once");
    (void)unlink(fix.path.c_str());
}

// Scenario 3: duplicate completion for a live-but-already-terminal route must
// not double-publish or double-retire (the second CQE is a miss).
void duplicate_cqe_dropped() {
    Made made = make_backend(8);
    Fixture fix;
    auto f = make_fixture(fix);
    std::vector<std::byte> buffer(4096);
    async::ReadOp op;
    op.file = async::NativeFileRef{f.value()};
    op.dst = buffer.data();
    op.offset = 0;
    op.len = 4096;
    auto req = made.ctx.submit_read(op);
    auto value = std::move(req).value();
    const std::uint64_t cookie = made.raw->peek_next_cookie_for_test() - 1;
    (void)await_ready(value, made.ctx);
    auto first = value.take_result();
    check(first.readiness == async::RequestReadiness::ready, "s3 first terminal delivered");
    (void)settle(made.ctx, *made.raw);
    made.raw->inject_cqe_for_test(cookie, 4096);
    (void)made.ctx.poll();
    check(quiescent(*made.raw), "s3 duplicate CQE produced no second publication or residue");
    (void)unlink(fix.path.c_str());
}

// Scenario 4: a running cancel must converge through its control CQE, and a
// control CQE aimed at a wrong (non-live) target must be silently ignored --
// control routing is exact-cookie, never slot-approximate. The real kernel
// races the original CQE against the cancel, so the terminal may legally be
// either success or canceled; identity is asserted through the control
// accounting, not the terminal race outcome.
void cancel_control_routing_is_exact() {
    Made made = make_backend(8);
    Fixture fix;
    auto f = make_fixture(fix);
    std::vector<std::byte> buffer(4096);
    async::ReadOp op;
    op.file = async::NativeFileRef{f.value()};
    op.dst = buffer.data();
    op.offset = 0;
    op.len = 4096;
    auto req = made.ctx.submit_read(op);
    check(req.has_value(), "s4 submit accepted");
    auto value = std::move(req).value();
    const auto disposition = value.cancel();
    check(disposition.has_value() &&
              disposition.value() == async::CancelDisposition::requested,
          "s4 running cancel reported requested");
    const auto cookie = made.raw->live_cookie_for_offset_for_test(0);
    check(cookie.has_value(), "s4 live cookie resolved");
    if (!cookie.has_value())
        return;
    const std::uint64_t wrong_target = made.raw->peek_next_cookie_for_test();
    for (int i = 0; i < 100000 && made.raw->live_control_sqes_for_test() == 0 &&
                    !value.ready();
         ++i)
        (void)made.ctx.poll();
    const std::size_t before = made.raw->live_control_sqes_for_test();
    made.raw->inject_cqe_for_test(kControlTagBit | wrong_target, 0);
    check(made.raw->live_control_sqes_for_test() == before,
          "s4 wrong-target control CQE ignored by exact-cookie routing");
    made.raw->inject_cqe_for_test(kControlTagBit | *cookie, 0);
    const bool ready4 = await_ready(value, made.ctx);
    check(ready4, "s4 request settled within the poll budget");
    if (ready4) {
        auto observed = value.take_result();
        const bool success_terminal =
            observed.result.has_value() && observed.result.value() == 4096;
        const bool canceled_terminal =
            !observed.result.has_value() &&
            observed.result.error().code == IoError::Code::canceled;
        check(observed.readiness == async::RequestReadiness::ready &&
                  (success_terminal || canceled_terminal),
              "s4 terminal is a legal kernel outcome (success or canceled)");
    }
    check(settle(made.ctx, *made.raw) && made.raw->live_control_sqes_for_test() == 0 &&
              made.raw->live_cookies_for_test() == 0,
          "s4 control released and route retired (bounded)");
    (void)unlink(fix.path.c_str());
}

// Scenario 5: canceling an already-settled request must not touch the live
// successor that reused the slot (identity is slot+generation, never slot).
void cancel_of_settled_generation_misses_live_successor() {
    Made made = make_backend(4);
    Fixture fix;
    auto f = make_fixture(fix);
    std::vector<std::byte> a(4096), b(4096);
    async::ReadOp first;
    first.file = async::NativeFileRef{f.value()};
    first.dst = a.data();
    first.offset = 0;
    first.len = 4096;
    auto req_a = made.ctx.submit_read(first);
    auto va = std::move(req_a).value();
    const auto id_a = va.id();
    while (!va.ready())
        (void)made.ctx.poll();
    (void)va.take_result();
    (void)settle(made.ctx, *made.raw);

    async::ReadOp second;
    second.file = async::NativeFileRef{f.value()};
    second.dst = b.data();
    second.offset = 8192;
    second.len = 4096;
    auto req_b = made.ctx.submit_read(second);
    check(req_b.has_value(), "s5 successor submit accepted");
    auto vb = std::move(req_b).value();

    const auto stale_disposition = made.ctx.cancel(id_a);
    check(stale_disposition.has_value() &&
              stale_disposition.value() != async::CancelDisposition::requested,
          "s5 settled-generation cancel is not a running cancel");

    const bool b5_ready = await_ready(vb, made.ctx);
    check(b5_ready, "s5 successor completed within the poll budget");
    bool s5_bytes_ok = false;
    if (b5_ready) {
        auto ob = vb.take_result();
        s5_bytes_ok = ob.readiness == async::RequestReadiness::ready &&
                      ob.result.has_value() && ob.result.value() == 4096 &&
                      bytes_ok(b.data(), 8192);
    }
    check(b5_ready && s5_bytes_ok,
          "s5 live successor completed despite settled-generation cancel on same slot");
    (void)settle(made.ctx, *made.raw);
    check(made.raw->live_control_sqes_for_test() == 0,
          "s5 no control reference leaked by the settled cancel");
    (void)unlink(fix.path.c_str());
}

// Scenario 6: user_data 0 is dropped without any router interaction.
void zero_user_data_dropped() {
    Made made = make_backend(8);
    Fixture fix;
    auto f = make_fixture(fix);
    std::vector<std::byte> buffer(4096);
    async::ReadOp op;
    op.file = async::NativeFileRef{f.value()};
    op.dst = buffer.data();
    op.offset = 0;
    op.len = 4096;
    auto req = made.ctx.submit_read(op);
    auto value = std::move(req).value();
    made.raw->inject_cqe_for_test(0, 12345);
    (void)await_ready(value, made.ctx);
    auto observed = value.take_result();
    check(observed.readiness == async::RequestReadiness::ready && observed.result.has_value() &&
              observed.result.value() == 4096,
          "s6 zero user_data dropped; real completion still delivered");
    (void)settle(made.ctx, *made.raw);
    (void)unlink(fix.path.c_str());
}

} // namespace

int main() {
    stale_cookie_dropped_after_completion();
    stale_cookie_dropped_after_slot_reuse();
    duplicate_cqe_dropped();
    cancel_control_routing_is_exact();
    cancel_of_settled_generation_misses_live_successor();
    zero_user_data_dropped();
    std::printf("%s: %d/%d oracle assertions passed\n", g_failed ? "ORACLE FAILED" : "ALL GREEN",
                g_checked - g_failed, g_checked);
    return g_failed == 0 ? 0 : 1;
}
