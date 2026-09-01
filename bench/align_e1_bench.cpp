// ALIGN-E1 application materiality sweep (#268, prereg
// research/align-e1/ALIGN-E1-PREREGISTRATION.md): realistic READ + WRITE
// file copy at 4K..64K chunk sizes x pipeline depth {1,2,4,8}, with buffer
// geometry as the ONLY variable between the three modules:
//
//   --module engine          the REAL production engine
//                            (run_pipelined_copy_with_backend — the same
//                            copy_task.cpp the CLI uses). Its slot storage
//                            is the fixed production representation
//                            (std::vector<std::byte>, allocator geometry);
//                            external consistency reference only.
//   --module replica-natural the research replica of the production
//                            pipeline (VERBATIM algorithm) with plain
//                            malloc(cap) slot storage — production-like
//                            allocator geometry (glibc: 16 B alignment,
//                            page-offset 0 for large chunks / unspecified
//                            mod-32 residue; recorded per slot).
//   --module replica-aligned the same replica with the exposed buffer
//                            rounded up to 64 B (ALIGN-E0's
//                            tested-effective alignment: 32 B minimum
//                            tested separation, 64 B amplifier best arm)
//                            inside a page-aligned over-allocated block.
//   --module causal-phase16  E1-C1 strict causal control (AMENDMENT 2):
//                            page-aligned posix_memalign backing — the
//                            SAME primitive, size and ownership as
//                            causal-aligned64 — exposed pointer = base+16
//                            (page offset 16, mod64 16; ALIGN-E0's
//                            actually tested +16 point).
//   --module causal-aligned64 same page-aligned backing, exposed = base
//                            (page offset 0, mod64 0).
//
// The two causal arms change ONLY the exposed pointer address phase:
// allocation primitive, allocation size (cap + 64), backing alignment,
// ownership, page-set policy, bytes, op counts, chunk, depth and worker
// topology are identical by construction. The bench fails closed (exit 3)
// if the backing is not page-aligned or the exposed phase deviates from
// the arm's contract. Per-run slot addresses are recorded as
// slots_base_mod4096 / slots_exposed_mod4096 / slots_residual_mod64
// (exposed mod 64).
//
// Only the exposed buffer geometry differs between modules. Same bytes,
// same op count, same depth (pre-submitted read window), same worker
// topology (workers = 1, production CLI default), same hash validation
// (driver hashes src/dst post-exit).
//
// Per run: slots are constructed (replica modules; timed separately), then
// the full engine span is timed (Runtime build/start/submit/wait/drain/
// join + the copy). Same-work gates (fail-closed, exit 3): bytes == file
// size, write_ops == ceil(bytes/chunk), read_ops in [ceil, ceil+depth],
// short_writes == 0. One JSON line per run on stdout (raw evidence;
// appended by the driver into the session runs.jsonl).
//
// Workload bytes: the TAX-0-line generator (4 KiB splitmix64 master block,
// kSeed 0xE1E1E1E121212121), generated once by --generate (512 MiB src).
// Production code is NOT modified (research-only replica + read-only
// engine call).

#include "copy_task.hpp"

#include <sluice/async/await_op_helpers.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;
using CopyStats = sluice_copy::CopyStats;

[[noreturn]] void e1_fatal(const char* what, int err) {
    std::fprintf(stderr, "align_e1_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(2);
}

[[noreturn]] void e1_semantic(const char* what) {
    std::fprintf(stderr, "align_e1_bench: semantic failure: %s\n", what);
    std::exit(3);
}

constexpr std::size_t kBlock = 4096;
constexpr std::size_t kPage = 4096;
constexpr std::uint64_t kSeed = 0xE1E1E1E121212121ull;
constexpr std::size_t kAlignedExposure = 64;  // tested-effective geometry
// E1-C1 (AMENDMENT 2): the exposed-pointer phase offset of the causal
// phase16 arm — ALIGN-E0's actually tested +16 point.
constexpr std::size_t kCausalPhaseOffset = 16;

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

enum class Module { engine, natural, aligned, causal_phase16, causal_aligned64 };

const char* module_name(Module m) {
    switch (m) {
    case Module::engine: return "engine";
    case Module::natural: return "replica-natural";
    case Module::aligned: return "replica-aligned";
    case Module::causal_phase16: return "causal-phase16";
    case Module::causal_aligned64: return "causal-aligned64";
    }
    return "?";
}

// Slot backing/exposure geometry. natural = production-like malloc;
// aligned64 = ALIGN-E1's aligned treatment; the two causal geometries
// (AMENDMENT 2) share ONE posix_memalign backing and differ only in the
// exposed pointer offset.
enum class SlotGeometry { natural, aligned64, causal_phase16, causal_aligned64 };

std::uintptr_t round_up(std::uintptr_t v, std::uintptr_t alignment) {
    return (v + alignment - 1) & ~(alignment - 1);
}

constexpr bool add_would_overflow(std::uint64_t a, std::uint64_t b) noexcept {
    return a > 0xFFFFFFFFFFFFFFFFull - b;
}

// ===========================================================================
// Replica of the production Version B pipeline (apps/sluice-copy/
// copy_task.cpp PipelinedCopyTask). VERBATIM algorithm; the ONLY delta is
// the selectable exposed buffer geometry (ReplicaSlot). Keep this in
// lockstep with the production file; the engine vs replica pair exists
// precisely to detect any drift (engine ~ replica-natural by construction).
// ===========================================================================

enum class SlotState : std::uint8_t {
    idle, reading, read_done, writing, done,
};

struct ReplicaSlot {
    // natural:    plain malloc(cap) — production-like allocator geometry.
    // aligned64:  ONE page-aligned over-allocated owned block per slot,
    //             exposed pointer = round_up(base, kAlignedExposure).
    // causal-*:   the SAME page-aligned posix_memalign(cap + 64) backing
    //             for both arms; exposed = base + kCausalPhaseOffset
    //             (phase16) or base (aligned64). AMENDMENT 2.
    void* base_ptr = nullptr;
    std::size_t alloc_cap = 0;
    bool over_allocated = false;
    std::byte* storage = nullptr;  // exposed pointer (L7 stable)
    sluice::async::Completion<std::size_t> read_c;
    sluice::async::Completion<std::size_t> write_c;
    std::uint64_t chunk_offset = 0;
    std::size_t filled = 0;
    std::size_t written = 0;
    bool eof = false;
    SlotState state = SlotState::idle;

    explicit ReplicaSlot(std::size_t cap, SlotGeometry geometry) {
        if (geometry == SlotGeometry::natural) {
            base_ptr = std::malloc(cap);
            if (base_ptr == nullptr) throw std::bad_alloc();
            storage = reinterpret_cast<std::byte*>(base_ptr);
            alloc_cap = cap;
            return;
        }
        // aligned64 / causal arms: one shared allocation recipe.
        alloc_cap = cap + kAlignedExposure;
        if (::posix_memalign(&base_ptr, kPage, alloc_cap) != 0)
            throw std::bad_alloc();
        const std::uintptr_t raw =
            reinterpret_cast<std::uintptr_t>(base_ptr);
        // aligned64 and causal_aligned64 both expose the base itself
        // (a page-aligned address is already 64-aligned); only phase16
        // shifts the exposed pointer.
        const std::uintptr_t exposed =
            (geometry == SlotGeometry::causal_phase16)
                ? raw + kCausalPhaseOffset
                : round_up(raw, kAlignedExposure);
        storage = reinterpret_cast<std::byte*>(exposed);
        if (exposed + cap > raw + alloc_cap)
            e1_semantic("exposed buffer exceeds owned block");
        over_allocated = true;
    }

    ~ReplicaSlot() {
        if (base_ptr) std::free(base_ptr);
    }

    // Completion members make the slot non-movable/non-copyable — the slot
    // is address-stable for the operation lifetime (L7), held via
    // unique_ptr exactly like the production PipelineSlot.
};

struct ReplicaCopyTask {
    int src_fd;
    int dst_fd;
    std::size_t buffer_size;
    std::size_t pipeline_depth;
    sluice_copy::SyncPolicy sync;
    SlotGeometry geometry;  // slot backing/exposure (built before the task)
    std::vector<std::unique_ptr<ReplicaSlot>> slots;
    Completion<void> sync_c;

    void operator()(RuntimeTaskContext& ctx,
                    sluice::async::TaskResultSlot<Result<CopyStats>>& slot) {
        slot.publish(run_body(ctx));
    }

    Result<void> submit_slot_read(RuntimeTaskContext& ctx, ReplicaSlot& s) {
        std::uint64_t off = s.chunk_offset + s.filled;
        std::byte* dst = s.storage + s.filled;
        std::size_t len = buffer_size - s.filled;
        auto rsr = ctx.submit_read(
            sluice::async::ReadOp{src_fd, dst, len, off}, s.read_c);
        if (!rsr.has_value()) return rsr;
        s.state = SlotState::reading;
        return {};
    }

    Result<void> submit_slot_write(RuntimeTaskContext& ctx, ReplicaSlot& s) {
        std::uint64_t off = s.chunk_offset + s.written;
        const std::byte* src = s.storage + s.written;
        std::size_t len = s.filled - s.written;
        auto wsr = ctx.submit_write(
            sluice::async::WriteOp{dst_fd, src, len, off}, s.write_c);
        if (!wsr.has_value()) return wsr;
        s.state = SlotState::writing;
        return {};
    }

    Result<void> await_slot_read(RuntimeTaskContext& ctx, ReplicaSlot& s,
                                 CopyStats& stats, bool& saw_eof) {
        using sluice::async::AwaitOpTally;
        const std::size_t remaining = buffer_size - s.filled;
        AwaitOpTally tally;
        auto first = await_take(ctx, s.read_c);
        if (!first.has_value())
            return sluice::make_unexpected_void(first.error());
        ++tally.ops;
        if (first.value() < remaining) ++tally.short_ops;

        if (first.value() == 0) {
            stats.read_ops += tally.ops;
            s.eof = true;
            saw_eof = true;
            s.state = (s.filled == 0) ? SlotState::done : SlotState::read_done;
            return {};
        }
        s.filled += first.value();
        if (s.filled >= buffer_size) {
            stats.read_ops += tally.ops;
            s.state = SlotState::read_done;
            return {};
        }
        auto fr = await_read_fill(
            ctx, src_fd,
            std::span<std::byte>(s.storage + s.filled,
                                 buffer_size - s.filled),
            s.chunk_offset + s.filled, s.read_c, &tally);
        if (!fr.has_value())
            return sluice::make_unexpected_void(fr.error());
        stats.read_ops += tally.ops;
        std::size_t n = fr.value();
        s.filled += n;
        if (n < buffer_size - (s.filled - n)) {
            s.eof = true;
            saw_eof = true;
        }
        s.state = (s.filled == 0) ? SlotState::done : SlotState::read_done;
        return {};
    }

    Result<void> await_slot_write(RuntimeTaskContext& ctx, ReplicaSlot& s,
                                  CopyStats& stats) {
        using sluice::async::AwaitOpTally;
        AwaitOpTally tally;
        auto first = await_take(ctx, s.write_c);
        if (!first.has_value())
            return sluice::make_unexpected_void(first.error());
        ++tally.ops;
        if (first.value() == 0) {
            return sluice::make_unexpected_void(
                IoError{IoError::Code::backend_error});
        }
        const std::size_t remaining_before = s.filled - s.written;
        if (first.value() < remaining_before) ++tally.short_ops;
        s.written += first.value();
        if (s.written < s.filled) {
            auto wr = await_write_exact(
                ctx, dst_fd,
                std::span<const std::byte>(s.storage + s.written,
                                           s.filled - s.written),
                s.chunk_offset + s.written, s.write_c, &tally);
            if (!wr.has_value())
                return sluice::make_unexpected_void(wr.error());
            s.written = s.filled;
        }
        stats.write_ops += tally.ops;
        stats.short_writes += tally.short_ops;
        return {};
    }

    Result<CopyStats> run_body(RuntimeTaskContext& ctx) {
        CopyStats stats{};
        stats.sync = sync;

        bool eof_seen = false;
        std::optional<IoError> primary_error;

        auto fail = [&](IoError e) {
            if (!primary_error.has_value()) primary_error = e;
            eof_seen = true;
        };

        for (std::size_t i = 0; i < pipeline_depth; ++i) {
            if (eof_seen) break;
            if (ctx.cancel_token().is_requested()) {
                fail(IoError{IoError::Code::canceled});
                break;
            }
            auto& s = slots[i];
            auto rsr = submit_slot_read(ctx, *s);
            if (!rsr.has_value()) {
                fail(rsr.error());
                break;
            }
        }

        while (!primary_error.has_value()) {
            if (ctx.cancel_token().is_requested()) {
                fail(IoError{IoError::Code::canceled});
                break;
            }

            ReplicaSlot* read_slot = nullptr;
            for (auto& s : slots) {
                if (s->state != SlotState::reading) continue;
                if (read_slot == nullptr ||
                    s->chunk_offset < read_slot->chunk_offset)
                    read_slot = s.get();
            }

            if (read_slot != nullptr) {
                auto r = await_slot_read(ctx, *read_slot, stats, eof_seen);
                if (!r.has_value()) {
                    fail(r.error());
                    break;
                }
            }

            for (;;) {
                if (ctx.cancel_token().is_requested()) {
                    fail(IoError{IoError::Code::canceled});
                    break;
                }
                ReplicaSlot* ws = nullptr;
                for (auto& s : slots) {
                    if (s->state != SlotState::read_done) continue;
                    if (ws == nullptr || s->chunk_offset < ws->chunk_offset)
                        ws = s.get();
                }
                if (ws == nullptr) break;

                if (ws->filled == 0) {
                    ws->state = SlotState::done;
                    continue;
                }
                auto wsr = submit_slot_write(ctx, *ws);
                if (!wsr.has_value()) {
                    fail(wsr.error());
                    break;
                }
                auto wr = await_slot_write(ctx, *ws, stats);
                if (!wr.has_value()) {
                    fail(wr.error());
                    break;
                }
                stats.bytes_copied += ws->filled;

                if (ws->eof || eof_seen) {
                    ws->state = SlotState::done;
                } else {
                    if (add_would_overflow(ws->chunk_offset,
                                           buffer_size * pipeline_depth)) {
                        fail(IoError{IoError::Code::invalid_state});
                        break;
                    }
                    ws->chunk_offset += buffer_size * pipeline_depth;
                    ws->filled = 0;
                    ws->written = 0;
                    ws->eof = false;
                    ws->state = SlotState::idle;
                    if (ctx.cancel_token().is_requested()) {
                        fail(IoError{IoError::Code::canceled});
                        break;
                    }
                    auto rsr = submit_slot_read(ctx, *ws);
                    if (!rsr.has_value()) {
                        eof_seen = true;
                        ws->state = SlotState::done;
                        fail(rsr.error());
                        break;
                    }
                }
            }
            if (primary_error.has_value()) break;

            bool all_done = true;
            for (auto& s : slots)
                if (s->state != SlotState::done) { all_done = false; break; }
            if (all_done) break;
        }

        for (auto& s : slots) {
            if (s->read_c.outstanding()) {
                auto dr = await_drain(ctx, s->read_c);
                if (!dr.has_value())
                    return sluice::make_unexpected<CopyStats>(
                        dr.error());
            }
            if (s->write_c.outstanding()) {
                auto dr = await_drain(ctx, s->write_c);
                if (!dr.has_value())
                    return sluice::make_unexpected<CopyStats>(
                        dr.error());
            }
        }

        if (primary_error.has_value())
            return sluice::make_unexpected<CopyStats>(
                primary_error.value());

        if (sync == sluice_copy::SyncPolicy::data) {
            auto ssr = ctx.submit_sync_data(
                sluice::async::SyncDataOp{dst_fd}, sync_c);
            if (!ssr.has_value())
                return sluice::make_unexpected<CopyStats>(ssr.error());
            auto sr = await_take(ctx, sync_c);
            if (!sr.has_value())
                return sluice::make_unexpected<CopyStats>(sr.error());
        } else if (sync == sluice_copy::SyncPolicy::all) {
            auto ssr = ctx.submit_sync_all(
                sluice::async::SyncAllOp{dst_fd}, sync_c);
            if (!ssr.has_value())
                return sluice::make_unexpected<CopyStats>(ssr.error());
            auto sr = await_take(ctx, sync_c);
            if (!sr.has_value())
                return sluice::make_unexpected<CopyStats>(sr.error());
        }
        sync_c.reset();
        return stats;
    }
};

// ---------------------------------------------------------------------------
// CLI + run driver
// ---------------------------------------------------------------------------

struct Config {
    Module module = Module::natural;
    std::size_t chunk = 1 << 20;
    std::size_t depth = 1;
    std::uint64_t file_bytes = 512ull << 20;
    std::string src, dst;
    bool generate = false;
    bool run = false;
    std::string label;
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* w) -> std::string {
            if (i + 1 >= argc) e1_fatal(w, EINVAL);
            return argv[++i];
        };
        if (a == "--module") {
            std::string p = next("--module");
            if (p == "engine") c.module = Module::engine;
            else if (p == "replica-natural") c.module = Module::natural;
            else if (p == "replica-aligned") c.module = Module::aligned;
            else if (p == "causal-phase16") c.module = Module::causal_phase16;
            else if (p == "causal-aligned64") c.module = Module::causal_aligned64;
            else e1_semantic("bad --module");
        } else if (a == "--chunk") {
            c.chunk = std::strtoull(next("--chunk").c_str(), nullptr, 10);
        } else if (a == "--depth") {
            c.depth = std::strtoull(next("--depth").c_str(), nullptr, 10);
        } else if (a == "--file-bytes") {
            c.file_bytes = std::strtoull(next("--file-bytes").c_str(),
                                         nullptr, 10);
        } else if (a == "--src") {
            c.src = next("--src");
        } else if (a == "--dst") {
            c.dst = next("--dst");
        } else if (a == "--label") {
            c.label = next("--label");
        } else if (a == "--generate") {
            c.generate = true;
        } else if (a == "--run") {
            c.run = true;
        } else {
            e1_semantic("unknown arg");
        }
    }
    return c;
}

void generate_file(const Config& cfg) {
    std::vector<std::byte> master(kBlock);
    auto* w = reinterpret_cast<std::uint64_t*>(master.data());
    for (std::size_t i = 0; i < kBlock / 8; ++i) w[i] = splitmix64(kSeed + i);
    int fd = ::open(cfg.src.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) e1_fatal("open(generate)", errno);
    std::vector<std::byte> chunk(1u << 20);
    for (std::size_t off = 0; off < chunk.size(); off += kBlock)
        std::memcpy(chunk.data() + off, master.data(), kBlock);
    std::uint64_t written = 0;
    while (written < cfg.file_bytes) {
        std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(),
                                    cfg.file_bytes - written));
        ssize_t x = ::write(fd, chunk.data(), n);
        if (x < 0) {
            if (errno == EINTR) continue;
            e1_fatal("write(generate)", errno);
        }
        written += static_cast<std::uint64_t>(x);
    }
    if (::close(fd) != 0) e1_fatal("close(generate)", errno);
}

int run_one(const Config& cfg) {
    if (cfg.chunk == 0 || cfg.depth == 0)
        e1_semantic("chunk/depth must be > 0");
    const std::uint64_t chunks =
        (cfg.file_bytes + cfg.chunk - 1) / cfg.chunk;  // ceil

    struct rusage ru0, ru1;
    if (::getrusage(RUSAGE_SELF, &ru0) != 0)
        e1_fatal("getrusage(before)", errno);

    int src_fd = ::open(cfg.src.c_str(), O_RDONLY);
    if (src_fd < 0) e1_fatal("open(src)", errno);
    int dst_fd = ::open(cfg.dst.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) e1_fatal("open(dst)", errno);

    std::uint64_t construct_ns = 0;
    std::uint64_t engine_ns = 0;
    CopyStats st{};
    std::vector<std::uint64_t> base_mods;
    std::vector<std::uint64_t> exposed_mods;
    std::vector<std::uint64_t> residuals;

    if (cfg.module == Module::engine) {
        std::uint64_t t0 = now_ns();
        auto res = sluice_copy::run_pipelined_copy_with_backend(
            src_fd, dst_fd, cfg.chunk, cfg.depth, /*workers=*/1,
            sluice_copy::SyncPolicy::none,
            std::make_unique<sluice::async::ThreadPoolBackend>());
        engine_ns = now_ns() - t0;
        if (!res.has_value()) e1_semantic("engine copy failed");
        st = res.value();
    } else {
        // Slot construction BEFORE the Runtime — the production order
        // (copy_task.cpp builds all slots before run_task_to_result).
        const SlotGeometry geometry =
            (cfg.module == Module::natural)          ? SlotGeometry::natural
            : (cfg.module == Module::aligned)        ? SlotGeometry::aligned64
            : (cfg.module == Module::causal_phase16) ? SlotGeometry::causal_phase16
                                                     : SlotGeometry::causal_aligned64;
        std::uint64_t t0 = now_ns();
        std::vector<std::unique_ptr<ReplicaSlot>> slots;
        try {
            slots.reserve(cfg.depth);
            for (std::size_t i = 0; i < cfg.depth; ++i) {
                auto s = std::make_unique<ReplicaSlot>(cfg.chunk, geometry);
                s->chunk_offset =
                    static_cast<std::uint64_t>(i) * cfg.chunk;
                slots.push_back(std::move(s));
            }
        } catch (const std::bad_alloc&) {
            e1_semantic("slot construction bad_alloc");
        }
        construct_ns = now_ns() - t0;
        // Bench-side causal address gate (AMENDMENT 2, fail-closed): the
        // backing must be page-aligned and the exposed phase must equal
        // the arm's contract (+16 / 0).
        if (cfg.module == Module::causal_phase16 ||
            cfg.module == Module::causal_aligned64) {
            const std::uint64_t want =
                (cfg.module == Module::causal_phase16) ? kCausalPhaseOffset : 0;
            for (const auto& s : slots) {
                const std::uintptr_t base =
                    reinterpret_cast<std::uintptr_t>(s->base_ptr);
                const std::uintptr_t exposed =
                    reinterpret_cast<std::uintptr_t>(s->storage);
                if (base % kPage != 0)
                    e1_semantic("causal backing not page-aligned");
                if (exposed % kPage != want)
                    e1_semantic("causal exposed phase mismatch");
            }
        }
        for (const auto& s : slots) {
            const std::uintptr_t base =
                reinterpret_cast<std::uintptr_t>(s->base_ptr);
            const std::uintptr_t exposed =
                reinterpret_cast<std::uintptr_t>(s->storage);
            base_mods.push_back(base % kPage);
            exposed_mods.push_back(exposed % kPage);
            residuals.push_back(exposed % 64);
        }

        ReplicaCopyTask task{src_fd,        dst_fd, cfg.chunk,
                             cfg.depth,     sluice_copy::SyncPolicy::none,
                             geometry, std::move(slots), {}};
        std::uint64_t t1 = now_ns();
        auto res = sluice::async::run_task_to_result<sluice_copy::CopyStats>(
            /*workers=*/1,
            std::make_unique<sluice::async::ThreadPoolBackend>(), task);
        engine_ns = now_ns() - t1;
        if (!res.has_value()) e1_semantic("replica copy failed");
        st = res.value();
    }

    // Same-work gates (prereg §7) — fail closed.
    if (st.bytes_copied != cfg.file_bytes)
        e1_semantic("bytes_copied != file size");
    if (st.write_ops != chunks) e1_semantic("write_ops != ceil(bytes/chunk)");
    if (st.read_ops < chunks || st.read_ops > chunks + cfg.depth)
        e1_semantic("read_ops out of [ceil, ceil+depth]");
    if (st.short_writes != 0) e1_semantic("short_writes != 0");

    if (::close(src_fd) != 0) e1_fatal("close(src)", errno);
    if (::close(dst_fd) != 0) e1_fatal("close(dst)", errno);

    if (::getrusage(RUSAGE_SELF, &ru1) != 0)
        e1_fatal("getrusage(after)", errno);
    const std::uint64_t utime_us =
        static_cast<std::uint64_t>(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) *
            1000000ull +
        static_cast<std::uint64_t>(ru1.ru_utime.tv_usec -
                                   ru0.ru_utime.tv_usec);
    const std::uint64_t stime_us =
        static_cast<std::uint64_t>(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) *
            1000000ull +
        static_cast<std::uint64_t>(ru1.ru_stime.tv_usec -
                                   ru0.ru_stime.tv_usec);

    // ---- one JSON line per run (raw evidence) ----
    char b[2048];
    std::snprintf(b, sizeof(b),
                  "{\"bench\":\"align_e1_bench\",\"label\":\"%s\","
                  "\"module\":\"%s\",\"chunk\":%llu,\"depth\":%llu,"
                  "\"workers\":1,\"file_bytes\":%llu,\"chunks\":%llu,"
                  "\"construct_ns\":%llu,\"engine_ns\":%llu,"
                  "\"total_ns\":%llu,\"bytes_copied\":%llu,"
                  "\"read_ops\":%llu,\"write_ops\":%llu,"
                  "\"short_writes\":%llu,\"slots_residual_mod64\":[",
                  cfg.label.c_str(), module_name(cfg.module),
                  (unsigned long long)cfg.chunk,
                  (unsigned long long)cfg.depth,
                  (unsigned long long)cfg.file_bytes,
                  (unsigned long long)chunks,
                  (unsigned long long)construct_ns,
                  (unsigned long long)engine_ns,
                  (unsigned long long)(construct_ns + engine_ns),
                  (unsigned long long)st.bytes_copied,
                  (unsigned long long)st.read_ops,
                  (unsigned long long)st.write_ops,
                  (unsigned long long)st.short_writes);
    std::fputs(b, stdout);
    for (std::size_t i = 0; i < residuals.size(); ++i) {
        std::printf("%s%llu", i ? "," : "",
                    (unsigned long long)residuals[i]);
    }
    std::printf("],\"slots_base_mod4096\":[");
    for (std::size_t i = 0; i < base_mods.size(); ++i) {
        std::printf("%s%llu", i ? "," : "",
                    (unsigned long long)base_mods[i]);
    }
    std::printf("],\"slots_exposed_mod4096\":[");
    for (std::size_t i = 0; i < exposed_mods.size(); ++i) {
        std::printf("%s%llu", i ? "," : "",
                    (unsigned long long)exposed_mods[i]);
    }
    std::printf("],\"utime_us\":%llu,\"stime_us\":%llu,\"ok\":true}\n",
                (unsigned long long)utime_us,
                (unsigned long long)stime_us);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    if (cfg.src.empty()) e1_semantic("--src required");
    if (cfg.generate) {
        generate_file(cfg);
        std::printf("generated %s (%llu bytes)\n", cfg.src.c_str(),
                    (unsigned long long)cfg.file_bytes);
        return 0;
    }
    if (cfg.dst.empty()) e1_semantic("--dst required");
    if (!cfg.run) e1_semantic("--run required (or --generate)");
    return run_one(cfg);
}