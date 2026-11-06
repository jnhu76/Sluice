// #595 R5 research diagnostic (v2): per-writer preallocated event buffers
// for the per-op tail trace declared in r5_tail_trace.hpp. Compiled in only
// with SLUICE_595_R5_TAIL_TRACE; when that macro is absent this TU is empty.
//
// v2 replaces the v1 shared arena + online (slot -> latest record) index,
// whose cross-thread accesses (plain g_cursor, plain record fields read by
// the driver outside dispatch_mtx_, phase reset/export concurrent with
// in-flight writers, 64->32-bit generation truncation) were data races.
//
// Concurrency protocol (all protocol operations seq_cst, so they are totally
// ordered; S denotes that total order):
// - Every hook-writing thread owns one WriterBuf, registered once via a
//   seq_cst fetch_add on g_writer_count. The event cursor, drop counters and
//   presentation counter live in that buffer and are touched only by the
//   owning thread (relaxed atomics: single-writer, zero contention).
// - Hook entry: in_hook.store(true, seq_cst), then g_active.load(seq_cst);
//   exit stores in_hook false (seq_cst). A writer that observes active=true
//   has its in_hook=true ordered before end/begin's active=false store in S,
//   so the phase-transition scan (which reads every registered in_hook after
//   its own active=false store) observes true and waits; the writer's
//   release of in_hook is ordered before the concluding scan read, which
//   makes the writer's event writes visible to the exporter. A writer whose
//   entry is ordered after the concluding scan read necessarily observes
//   active=false and writes nothing. No online cross-thread join exists:
//   (slot, generation) joins happen offline in the analyzer.
// - end_phase/begin_phase first clear g_active, then wait (bounded) until
//   every registered writer has left its hook, then export or reset. A
//   quiesce deadline breach is recorded in the dump header flags rather
//   than hanging the benchmark.

#if defined(SLUICE_595_R5_TAIL_TRACE)

#include "sluice/async/detail/r5_tail_trace.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <unistd.h>

namespace sluice::async::detail::tailtrace {

namespace {

enum class Ev : std::uint8_t {
    built = 1,
    admitted = 2,
    enter = 3,
    cookie = 4,
    cqe = 5,
    published = 6,
    taken = 7,
    stop = 8,
    wake = 9,
};

struct Event {
    std::int64_t t;
    std::uint64_t gen;
    std::uint64_t cookie;
    std::uint32_t seq;
    std::uint32_t slot;
    std::uint32_t aux;
    std::uint8_t kind;
    std::uint8_t pad0;
    std::uint16_t pad1;
};
static_assert(sizeof(Event) == 40, "dump layout is frozen for the analyzer");

#ifndef R5_TAIL_TRACE_EVENT_CAP
#define R5_TAIL_TRACE_EVENT_CAP 262144
#endif
constexpr std::size_t kEventCap = R5_TAIL_TRACE_EVENT_CAP;
constexpr unsigned kMaxWriters = 8;
// Per phase: built/admitted/enter/cookie/taken/stop <= 6 events per op and
// cqe/published/wake <= 3 per op on the driver side, with 20k ops per
// instrumented phase; the cap leaves 4x headroom for deeper shapes.
constexpr std::uint64_t kQuiesceBudgetMs = 2000;

struct WriterBuf {
    Event events[kEventCap];
    std::atomic<std::uint64_t> count;
    std::atomic<std::uint64_t> dropped_overflow;
    std::atomic<std::uint64_t> presentations;
    std::uint32_t tid;
    std::uint32_t pad;
    std::atomic<bool> in_hook;
};

WriterBuf g_writers[kMaxWriters];
std::atomic<unsigned> g_writer_count{0};
std::atomic<bool> g_active{false};
std::atomic<std::uint64_t> g_dropped_unregistered{0};
std::atomic<std::uint64_t> g_quiesce_timeouts{0};
std::uint64_t g_run_nonce = 0;
std::uint32_t g_dump_index = 0;

thread_local WriterBuf* t_buf = nullptr;

std::int64_t steady_ns() noexcept {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

bool env_enabled() noexcept {
    const char* v = std::getenv("SLUICE_595_R5_TAIL_TRACE");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

WriterBuf* register_writer() noexcept {
    const unsigned i = g_writer_count.fetch_add(1, std::memory_order_seq_cst);
    if (i >= kMaxWriters) {
        return nullptr;
    }
    WriterBuf* b = &g_writers[i];
    b->tid = static_cast<std::uint32_t>(::gettid());
    return b;
}

WriterBuf* hook_enter() noexcept {
    // Fast path for off-mode (env unset, g_active never true): one relaxed
    // load, no registration, no in_hook traffic. A stale true observation is
    // safe: the seq_cst active re-check inside the full protocol exits.
    if (!g_active.load(std::memory_order_relaxed)) {
        return nullptr;
    }
    WriterBuf* b = t_buf;
    if (b == nullptr) {
        b = register_writer();
        if (b == nullptr) {
            g_dropped_unregistered.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        t_buf = b;
    }
    b->in_hook.store(true, std::memory_order_seq_cst);
    if (!g_active.load(std::memory_order_seq_cst)) {
        b->in_hook.store(false, std::memory_order_seq_cst);
        return nullptr;
    }
    return b;
}

void hook_exit(WriterBuf* b) noexcept { b->in_hook.store(false, std::memory_order_seq_cst); }

void push(WriterBuf* b, const Event& e) noexcept {
    const std::uint64_t k = b->count.load(std::memory_order_relaxed);
    if (k >= kEventCap) {
        b->dropped_overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    b->events[k] = e;
    b->count.store(k + 1, std::memory_order_relaxed);
}

Event make_event(Ev kind, std::int64_t t) noexcept {
    Event e{};
    e.kind = static_cast<std::uint8_t>(kind);
    e.t = t;
    return e;
}

bool wait_quiesce() noexcept {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kQuiesceBudgetMs);
    for (;;) {
        const unsigned n =
            g_writer_count.load(std::memory_order_seq_cst) < kMaxWriters
                ? g_writer_count.load(std::memory_order_seq_cst)
                : kMaxWriters;
        bool busy = false;
        for (unsigned i = 0; i < n; ++i) {
            if (g_writers[i].in_hook.load(std::memory_order_seq_cst)) {
                busy = true;
                break;
            }
        }
        if (!busy) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            g_quiesce_timeouts.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::this_thread::yield();
    }
}

void reset_writers() noexcept {
    const unsigned n = g_writer_count.load(std::memory_order_seq_cst) < kMaxWriters
                           ? g_writer_count.load(std::memory_order_seq_cst)
                           : kMaxWriters;
    for (unsigned i = 0; i < n; ++i) {
        WriterBuf& b = g_writers[i];
        b.count.store(0, std::memory_order_relaxed);
        b.dropped_overflow.store(0, std::memory_order_relaxed);
        b.presentations.store(0, std::memory_order_relaxed);
    }
}

} // namespace

bool enabled() noexcept {
    static const bool on = env_enabled();
    return on;
}

std::int64_t now_ns_i() noexcept { return steady_ns(); }

std::uint32_t next_presentation() noexcept {
    if (!g_active.load(std::memory_order_relaxed)) {
        return 0;
    }
    WriterBuf* b = t_buf;
    if (b == nullptr) {
        // The submitter is the only caller, and it calls this before the
        // phase's first note_admitted; register here so the presentation
        // counter is per-thread monotonic from the first presentation on.
        b = register_writer();
        if (b == nullptr) {
            return 0;
        }
        t_buf = b;
    }
    return static_cast<std::uint32_t>(
               b->presentations.fetch_add(1, std::memory_order_relaxed)) +
           1;
}

void begin_phase() noexcept {
    if (!enabled()) {
        return;
    }
    if (g_run_nonce == 0) {
        g_run_nonce = static_cast<std::uint64_t>(::getpid()) << 32 |
                      static_cast<std::uint64_t>(steady_ns() & 0xffffffffu);
    }
    g_active.store(false, std::memory_order_seq_cst);
    (void)wait_quiesce();
    reset_writers();
    g_active.store(true, std::memory_order_seq_cst);
}

void end_phase(const char* tag) noexcept {
    if (!g_active.exchange(false, std::memory_order_seq_cst)) {
        return;
    }
    const bool quiesced = wait_quiesce();

    const char* dir = std::getenv("SLUICE_595_R5_TAIL_TRACE_DUMP_DIR");
    if (dir == nullptr || *dir == '\0') {
        return;
    }

    char safe_tag[64];
    std::size_t out = 0;
    for (std::size_t i = 0; tag != nullptr && tag[i] != '\0' && out + 1 < sizeof safe_tag; ++i) {
        const char c = tag[i];
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '.';
        safe_tag[out++] = keep ? c : '_';
    }
    safe_tag[out] = '\0';

    char path[512];
    std::snprintf(path, sizeof path, "%s/tail-%03u-%s.bin", dir, g_dump_index, safe_tag);
    const std::uint64_t dump_idx64 = g_dump_index;
    ++g_dump_index;

    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "tailtrace: cannot open %s\n", path);
        return;
    }

    const unsigned n_stored = g_writer_count.load(std::memory_order_seq_cst) < kMaxWriters
                                  ? g_writer_count.load(std::memory_order_seq_cst)
                                  : kMaxWriters;
    const std::uint64_t magic = 0x32435254544c4941ull; // "AILTTRC2" marker
    const std::uint32_t version = 2;
    const std::uint32_t event_bytes = sizeof(Event);
    const std::uint32_t n_writers = n_stored;
    const std::uint32_t flags =
        (quiesced ? 0u : 1u) |
        (g_dropped_unregistered.load(std::memory_order_relaxed) != 0 ? 2u : 0u);
    const std::uint32_t tag_len = static_cast<std::uint32_t>(out);
    const std::uint64_t pid = static_cast<std::uint64_t>(::getpid());
    const std::uint64_t quiesce_timeouts = g_quiesce_timeouts.load(std::memory_order_relaxed);

    std::fwrite(&magic, sizeof magic, 1, f);
    std::fwrite(&version, sizeof version, 1, f);
    std::fwrite(&event_bytes, sizeof event_bytes, 1, f);
    std::fwrite(&n_writers, sizeof n_writers, 1, f);
    std::fwrite(&flags, sizeof flags, 1, f);
    std::fwrite(&tag_len, sizeof tag_len, 1, f);
    std::fwrite(safe_tag, 1, tag_len, f);
    std::fwrite(&pid, sizeof pid, 1, f);
    std::fwrite(&g_run_nonce, sizeof g_run_nonce, 1, f);
    std::fwrite(&dump_idx64, sizeof dump_idx64, 1, f);
    std::fwrite(&quiesce_timeouts, sizeof quiesce_timeouts, 1, f);

    for (unsigned i = 0; i < n_stored; ++i) {
        const WriterBuf& b = g_writers[i];
        const std::uint64_t wtid = b.tid;
        const std::uint64_t count = b.count.load(std::memory_order_relaxed);
        const std::uint64_t dropped = b.dropped_overflow.load(std::memory_order_relaxed);
        const std::uint64_t pres = b.presentations.load(std::memory_order_relaxed);
        std::fwrite(&wtid, sizeof wtid, 1, f);
        std::fwrite(&count, sizeof count, 1, f);
        std::fwrite(&dropped, sizeof dropped, 1, f);
        std::fwrite(&pres, sizeof pres, 1, f);
        if (count > kEventCap) {
            continue;
        }
        if (count != 0) {
            std::fwrite(b.events, sizeof(Event), static_cast<std::size_t>(count), f);
        }
    }
    std::fclose(f);
    std::fprintf(stderr, "tailtrace: wrote %u writers to %s (quiesced=%d)\n", n_stored, path,
                 quiesced ? 1 : 0);
}

void note_built(std::uint32_t seq, std::uint32_t caller_window, std::int64_t t0_ns) noexcept {
    WriterBuf* b = hook_enter();
    if (b == nullptr) {
        return;
    }
    Event e = make_event(Ev::built, t0_ns);
    e.seq = seq;
    e.aux = caller_window;
    push(b, e);
    hook_exit(b);
}

void note_admitted(std::uint32_t slot, std::uint64_t generation,
                   std::uint32_t presentation) noexcept {
    WriterBuf* b = hook_enter();
    if (b == nullptr) {
        return;
    }
    Event e = make_event(Ev::admitted, steady_ns());
    e.slot = slot;
    e.gen = generation;
    e.aux = presentation;
    push(b, e);
    hook_exit(b);
}

void note_enter_batch(const RequestKey* keys, std::size_t count) noexcept {
    WriterBuf* b = hook_enter();
    if (b == nullptr || keys == nullptr) {
        if (b != nullptr) {
            hook_exit(b);
        }
        return;
    }
    const std::int64_t t2 = steady_ns();
    for (std::size_t i = 0; i < count; ++i) {
        Event e = make_event(Ev::enter, t2);
        e.slot = keys[i].slot.value;
        e.gen = keys[i].generation.value;
        push(b, e);
    }
    hook_exit(b);
}

void note_enter_single(std::uint32_t slot, std::uint64_t generation) noexcept {
    WriterBuf* b = hook_enter();
    if (b == nullptr) {
        return;
    }
    Event e = make_event(Ev::enter, steady_ns());
    e.slot = slot;
    e.gen = generation;
    push(b, e);
    hook_exit(b);
}

void note_cookie(std::uint32_t slot, std::uint64_t generation, std::uint64_t cookie) noexcept {
    WriterBuf* b = hook_enter();
    if (b == nullptr) {
        return;
    }
    Event e = make_event(Ev::cookie, steady_ns());
    e.slot = slot;
    e.gen = generation;
    e.cookie = cookie;
    push(b, e);
    hook_exit(b);
}

void note_cqe(std::uint32_t slot, std::uint64_t generation) noexcept {
    WriterBuf* b = hook_enter();
    if (b == nullptr) {
        return;
    }
    Event e = make_event(Ev::cqe, steady_ns());
    e.slot = slot;
    e.gen = generation;
    push(b, e);
    hook_exit(b);
}

void note_published(std::uint32_t slot, std::uint64_t generation) noexcept {
    WriterBuf* b = hook_enter();
    if (b == nullptr) {
        return;
    }
    Event e = make_event(Ev::published, steady_ns());
    e.slot = slot;
    e.gen = generation;
    push(b, e);
    hook_exit(b);
}

void note_taken(std::uint32_t seq, std::int64_t t5_ns) noexcept {
    WriterBuf* b = hook_enter();
    if (b == nullptr) {
        return;
    }
    Event e = make_event(Ev::taken, t5_ns);
    e.seq = seq;
    push(b, e);
    hook_exit(b);
}

void note_stop_kind(const RequestKey* keys, std::size_t count, std::uint32_t stop_kind) noexcept {
    WriterBuf* b = hook_enter();
    if (b == nullptr || keys == nullptr) {
        if (b != nullptr) {
            hook_exit(b);
        }
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        Event e = make_event(Ev::stop, 0);
        e.slot = keys[i].slot.value;
        e.gen = keys[i].generation.value;
        e.aux = stop_kind;
        push(b, e);
    }
    hook_exit(b);
}

void note_wake() noexcept {
    WriterBuf* b = hook_enter();
    if (b == nullptr) {
        return;
    }
    push(b, make_event(Ev::wake, steady_ns()));
    hook_exit(b);
}

} // namespace sluice::async::detail::tailtrace

#endif
