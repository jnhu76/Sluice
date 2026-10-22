// Pending-control P>0 mechanism-cost A/B harness (controlled submit seam).
//
// Both arms execute an identical state-transition script against an identical
// fake-kernel submit seam: SQE pressure is held while running operations are
// canceled, so real sticky intents (P>0) are created, retained across
// pressure passes, then issued and settled once pressure releases. The arms
// differ only in the compiled backend sources (BASE vs E1); the harness,
// seam and workload are byte-identical. This measures the whole controlled
// execution path's user-space cost under P>0 — a mechanism cost bound, not a
// native-kernel performance result.
//
// Per-cycle script (R = pressured ops, C = router capacity, Q = queue depth):
//   hold ON; submit R reads (fills the Q-entry SQ ring, extras queue);
//   cancel all R (sticky intents retained, P=R);
//   M pressure passes (poll under hold: both arms scan the router while
//   P>0; BASE scans unconditionally, E1 gates on the derived count);
//   hold OFF; poll twice (submit fakes kernel consumption; the R sticky
//   intents are issued as prepared control SQEs and then submitted);
//   inject R control CQEs and R original CQEs; poll; every request must
//   settle (oracle) and internal work must retire.

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <liburing.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <string>

namespace {

using namespace sluice::async;

constexpr std::uint64_t kControlTag = std::uint64_t{1} << 63u;

int switchable_submit(void* context, ::io_uring* ring) noexcept {
    auto* hold = static_cast<std::atomic<bool>*>(context);
    if (hold->load(std::memory_order_acquire)) {
        return 0;
    }
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    *ring->sq.khead = ring->sq.sqe_tail;
    return static_cast<int>(ready);
}

struct Counters {
    std::uint64_t cancel_attempts = 0;
    std::uint64_t cancel_requested = 0;
    std::uint64_t sticky_transitions = 0;
    std::uint64_t control_prepared = 0;
    std::uint64_t control_submitted = 0;
    std::uint64_t settled = 0;
    std::size_t p_max = 0;
    std::uint64_t parity_violations = 0;
};

std::uint64_t thread_cpu_ns() {
    ::timespec ts{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint64_t mono_ns() {
    ::timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

struct Params {
    std::string arm;
    std::size_t capacity = 64;
    unsigned queue_depth = 8;
    std::size_t pressured = 8;
    std::size_t cancels_per_op = 1;
    std::size_t passes = 24;
    std::size_t cycles = 200;
    std::size_t repeats = 3;
    unsigned rep = 0;
    std::string phase = "high_p";
};
void bail(const Params& p, const Counters& counters, std::uint64_t cancel_ns,
          const char* why) {
    std::printf(
        "{\"arm\":\"%s\",\"phase\":\"%s\",\"rep\":%u,\"repeat\":0,\"ok\":false,"
        "\"cpu_ns\":0,\"wall_ns\":0,\"cancel_ns\":%llu,"
        "\"cycles\":%zu,\"cancel_attempts\":%llu,\"cancel_requested\":%llu,"
        "\"sticky_transitions\":%llu,\"control_prepared\":%llu,"
        "\"control_submitted\":%llu,\"settled\":%llu,\"p_max\":%zu,"
        "\"parity_violations\":%llu,\"failure\":\"%s\"}\n",
        p.arm.c_str(), p.phase.c_str(), p.rep,
        static_cast<unsigned long long>(cancel_ns), p.cycles,
        static_cast<unsigned long long>(counters.cancel_attempts),
        static_cast<unsigned long long>(counters.cancel_requested),
        static_cast<unsigned long long>(counters.sticky_transitions),
        static_cast<unsigned long long>(counters.control_prepared),
        static_cast<unsigned long long>(counters.control_submitted),
        static_cast<unsigned long long>(counters.settled), counters.p_max,
        static_cast<unsigned long long>(counters.parity_violations), why);
    std::fflush(stdout);
    std::_Exit(1);
}


bool run_cycles(const Params& p, Counters& counters, std::uint64_t& cpu_ns,
                std::uint64_t& wall_ns, std::uint64_t& cancel_ns) {
    cancel_ns = 0;
    FILE* backing = ::tmpfile();
    if (backing == nullptr) {
        bail(p, counters, cancel_ns, "tmpfile failed");
    }
    std::byte content[4096];
    std::memset(content, 0x33, sizeof(content));
    if (::fwrite(content, 1, sizeof(content), backing) != sizeof(content)) {
        bail(p, counters, cancel_ns, "fixture write failed");
    }
    ::rewind(backing);

    auto hold = std::make_unique<std::atomic<bool>>(true);
    UringBackendSubmitTestHooks hooks;
    hooks.submit = switchable_submit;
    hooks.context = hold.get();
    auto owned = std::make_unique<UringAsyncBackend>(
        UringConfig{p.capacity, p.queue_depth}, hooks);
    UringAsyncBackend* raw = owned.get();
    auto ctx = std::make_unique<AsyncIoContext>(detail::make_context(std::move(owned)));

    const auto data_op = [&](std::byte* buffer, std::uint64_t offset) {
        return ReadOp{NativeFileRef(::fileno(backing), sluice::FileAccess::read_only), buffer, 4,
                      offset};
    };

    // The SQ ring must be full before the cancels so the control path finds
    // no SQE and retains sticky intents: `pressured` canceled operations plus
    // `queue_depth - pressured` uncanceled fillers fill it exactly.
    const std::size_t pressured = p.pressured < p.queue_depth ? p.pressured : p.queue_depth;
    const std::size_t fillers = p.queue_depth - pressured;

    const std::uint64_t cpu_start = thread_cpu_ns();
    const std::uint64_t wall_start = mono_ns();
    for (std::size_t cycle = 0; cycle < p.cycles; ++cycle) {
        hold->store(true, std::memory_order_release);

        std::deque<Request<std::size_t>> window;
        std::deque<Request<std::size_t>> filler_window;
        std::byte buffers[16][4];
        for (std::size_t i = 0; i < fillers; ++i) {
            auto made = ctx->submit_read(data_op(buffers[i], 256 + static_cast<std::uint64_t>(i) * 8));
            if (!made.has_value()) {
                bail(p, counters, cancel_ns, "filler submit rejected");
            }
            filler_window.push_back(std::move(made).value());
        }
        for (std::size_t i = 0; i < pressured; ++i) {
            auto made = ctx->submit_read(data_op(buffers[fillers + i],
                                                 static_cast<std::uint64_t>(i) * 8));
            if (!made.has_value()) {
                bail(p, counters, cancel_ns, "submit rejected");
            }
            window.push_back(std::move(made).value());
        }

        const std::uint64_t cancel_start = thread_cpu_ns();
        for (std::size_t i = 0; i < pressured; ++i) {
            for (std::size_t k = 0; k < p.cancels_per_op; ++k) {
                ++counters.cancel_attempts;
                const auto seen = ctx->cancel(window.front().id());
                if (!seen.has_value() || seen.value() != CancelDisposition::requested) {
                    bail(p, counters, cancel_ns, "cancel not requested");
                }
                ++counters.cancel_requested;
            }
            window.push_back(std::move(window.front()));
            window.pop_front();
        }
        cancel_ns += thread_cpu_ns() - cancel_start;

#ifdef SLUICE_E1_SEAM_ARM
        {
            const auto snap = raw->pending_control_snapshot_for_test();
            if (snap.derived_count != snap.authoritative_pending)
                ++counters.parity_violations;
            if (snap.derived_count > counters.p_max)
                counters.p_max = snap.derived_count;
            counters.sticky_transitions += snap.derived_count;
        }
#else
        if (raw->live_control_sqes_for_test() != 0) {
            bail(p, counters, cancel_ns, "control SQE submitted while SQ ring was held");
        }
        if (raw->transport_ledger_size_for_test() != p.queue_depth) {
            bail(p, counters, cancel_ns,
                 "control SQE prepared while SQ ring was held (ledger cardinality)");
        }
        counters.sticky_transitions += pressured;
#endif

        for (std::size_t pass = 0; pass < p.passes; ++pass) {
            (void)ctx->poll();
        }

#ifdef SLUICE_E1_SEAM_ARM
        {
            const auto snap = raw->pending_control_snapshot_for_test();
            if (snap.derived_count != snap.authoritative_pending)
                ++counters.parity_violations;
            if (snap.derived_count != pressured) {
                bail(p, counters, cancel_ns, "sticky intents lost across pressure passes");
            }
        }
#else
        if (raw->live_control_sqes_for_test() != 0) {
            bail(p, counters, cancel_ns, "control SQE escaped during hold");
        }
#endif

        hold->store(false, std::memory_order_release);
        (void)ctx->poll();
        (void)ctx->poll();
        if (raw->live_control_sqes_for_test() != pressured) {
            bail(p, counters, cancel_ns, "retained intents were not issued after release");
        }
        counters.control_prepared += pressured;
        counters.control_submitted += pressured;

        for (std::size_t i = 0; i < pressured; ++i) {
            const auto identity =
                raw->live_cookie_for_offset_for_test(static_cast<std::uint64_t>(i) * 8);
            if (!identity.has_value()) {
                bail(p, counters, cancel_ns, "cookie missing at settle");
            }
            raw->inject_cqe_for_test(kControlTag | *identity, 0);
        }
        for (std::size_t i = 0; i < pressured; ++i) {
            const auto identity =
                raw->live_cookie_for_offset_for_test(static_cast<std::uint64_t>(i) * 8);
            if (!identity.has_value()) {
                bail(p, counters, cancel_ns, "cookie missing at original CQE");
            }
            raw->inject_cqe_for_test(*identity, -ECANCELED);
        }
        for (std::size_t i = 0; i < fillers; ++i) {
            const auto identity =
                raw->live_cookie_for_offset_for_test(256 + static_cast<std::uint64_t>(i) * 8);
            if (!identity.has_value()) {
                bail(p, counters, cancel_ns, "filler cookie missing at settle");
            }
            raw->inject_cqe_for_test(*identity, 4);
        }
        (void)ctx->poll();

        for (auto& request : window) {
            if (!request.ready()) {
                bail(p, counters, cancel_ns, "request did not settle through the control path");
            }
            (void)request.take_result();
            ++counters.settled;
        }
        for (auto& request : filler_window) {
            if (!request.ready()) {
                bail(p, counters, cancel_ns, "filler request did not settle");
            }
            (void)request.take_result();
            ++counters.settled;
        }

#ifdef SLUICE_E1_SEAM_ARM
        {
            const auto snap = raw->pending_control_snapshot_for_test();
            if (snap.derived_count != snap.authoritative_pending || snap.derived_count != 0) {
                bail(p, counters, cancel_ns, "count did not return to zero after settle");
            }
        }
#endif
        if (!raw->internal_work_retired()) {
            bail(p, counters, cancel_ns, "internal work not retired after cycle");
        }
    }
    wall_ns += mono_ns() - wall_start;
    cpu_ns += thread_cpu_ns() - cpu_start;

    const auto shutdown = ctx->shutdown(ShutdownPolicy::drain);
    if (!shutdown.has_value()) {
        bail(p, counters, cancel_ns, "shutdown failed");
    }
    ::fclose(backing);
    return true;
}


std::size_t parse_size(const std::string& v) {
    return static_cast<std::size_t>(std::strtoull(v.c_str(), nullptr, 10));
}

} // namespace

int main(int argc, char** argv) {
    ::alarm(600);
    Params p;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* key) -> std::optional<std::string> {
            const std::string prefix = std::string("--") + key + "=";
            if (arg.rfind(prefix, 0) == 0)
                return arg.substr(prefix.size());
            return std::nullopt;
        };
        if (auto v = value("arm")) p.arm = *v;
        else if (auto v = value("capacity")) p.capacity = parse_size(*v);
        else if (auto v = value("depth")) p.queue_depth = static_cast<unsigned>(parse_size(*v));
        else if (auto v = value("pressured")) p.pressured = parse_size(*v);
        else if (auto v = value("cancels")) p.cancels_per_op = parse_size(*v);
        else if (auto v = value("passes")) p.passes = parse_size(*v);
        else if (auto v = value("cycles")) p.cycles = parse_size(*v);
        else if (auto v = value("repeats")) p.repeats = parse_size(*v);
        else if (auto v = value("rep")) p.rep = static_cast<unsigned>(parse_size(*v));
        else if (auto v = value("phase")) p.phase = *v;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }

    for (std::size_t repeat = 0; repeat < p.repeats; ++repeat) {
        Counters counters;
        std::uint64_t cpu_ns = 0;
        std::uint64_t wall_ns = 0;
        std::uint64_t cancel_ns = 0;
        const bool ok = run_cycles(p, counters, cpu_ns, wall_ns, cancel_ns);
        std::printf(
            "{\"arm\":\"%s\",\"phase\":\"%s\",\"rep\":%u,\"repeat\":%zu,\"ok\":%s,"
            "\"cpu_ns\":%llu,\"wall_ns\":%llu,\"cancel_ns\":%llu,"
            "\"cycles\":%zu,\"cancel_attempts\":%llu,\"cancel_requested\":%llu,"
            "\"sticky_transitions\":%llu,\"control_prepared\":%llu,"
            "\"control_submitted\":%llu,\"settled\":%llu,\"p_max\":%zu,"
            "\"parity_violations\":%llu,\"failure\":\"%s\"}\n",
            p.arm.c_str(), p.phase.c_str(), p.rep, repeat, ok ? "true" : "false",
            static_cast<unsigned long long>(cpu_ns), static_cast<unsigned long long>(wall_ns),
            static_cast<unsigned long long>(cancel_ns), p.cycles,
            static_cast<unsigned long long>(counters.cancel_attempts),
            static_cast<unsigned long long>(counters.cancel_requested),
            static_cast<unsigned long long>(counters.sticky_transitions),
            static_cast<unsigned long long>(counters.control_prepared),
            static_cast<unsigned long long>(counters.control_submitted),
            static_cast<unsigned long long>(counters.settled), counters.p_max,
            static_cast<unsigned long long>(counters.parity_violations), "");
        std::fflush(stdout);
        if (!ok)
            return 1;
    }
    return 0;
}
