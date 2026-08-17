// Issue #116 liveness forensics reproducer (investigation Phase 2).
//
// Probabilistic stress reproducer with IN-PROCESS state capture. Drives the
// same Runtime/ThreadPoolBackend pipelined-await structure as
// apps/sluice-copy (submit a read window, await the lowest-offset read,
// write, recycle — one fiber, identity Completion waits only, no external
// wake sources) with workers=1, the shape of the captured CI hang family.
//
// Each round builds a FRESH ApplicationRuntime (like the integration test's
// per-case runtimes). A per-round watchdog parks with a 20s bound on the
// round's done flag; on timeout — the run is presumed permanently stalled —
// it dumps the race-free ApplicationRuntime + Scheduler + backend state and
// exits 42 so an outer harness can count hangs WITH evidence:
//
//   SLUICE_TEST_FILTER=issue116_pipeline_forensics_starvation \
//   taskset -c 0-3 <binary>   # 3 CPU spinners on CPUs 0-3 alongside
//
// This file is internal-testing only: it uses test_dump_forensics and the
// ThreadPoolBackend *_for_test accessors. It is diagnosis tooling for the
// investigation, NOT the deterministic merge-gate regression (that lands
// after the interleaving is proven; see the investigation report).
#include "harness.hpp"

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

constexpr std::size_t kBuffer = 4096;

struct TempPath {
    explicit TempPath(const char* tag) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_issue116_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + ".tmp"))
                    .string();
    }
    ~TempPath() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

int open_temp(const std::string& path, bool truncate) {
    int flags = O_RDWR | O_CREAT | (truncate ? O_TRUNC : 0);
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "open_temp failed\n");
        std::exit(1);
    }
    return fd;
}

// One pipelined copy slot: fixed buffer + address-stable read/write
// Completions (the same L7 shape as PipelinedCopyTask). `read_pending` is the
// slot's OWN bookkeeping of an un-awaited read — a Completion that was reaped
// ready by a worker poll BEFORE this fiber awaited it is `ready()` but NOT
// `outstanding()`, so Completion state alone cannot drive the pipeline (the
// real copy task tracks SlotState for the same reason).
struct Slot {
    std::vector<std::byte> buffer;
    Completion<std::size_t> read_c;
    Completion<std::size_t> write_c;
    std::uint64_t off = 0;
    bool read_pending = false;
    Slot() = default;
    explicit Slot(std::size_t cap) : buffer(cap) {}
};

// Compact Version-B-style pipelined copy: submit `depth` reads, repeatedly
// await the lowest-offset read, write it out, recycle the slot to the next
// chunk until EOF. Structurally identical to the copy app's steady state:
// one fiber, identity Completion waits, overlapping outstanding reads.
struct ForensicsCopyTask {
    int src_fd;
    int dst_fd;
    std::size_t depth;
    std::uint64_t file_size;
    std::vector<std::unique_ptr<Slot>> slots;

    std::mutex& mtx;
    std::atomic<bool>& done;
    std::condition_variable& done_cv;
    std::optional<Result<std::uint64_t>>& out;

    void operator()(RuntimeTaskContext& ctx) {
        Result<std::uint64_t> r = run(ctx);
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!done.load(std::memory_order::relaxed)) {
                out = std::move(r);
                done.store(true, std::memory_order::release);
            }
        }
        done_cv.notify_all();
    }

    Result<std::uint64_t> run(RuntimeTaskContext& ctx) {
        try {
            return body(ctx);
        } catch (...) {
            return make_unexpected<std::uint64_t>(
                IoError{IoError::Code::backend_error});
        }
    }

    Result<std::uint64_t> body(RuntimeTaskContext& ctx) {
        std::uint64_t copied = 0;
        // Initial read window.
        for (std::size_t i = 0; i < depth; ++i) {
            auto& s = slots[i];
            if (s->off >= file_size) continue;
            std::size_t len = static_cast<std::size_t>(
                std::min<std::uint64_t>(kBuffer, file_size - s->off));
            auto r = ctx.submit_read(
                ReadOp{src_fd, s->buffer.data(), len, s->off}, s->read_c);
            if (!r.has_value()) return make_unexpected<std::uint64_t>(r.error());
            s->read_pending = true;
        }
        for (;;) {
            // Lowest-offset slot with a pending (submitted, un-awaited) read.
            Slot* s = nullptr;
            for (auto& c : slots) {
                if (!c->read_pending) continue;
                if (s == nullptr || c->off < s->off) s = c.get();
            }
            if (s == nullptr) break;  // all reads drained; done
            s->read_pending = false;
            auto aw = ctx.await_completion(s->read_c);
            if (!aw.has_value())
                return make_unexpected<std::uint64_t>(aw.error());
            auto rr = s->read_c.result();
            s->read_c.reset();
            if (!rr.has_value())
                return make_unexpected<std::uint64_t>(rr.error());
            std::size_t n = rr.value();
            if (n == 0) continue;  // EOF slot
            // Write this chunk out.
            std::size_t written = 0;
            while (written < n) {
                auto w = ctx.submit_write(
                    WriteOp{dst_fd, s->buffer.data() + written, n - written,
                            s->off + written},
                    s->write_c);
                if (!w.has_value())
                    return make_unexpected<std::uint64_t>(w.error());
                auto ww = ctx.await_completion(s->write_c);
                if (!ww.has_value())
                    return make_unexpected<std::uint64_t>(ww.error());
                auto wr = s->write_c.result();
                s->write_c.reset();
                if (!wr.has_value())
                    return make_unexpected<std::uint64_t>(wr.error());
                if (wr.value() == 0)
                    return make_unexpected<std::uint64_t>(
                        IoError{IoError::Code::backend_error});
                written += wr.value();
            }
            copied += n;
            // Recycle: next chunk for this slot.
            s->off += kBuffer * depth;
            if (s->off < file_size) {
                std::size_t len = static_cast<std::size_t>(
                    std::min<std::uint64_t>(kBuffer, file_size - s->off));
                auto r = ctx.submit_read(
                    ReadOp{src_fd, s->buffer.data(), len, s->off}, s->read_c);
                if (!r.has_value())
                    return make_unexpected<std::uint64_t>(r.error());
                s->read_pending = true;
            }
        }
        return copied;
    }
};

// Per-round watchdog: bounded park on the round's done flag; on timeout dumps
// the race-free Runtime/Scheduler/backend state and exits 42 (hang-with-
// evidence). The dump is safe at a permanent stall: the driver parks on
// runtime_cv_ (lifecycle_mtx_ released by the cv wait), workers have exited,
// backend workers idle on work_cv_.
class RoundWatchdog {
public:
    ~RoundWatchdog() { join(); }

    void arm(int round, ApplicationRuntime* rt, ThreadPoolBackend* be) {
        round_ = round;
        rt_ = rt;
        be_ = be;
        done_ = false;
        th_ = std::thread([this] { watch(); });
    }

    void disarm() {
        {
            std::lock_guard<std::mutex> lk(m_);
            done_ = true;
        }
        cv_.notify_all();
        join();
    }

private:
    void join() {
        if (th_.joinable()) th_.join();
    }

    void watch() {
        std::unique_lock<std::mutex> lk(m_);
        if (cv_.wait_until(lk, std::chrono::steady_clock::now() +
                                   std::chrono::seconds(20),
                           [this] { return done_; })) {
            return;  // round completed inside the bound
        }
        lk.unlock();
        char tag[32];
        std::snprintf(tag, sizeof(tag), "round-%d", round_);
        std::fprintf(stderr, "=== issue116 HANG %s (20s bound) ===\n", tag);
        rt_->test_dump_forensics(tag);
        std::fprintf(stderr,
                     "[issue116-forensics] backend %s: dispatch=%zu "
                     "active_workers=%zu backend_ready=%zu syscalls=%llu\n",
                     tag, be_->dispatch_size_for_test(),
                     be_->active_workers_for_test(),
                     be_->backend_ready_count_for_test(),
                     static_cast<unsigned long long>(
                         be_->syscall_count_for_test()));
        std::fflush(stderr);
        std::_Exit(42);  // skip atexit/static destructors: the world is stalled
    }

    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false;
    int round_ = 0;
    ApplicationRuntime* rt_ = nullptr;
    ThreadPoolBackend* be_ = nullptr;
};

}  // namespace

SLUICE_MAIN()

SLUICE_TEST_CASE(issue116_pipeline_forensics_starvation) {
    TempPath src_path("src");
    TempPath dst_path("dst");
    const int src_fd = open_temp(src_path.path(), true);
    const int dst_fd = open_temp(dst_path.path(), true);

    // Fixed pattern file (1 MiB + 7 bytes: not chunk-aligned).
    const std::uint64_t kFileSize = 25 * kBuffer * 10 + 7;
    {
        std::vector<std::byte> pat(kBuffer);
        for (std::size_t i = 0; i < kBuffer; ++i) {
            pat[i] = static_cast<std::byte>((i * 131 + 17) & 0xFF);
        }
        std::uint64_t written = 0;
        while (written < kFileSize) {
            std::size_t len = static_cast<std::size_t>(
                std::min<std::uint64_t>(kBuffer, kFileSize - written));
            if (::write(src_fd, pat.data(), len) != static_cast<ssize_t>(len)) {
                SLUICE_FAIL("setup write failed");
            }
            written += len;
        }
    }

    RoundWatchdog watchdog;
    int round = 0;
    // The same edge-size-per-depth matrix as the integration test's
    // pipeline_integration_edge_sizes_per_depth (the captured CI hang family):
    // sizes 0, 1, B-1, B, B+1, depth*B, depth*B+1, 3*depth*B+7 across depths
    // 1,2,3,4,8 — a fresh Runtime + backend per case, one process.
    const std::size_t kBuf = kBuffer;
    for (std::size_t depth : {1u, 2u, 3u, 4u, 8u}) {
        for (std::uint64_t size : {std::uint64_t{0}, std::uint64_t{1},
                                   static_cast<std::uint64_t>(kBuf) - 1,
                                   static_cast<std::uint64_t>(kBuf),
                                   static_cast<std::uint64_t>(kBuf) + 1,
                                   static_cast<std::uint64_t>(kBuf) * depth,
                                   static_cast<std::uint64_t>(kBuf) * depth + 1,
                                   static_cast<std::uint64_t>(kBuf) * depth * 3 + 7}) {
            ++round;
            const std::uint64_t file_size = size;
            auto backend = std::make_unique<ThreadPoolBackend>();
            ThreadPoolBackend* raw_be = backend.get();

            RuntimeBuilder builder;
            builder.backend(std::move(backend));
            builder.workers(1);
            auto build_r = builder.build();
            SLUICE_CHECK(build_r.has_value());
            std::unique_ptr<ApplicationRuntime> rt =
                std::move(build_r.value());
            SLUICE_CHECK(rt->start().has_value());

            std::mutex mtx;
            std::condition_variable done_cv;
            std::atomic<bool> done{false};
            std::optional<Result<std::uint64_t>> out;

            std::vector<std::unique_ptr<Slot>> slots;
            slots.reserve(depth);
            for (std::size_t i = 0; i < depth; ++i) {
                auto s = std::make_unique<Slot>(kBuffer);
                s->off = static_cast<std::uint64_t>(i) * kBuffer;
                slots.push_back(std::move(s));
            }
            ForensicsCopyTask task{src_fd,  dst_fd,     depth, file_size,
                                   std::move(slots), mtx, done, done_cv, out};

            watchdog.arm(round, rt.get(), raw_be);
            SLUICE_CHECK(rt->submit(std::ref(task)).has_value());

            {
                std::unique_lock<std::mutex> wlk(mtx);
                done_cv.wait(wlk,
                             [&] { return done.load(std::memory_order::acquire); });
            }
            watchdog.disarm();

            // Teardown FIRST, then checks: a failed check's early return must
            // not skip request_stop/drain/join (~ApplicationRuntime fail-fasts
            // on a not-Stopped state).
            rt->request_stop();
            const bool drain_ok = rt->drain().has_value();
            const bool join_ok = rt->join().has_value();

            std::fprintf(stderr, "[round %d depth=%zu] out_present=%d", round,
                         depth, out.has_value() ? 1 : 0);
            if (out.has_value() && out.value().has_value()) {
                std::fprintf(stderr, " copied=%llu",
                             static_cast<unsigned long long>(
                                 out.value().value()));
            } else if (out.has_value()) {
                std::fprintf(stderr, " err=%d",
                             static_cast<int>(out.value().error().code));
            }
            std::fprintf(stderr, " drain=%d join=%d\n", drain_ok ? 1 : 0,
                         join_ok ? 1 : 0);
            std::fflush(stderr);

            SLUICE_CHECK(drain_ok);
            SLUICE_CHECK(join_ok);
            SLUICE_CHECK(out.has_value());
            if (out.has_value() && out.value().has_value()) {
                SLUICE_CHECK_MSG(out.value().value() == file_size,
                                 "copied byte count mismatch");
            } else if (out.has_value()) {
                SLUICE_FAIL("copy task failed");
            }
        }
    }

    // Verify the last round's destination bytes. Exact-read loops: a regular
    // file read() may legitimately return short; comparing the raw return
    // against len would misreport a correct copy.
    {
        std::vector<std::byte> a(kBuffer), b(kBuffer);
        std::uint64_t off = 0;
        ::lseek(src_fd, 0, SEEK_SET);
        ::lseek(dst_fd, 0, SEEK_SET);
        auto read_exact = [](int fd, std::byte* dst, std::size_t len) -> bool {
            std::size_t done = 0;
            while (done < len) {
                ssize_t r = ::read(fd, dst + done, len - done);
                if (r <= 0) return false;
                done += static_cast<std::size_t>(r);
            }
            return true;
        };
        while (off < kFileSize) {
            std::size_t len = static_cast<std::size_t>(
                std::min<std::uint64_t>(kBuffer, kFileSize - off));
            if (!read_exact(src_fd, a.data(), len)) break;
            if (!read_exact(dst_fd, b.data(), len)) break;
            SLUICE_CHECK(std::memcmp(a.data(), b.data(), len) == 0);
            off += len;
        }
    }

    ::close(src_fd);
    ::close(dst_fd);
}
