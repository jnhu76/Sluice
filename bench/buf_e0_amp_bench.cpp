// BUF-E0 application amplifier (#263 Phase 2, prereg §8): the realistic
// PipelineSlot lifecycle end-to-end, with slot storage as the only variable.
//
//   --arm engine-b0    the REAL production engine
//                      (run_pipelined_copy_with_backend — the same
//                      copy_task.cpp the CLI uses). External consistency
//                      reference; slot construction is inside the engine
//                      call and not separated.
//   --arm replica-b0   research replica of PipelinedCopyTask (verbatim
//                      algorithm) with std::vector<std::byte> slots — the
//                      production representation. Validates replica fidelity
//                      against engine-b0.
//   --arm replica-b1   the same replica with uninitialized owned storage
//                      (std::make_unique_for_overwrite<std::byte[]>).
//
// Production code is NOT modified (research-only replica). Per rep: fresh
// slots are constructed (replica arms; timed separately), then the full
// engine call is timed (Runtime build/start/submit/wait/drain/join + the
// copy). Same-work gates (fail-closed): bytes_copied == file size,
// write_ops == chunks, read_ops in [chunks, chunks+depth], short_writes
// == 0; the runner additionally compares src/dst hashes post-exit.
//
// Workload bytes: the TAX-0-line generator (4 KiB splitmix64 master block,
// kSeed 0xE1E1E1E121212121), generated once by --generate.

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
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;
using CopyStats = sluice_copy::CopyStats;

[[noreturn]] void amp_fatal(const char* what, int err) {
    std::fprintf(stderr, "buf_e0_amp_bench: fatal: %s (errno=%d: %s)\n",
                 what, err, std::strerror(err));
    std::exit(2);
}

[[noreturn]] void amp_semantic(const char* what) {
    std::fprintf(stderr, "buf_e0_amp_bench: semantic failure: %s\n", what);
    std::exit(3);
}

constexpr std::size_t kBlock = 4096;
constexpr std::uint64_t kSeed = 0xE1E1E1E121212121ull;

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

enum class Arm { engine_b0, replica_b0, replica_b1, replica_b3 };

// Replica storage selection — the ONLY delta vs production PipelineSlot
// (AMENDMENT 1: replica-b3 added per the prereg §8 swap rule).
enum class ReplicaStorage { b0_vector, b1_uninit, b3_aligned };

const char* arm_name(Arm a) {
    switch (a) {
    case Arm::engine_b0: return "engine-b0";
    case Arm::replica_b0: return "replica-b0";
    case Arm::replica_b1: return "replica-b1";
    case Arm::replica_b3: return "replica-b3";
    }
    return "?";
}

constexpr bool add_would_overflow(std::uint64_t a, std::uint64_t b) noexcept {
    return a > 0xFFFFFFFFFFFFFFFFull - b;
}

// ===========================================================================
// Replica of the production Version B pipeline (apps/sluice-copy/
// copy_task.cpp PipelinedCopyTask). VERBATIM algorithm; the ONLY delta is
// the selectable slot storage (ReplicaSlot). Keep this in lockstep with the
// production file when comparing results; the replica-b0 vs engine-b0 arm
// pair exists precisely to detect any drift.
// ===========================================================================

enum class SlotState : std::uint8_t {
    idle, reading, read_done, writing, done,
};

struct ReplicaSlot {
    // Storage variant — the ONLY delta vs production PipelineSlot.
    std::vector<std::byte> b0_buffer;        // replica-b0: production repr
    std::unique_ptr<std::byte[]> b1_buffer;  // replica-b1: uninitialized
    void* b3_ptr = nullptr;                  // replica-b3: page-aligned owned
    std::byte* storage = nullptr;            // active pointer (L7 stable)
    sluice::async::Completion<std::size_t> read_c;
    sluice::async::Completion<std::size_t> write_c;
    std::uint64_t chunk_offset = 0;
    std::size_t filled = 0;
    std::size_t written = 0;
    bool eof = false;
    SlotState state = SlotState::idle;

    explicit ReplicaSlot(std::size_t cap, ReplicaStorage st) {
        switch (st) {
        case ReplicaStorage::b0_vector:
            b0_buffer.resize(cap);  // value-init, same as vector(cap)
            storage = b0_buffer.data();
            break;
        case ReplicaStorage::b1_uninit:
            b1_buffer = std::make_unique_for_overwrite<std::byte[]>(cap);
            storage = b1_buffer.get();
            break;
        case ReplicaStorage::b3_aligned:
            if (::posix_memalign(&b3_ptr, 4096, cap) != 0)
                throw std::bad_alloc();
            storage = static_cast<std::byte*>(b3_ptr);
            break;
        }
    }

    ~ReplicaSlot() {
        if (b3_ptr) ::free(b3_ptr);
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
    ReplicaStorage storage_kind;  // the ONLY delta vs the production task
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
// Rep driver
// ---------------------------------------------------------------------------

struct Config {
    Arm arm = Arm::replica_b0;
    std::size_t buffer_size = 1 << 20;
    std::size_t depth = 8;
    unsigned workers = 1;  // production CLI default
    std::size_t reps = 7;
    std::uint64_t file_bytes = 512ull << 20;
    std::string src, dst;
    bool generate = false;
    std::string label;
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* w) -> std::string {
            if (i + 1 >= argc) amp_fatal(w, EINVAL);
            return argv[++i];
        };
        if (a == "--arm") {
            std::string p = next("--arm");
            if (p == "engine-b0") c.arm = Arm::engine_b0;
            else if (p == "replica-b0") c.arm = Arm::replica_b0;
            else if (p == "replica-b1") c.arm = Arm::replica_b1;
            else if (p == "replica-b3") c.arm = Arm::replica_b3;
            else amp_semantic("bad --arm");
        } else if (a == "--buffer-size") {
            c.buffer_size = std::strtoull(next("--buffer-size").c_str(), nullptr, 10);
        } else if (a == "--depth") {
            c.depth = std::strtoull(next("--depth").c_str(), nullptr, 10);
        } else if (a == "--workers") {
            c.workers = static_cast<unsigned>(
                std::strtoul(next("--workers").c_str(), nullptr, 10));
        } else if (a == "--reps") {
            c.reps = std::strtoull(next("--reps").c_str(), nullptr, 10);
        } else if (a == "--file-bytes") {
            c.file_bytes = std::strtoull(next("--file-bytes").c_str(), nullptr, 10);
        } else if (a == "--src") {
            c.src = next("--src");
        } else if (a == "--dst") {
            c.dst = next("--dst");
        } else if (a == "--label") {
            c.label = next("--label");
        } else if (a == "--generate") {
            c.generate = true;
        } else {
            amp_semantic("unknown arg");
        }
    }
    return c;
}

void generate_file(const Config& cfg) {
    std::vector<std::byte> master(kBlock);
    auto* w = reinterpret_cast<std::uint64_t*>(master.data());
    for (std::size_t i = 0; i < kBlock / 8; ++i) w[i] = splitmix64(kSeed + i);
    int fd = ::open(cfg.src.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) amp_fatal("open(generate)", errno);
    std::vector<std::byte> chunk(1u << 20);
    for (std::size_t off = 0; off < chunk.size(); off += kBlock)
        std::memcpy(chunk.data() + off, master.data(), kBlock);
    std::uint64_t written = 0;
    while (written < cfg.file_bytes) {
        std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(), cfg.file_bytes - written));
        ssize_t x = ::write(fd, chunk.data(), n);
        if (x < 0) {
            if (errno == EINTR) continue;
            amp_fatal("write(generate)", errno);
        }
        written += static_cast<std::uint64_t>(x);
    }
    if (::close(fd) != 0) amp_fatal("close(generate)", errno);
}

struct RepResult {
    std::uint64_t construct_ns = 0;
    std::uint64_t engine_ns = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t bytes_copied = 0;
    std::uint64_t read_ops = 0;
    std::uint64_t write_ops = 0;
    std::uint64_t short_writes = 0;
    bool ok = false;
};

int run_amp(const Config& cfg) {
    const std::uint64_t chunks = cfg.file_bytes / cfg.buffer_size;
    std::vector<RepResult> reps;

    for (std::size_t rep = 0; rep < cfg.reps; ++rep) {
        int src_fd = ::open(cfg.src.c_str(), O_RDONLY);
        if (src_fd < 0) amp_fatal("open(src)", errno);
        int dst_fd = ::open(cfg.dst.c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst_fd < 0) amp_fatal("open(dst)", errno);

        RepResult r;
        if (cfg.arm == Arm::engine_b0) {
            std::uint64_t t0 = now_ns();
            auto res = sluice_copy::run_pipelined_copy_with_backend(
                src_fd, dst_fd, cfg.buffer_size, cfg.depth, cfg.workers,
                sluice_copy::SyncPolicy::none,
                std::make_unique<sluice::async::ThreadPoolBackend>());
            r.engine_ns = now_ns() - t0;
            r.total_ns = r.engine_ns;
            if (!res.has_value())
                amp_semantic("engine copy failed");
            r.bytes_copied = res.value().bytes_copied;
            r.read_ops = res.value().read_ops;
            r.write_ops = res.value().write_ops;
            r.short_writes = res.value().short_writes;
        } else {
            const ReplicaStorage st =
                cfg.arm == Arm::replica_b1 ? ReplicaStorage::b1_uninit
                : cfg.arm == Arm::replica_b3 ? ReplicaStorage::b3_aligned
                                             : ReplicaStorage::b0_vector;
            // Slot construction BEFORE the Runtime — the production order
            // (copy_task.cpp builds all slots before run_task_to_result).
            std::uint64_t t0 = now_ns();
            std::vector<std::unique_ptr<ReplicaSlot>> slots;
            try {
                slots.reserve(cfg.depth);
                for (std::size_t i = 0; i < cfg.depth; ++i) {
                    auto s = std::make_unique<ReplicaSlot>(cfg.buffer_size,
                                                           st);
                    s->chunk_offset =
                        static_cast<std::uint64_t>(i) * cfg.buffer_size;
                    slots.push_back(std::move(s));
                }
            } catch (const std::bad_alloc&) {
                amp_semantic("slot construction bad_alloc");
            }
            r.construct_ns = now_ns() - t0;

            ReplicaCopyTask task{src_fd,        dst_fd,  cfg.buffer_size,
                                 cfg.depth,     sluice_copy::SyncPolicy::none,
                                 st,            std::move(slots),
                                 {}};
            std::uint64_t t1 = now_ns();
            auto res = sluice::async::run_task_to_result<sluice_copy::CopyStats>(
                cfg.workers,
                std::make_unique<sluice::async::ThreadPoolBackend>(), task);
            r.engine_ns = now_ns() - t1;
            r.total_ns = r.construct_ns + r.engine_ns;
            if (!res.has_value()) amp_semantic("replica copy failed");
            r.bytes_copied = res.value().bytes_copied;
            r.read_ops = res.value().read_ops;
            r.write_ops = res.value().write_ops;
            r.short_writes = res.value().short_writes;
        }

        // Same-work gates (prereg §8)
        if (r.bytes_copied != cfg.file_bytes)
            amp_semantic("bytes_copied != file size");
        if (r.write_ops != chunks) amp_semantic("write_ops != chunks");
        if (r.read_ops < chunks || r.read_ops > chunks + cfg.depth)
            amp_semantic("read_ops out of [chunks, chunks+depth]");
        if (r.short_writes != 0) amp_semantic("short_writes != 0");
        r.ok = true;
        reps.push_back(r);

        ::close(src_fd);
        ::close(dst_fd);
    }

    // ---- JSON out ----
    auto median_of = [](std::vector<std::uint64_t>& v) -> std::uint64_t {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    auto vals = [&](auto field) {
        std::vector<std::uint64_t> v;
        for (const auto& r : reps) v.push_back(field(r));
        return v;
    };
    std::string out = "{\n";
    out += "  \"bench\": \"buf_e0_amp_bench\",\n";
    out += std::string("  \"arm\": \"") + arm_name(cfg.arm) + "\",\n";
    out += "  \"buffer_size\": " + std::to_string(cfg.buffer_size) + ",\n";
    out += "  \"depth\": " + std::to_string(cfg.depth) + ",\n";
    out += "  \"workers\": " + std::to_string(cfg.workers) + ",\n";
    out += "  \"reps\": " + std::to_string(cfg.reps) + ",\n";
    out += "  \"file_bytes\": " + std::to_string(cfg.file_bytes) + ",\n";
    out += "  \"chunks\": " + std::to_string(chunks) + ",\n";
    out += "  \"ops_for_diff\": " + std::to_string(cfg.reps) + ",\n";
    {
        auto c = vals([](const RepResult& r) { return r.construct_ns; });
        auto e = vals([](const RepResult& r) { return r.engine_ns; });
        auto t = vals([](const RepResult& r) { return r.total_ns; });
        auto m = median_of(c);
        out += "  \"construct_ns\": {\"median\": " + std::to_string(m) +
               ", \"n\": " + std::to_string(c.size()) + "},\n";
        m = median_of(e);
        out += "  \"engine_ns\": {\"median\": " + std::to_string(m) +
               ", \"n\": " + std::to_string(e.size()) + "},\n";
        m = median_of(t);
        out += "  \"total_ns\": {\"median\": " + std::to_string(m) +
               ", \"n\": " + std::to_string(t.size()) + "},\n";
    }
    out += "  \"reps_detail\": [\n";
    for (std::size_t i = 0; i < reps.size(); ++i) {
        char b[384];
        std::snprintf(b, sizeof(b),
                      "    {\"rep\": %llu, \"construct_ns\": %llu, "
                      "\"engine_ns\": %llu, \"total_ns\": %llu, "
                      "\"bytes_copied\": %llu, \"read_ops\": %llu, "
                      "\"write_ops\": %llu, \"short_writes\": %llu, "
                      "\"ok\": %s}%s\n",
                      (unsigned long long)i,
                      (unsigned long long)reps[i].construct_ns,
                      (unsigned long long)reps[i].engine_ns,
                      (unsigned long long)reps[i].total_ns,
                      (unsigned long long)reps[i].bytes_copied,
                      (unsigned long long)reps[i].read_ops,
                      (unsigned long long)reps[i].write_ops,
                      (unsigned long long)reps[i].short_writes,
                      reps[i].ok ? "true" : "false",
                      (i + 1 < reps.size()) ? "," : "");
        out += b;
    }
    out += "  ],\n";
    out += "  \"all_reps_ok\": true\n}\n";
    std::fputs(out.c_str(), stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    if (cfg.src.empty()) amp_semantic("--src required");
    if (cfg.generate) {
        generate_file(cfg);
        std::printf("generated %s (%llu bytes)\n", cfg.src.c_str(),
                    (unsigned long long)cfg.file_bytes);
        return 0;
    }
    if (cfg.dst.empty()) amp_semantic("--dst required");
    return run_amp(cfg);
}
