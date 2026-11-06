// #595 R5 research diagnostic: preallocated arena backing for the per-op
// tail trace declared in r5_tail_trace.hpp. Compiled in only with
// SLUICE_595_R5_TAIL_TRACE; when that macro is absent this TU is empty.

#if defined(SLUICE_595_R5_TAIL_TRACE)

#include "sluice/async/detail/r5_tail_trace.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sluice::async::detail::tailtrace {

namespace {

// Record layout is frozen for the offline analyzer; see the R5 protocol and
// analysis notes for the field-by-field contract.
struct Record {
    std::uint32_t seq;
    std::uint32_t slot;
    std::uint32_t generation;
    std::uint32_t presentation;
    std::uint64_t cookie;
    std::uint32_t flags; // stop kind in bits 0..2, caller_window in bits 3..10
    std::uint32_t pad;
    std::int64_t t[7]; // T0, T1, T2, T_wake, T3, T4, T5
};

constexpr std::size_t kArenaRecords = 400000; // 2 * bench kInstrumentedSampleCap
constexpr std::size_t kSlotLatest = 64;
constexpr std::uint32_t kEmpty = 0xffffffffu;
constexpr std::uint64_t kMagic = 0x5441494c54524143ull; // "CARTLIAT"
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kCountsLen = 18;

Record g_records[kArenaRecords];
// Synchronization note: the per-slot latest-record index is written by the
// submitter at admit time inside the dispatch mutex and read by the driver
// only after a kernel completion of that same op, so relaxed atomics suffice;
// plain record fields rely on the same publication happens-before chain.
std::atomic<std::uint32_t> g_slot_latest[kSlotLatest];
std::atomic<std::uint32_t> g_seq_map[kArenaRecords];
std::size_t g_cursor = 0;
std::size_t g_t1_cursor = 0;
std::uint64_t g_presentation = 0;
std::uint32_t g_dump_index = 0;
std::atomic<bool> g_active{false};
std::atomic<std::int64_t> g_last_wake_ns{0};

struct Counts {
    std::uint64_t t0, t1, t2_keys, t2_single, cookie, cqe, published, taken, wake,
        lost_overflow, join_fail_t1, join_fail_t2, join_fail_cookie, join_fail_cqe,
        join_fail_published, join_fail_taken, seq_out_of_range, presentations;
};
Counts g_counts{};

std::int64_t steady_ns() noexcept {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

bool env_enabled() noexcept {
    const char* v = std::getenv("SLUICE_595_R5_TAIL_TRACE");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

Record* latest_for(std::uint32_t slot, std::uint64_t generation) noexcept {
    if (slot >= kSlotLatest) {
        return nullptr;
    }
    const std::uint32_t idx = g_slot_latest[slot].load(std::memory_order_relaxed);
    if (idx == kEmpty || idx >= g_cursor) {
        return nullptr;
    }
    Record& r = g_records[idx];
    if (r.generation != static_cast<std::uint32_t>(generation) || r.t[1] == 0) {
        return nullptr;
    }
    return &r;
}

} // namespace

bool enabled() noexcept {
    static const bool on = env_enabled();
    return on;
}

std::int64_t now_ns_i() noexcept { return steady_ns(); }

std::uint32_t next_presentation() noexcept {
    ++g_counts.presentations;
    return static_cast<std::uint32_t>(++g_presentation);
}

void begin_phase() noexcept {
    if (!enabled()) {
        return;
    }
    g_cursor = 0;
    g_t1_cursor = 0;
    g_counts = Counts{};
    g_last_wake_ns.store(0, std::memory_order_relaxed);
    for (auto& s : g_slot_latest) {
        s.store(kEmpty, std::memory_order_relaxed);
    }
    for (auto& s : g_seq_map) {
        s.store(kEmpty, std::memory_order_relaxed);
    }
    g_active.store(true, std::memory_order_release);
}

void end_phase(const char* tag) noexcept {
    if (!g_active.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const char* dir = std::getenv("SLUICE_595_R5_TAIL_TRACE_DUMP_DIR");
    if (dir == nullptr || *dir == '\0') {
        return;
    }
    char safe_tag[128];
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
    ++g_dump_index;

    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "tailtrace: cannot open %s\n", path);
        return;
    }
    const std::uint64_t counts[kCountsLen] = {
        g_counts.t0,           g_counts.t1,           g_counts.t2_keys,
        g_counts.t2_single,    g_counts.cookie,       g_counts.cqe,
        g_counts.published,    g_counts.taken,        g_counts.wake,
        g_counts.lost_overflow, g_counts.join_fail_t1, g_counts.join_fail_t2,
        g_counts.join_fail_cookie, g_counts.join_fail_cqe, g_counts.join_fail_published,
        g_counts.join_fail_taken, g_counts.seq_out_of_range, g_counts.presentations,
    };
    const std::uint32_t n_records = static_cast<std::uint32_t>(g_cursor);
    std::fwrite(&kMagic, sizeof kMagic, 1, f);
    std::fwrite(&kVersion, sizeof kVersion, 1, f);
    const std::uint32_t record_bytes = sizeof(Record);
    std::fwrite(&record_bytes, sizeof record_bytes, 1, f);
    std::fwrite(&n_records, sizeof n_records, 1, f);
    const std::uint32_t counts_len = kCountsLen;
    std::fwrite(&counts_len, sizeof counts_len, 1, f);
    std::fwrite(counts, sizeof counts, 1, f);
    if (n_records != 0) {
        std::fwrite(g_records, sizeof(Record), n_records, f);
    }
    std::fclose(f);
    std::fprintf(stderr, "tailtrace: wrote %u records to %s\n", n_records, path);
}

void note_built(std::uint32_t seq, std::uint32_t caller_window, std::int64_t t0_ns) noexcept {
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    if (seq >= kArenaRecords) {
        ++g_counts.seq_out_of_range;
        return;
    }
    const std::size_t idx = g_cursor;
    if (idx >= kArenaRecords) {
        ++g_counts.lost_overflow;
        g_seq_map[seq].store(kEmpty, std::memory_order_relaxed);
        return;
    }
    Record& r = g_records[idx];
    r = Record{};
    r.seq = seq;
    r.flags = (caller_window & 0xffu) << 3;
    r.t[0] = t0_ns;
    g_seq_map[seq].store(static_cast<std::uint32_t>(idx), std::memory_order_relaxed);
    g_cursor = idx + 1;
    ++g_counts.t0;
}

void note_admitted(std::uint32_t slot, std::uint64_t generation,
                   std::uint32_t presentation) noexcept {
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    if (g_t1_cursor >= g_cursor) {
        ++g_counts.join_fail_t1;
        return;
    }
    const std::size_t idx = g_t1_cursor;
    ++g_t1_cursor;
    Record& r = g_records[idx];
    r.slot = slot;
    r.generation = static_cast<std::uint32_t>(generation);
    r.presentation = presentation;
    r.t[1] = steady_ns();
    if (slot < kSlotLatest) {
        g_slot_latest[slot].store(static_cast<std::uint32_t>(idx), std::memory_order_relaxed);
    }
    ++g_counts.t1;
}

void note_enter_batch(const RequestKey* keys, std::size_t count) noexcept {
    if (!g_active.load(std::memory_order_relaxed) || keys == nullptr) {
        return;
    }
    const std::int64_t t2 = steady_ns();
    for (std::size_t i = 0; i < count; ++i) {
        Record* r = latest_for(keys[i].slot.value, keys[i].generation.value);
        if (r == nullptr) {
            ++g_counts.join_fail_t2;
            continue;
        }
        r->t[2] = t2;
        ++g_counts.t2_keys;
    }
}

void note_enter_single(std::uint32_t slot, std::uint64_t generation) noexcept {
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    Record* r = latest_for(slot, generation);
    if (r == nullptr) {
        ++g_counts.join_fail_t2;
        return;
    }
    r->t[2] = steady_ns();
    ++g_counts.t2_single;
}

void note_cookie(std::uint32_t slot, std::uint64_t generation, std::uint64_t cookie) noexcept {
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    Record* r = latest_for(slot, generation);
    if (r == nullptr) {
        ++g_counts.join_fail_cookie;
        return;
    }
    r->cookie = cookie;
    ++g_counts.cookie;
}

void note_cqe(std::uint32_t slot, std::uint64_t generation) noexcept {
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    Record* r = latest_for(slot, generation);
    if (r == nullptr) {
        ++g_counts.join_fail_cqe;
        return;
    }
    r->t[3] = g_last_wake_ns.load(std::memory_order_relaxed);
    r->t[4] = steady_ns();
    ++g_counts.cqe;
}

void note_published(std::uint32_t slot, std::uint64_t generation) noexcept {
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    Record* r = latest_for(slot, generation);
    if (r == nullptr) {
        ++g_counts.join_fail_published;
        return;
    }
    r->t[5] = steady_ns();
    ++g_counts.published;
}

void note_taken(std::uint32_t seq, std::int64_t t5_ns) noexcept {
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    if (seq >= kArenaRecords) {
        ++g_counts.seq_out_of_range;
        return;
    }
    const std::uint32_t idx = g_seq_map[seq].load(std::memory_order_relaxed);
    if (idx == kEmpty || idx >= g_cursor) {
        ++g_counts.join_fail_taken;
        return;
    }
    g_records[idx].t[6] = t5_ns;
    ++g_counts.taken;
}

void note_stop_kind(const RequestKey* keys, std::size_t count, std::uint32_t stop_kind) noexcept {
    if (!g_active.load(std::memory_order_relaxed) || keys == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        Record* r = latest_for(keys[i].slot.value, keys[i].generation.value);
        if (r == nullptr) {
            continue;
        }
        r->flags = (r->flags & ~7u) | (stop_kind & 7u);
    }
}

void note_wake() noexcept {
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    g_last_wake_ns.store(steady_ns(), std::memory_order_relaxed);
    ++g_counts.wake;
}

} // namespace sluice::async::detail::tailtrace

#endif
