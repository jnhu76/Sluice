// sluice-copy Version B pipeline integration tests.
//
// Real temporary files + ThreadPoolBackend. Drives the SAME copy_task code the
// CLI uses (apps/sluice-copy/copy_task.cpp) via run_pipelined_copy[_with_backend],
// asserting end-to-end correctness that the scripted-backend contract tests
// cannot (the scripted backend performs no real I/O, so it cannot prove byte-
// for-byte content). Covers:
//   - exact source/destination byte equality across many sizes/depths/buffers;
//   - edge sizes 0, 1, buffer-1, buffer, buffer+1, depth*buffer, depth*buffer+1;
//   - depths 1, 2, 3, 4, 8 (depth=1 == Version A);
//   - multi-round slot reuse (file much larger than depth*buffer);
//   - binary data with embedded zeros;
//   - sync none/data/all after a pipelined copy;
//   - a deterministic proof that depth>1 yields >= 2 concurrent reads against
//     ThreadPoolBackend (the real backend), via a best-effort observer.
//
// No sleeps used as proof of ordering; the content-equality checks are the
// proof. copy_task.cpp is compiled into this target alongside the test.
#include "harness.hpp"

#include "copy_task.hpp"  // apps/sluice-copy public header

#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace sluice_copy;
using sluice::IoError;
using sluice::Result;

namespace {

// RAII temp file (anonymous, auto-unlinked).
struct TempFile {
    int fd;
    TempFile() {
        char p[] = "/tmp/sluice_copy_pit_XXXXXX";
        fd = ::mkstemp(p);
        SLUICE_CHECK(fd >= 0);
        ::unlink(p);
    }
    ~TempFile() { if (fd >= 0) ::close(fd); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

// Deterministic byte for a given offset (no /dev/urandom so failures are
// reproducible). Mixes in zeros every 7th byte to exercise embedded zeros.
std::byte byte_at(std::size_t i) {
    unsigned char b = static_cast<unsigned char>((i * 31 + 7) & 0xFF);
    return (i % 7 == 0) ? std::byte{0} : std::byte{b};
}

void seed_file(int fd, std::size_t n) {
    std::vector<std::byte> data(n);
    for (std::size_t i = 0; i < n; ++i) data[i] = byte_at(i);
    if (n > 0) {
        ssize_t w = ::pwrite(fd, data.data(), n, 0);
        SLUICE_CHECK(static_cast<std::size_t>(w) == n);
    }
}

bool files_equal(int a, int b, std::size_t n) {
    if (n == 0) return true;
    std::vector<std::byte> da(n), db(n);
    ssize_t ra = ::pread(a, da.data(), n, 0);
    ssize_t rb = ::pread(b, db.data(), n, 0);
    if (ra != static_cast<ssize_t>(n) || rb != static_cast<ssize_t>(n))
        return false;
    return std::memcmp(da.data(), db.data(), n) == 0;
}

// Copy src->dst at the given buffer/depth/workers/sync and verify the copy
// EXACTLY:
//   - the destination is truncated first (mirroring the CLI), so a pre-existing
//     longer destination cannot hide a stale tail;
//   - fstat both fds: dst_size must equal src_size == total (a byte-compare of
//     the first `total` bytes alone would MISS an extra tail in the dst);
//   - the reported bytes_copied must equal total;
//   - all bytes must be identical.
void check_copy(int src, int dst, std::size_t total, std::size_t buf,
                std::size_t depth, unsigned workers, SyncPolicy sync) {
    // Mirror the CLI contract: the destination is truncated before the copy.
    SLUICE_CHECK(::ftruncate(dst, 0) == 0);

    auto r = run_pipelined_copy(src, dst, buf, depth, workers, sync);
    SLUICE_CHECK_MSG(r.has_value(), "pipelined copy returned an error");
    SLUICE_CHECK(r.value().bytes_copied == total);

    struct stat sst{}, dstt{};
    SLUICE_CHECK(::fstat(src, &sst) == 0);
    SLUICE_CHECK(::fstat(dst, &dstt) == 0);
    SLUICE_CHECK_MSG(sst.st_size == static_cast<off_t>(total),
                     "source size mismatch");
    SLUICE_CHECK_MSG(dstt.st_size == static_cast<off_t>(total),
                     "destination size is not exactly the source size "
                     "(stale tail or lost data)");
    SLUICE_CHECK(files_equal(src, dst, total));
}

}  // namespace

// ---------------------------------------------------------------------------
// A destination that was originally LONGER than the source must end up with
// the exact source size. This proves truncate + pipeline write leaves no
// residual tail (the old byte-compare of the first `total` bytes could not
// catch a stale tail).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(pipeline_integration_destination_originally_longer) {
    constexpr std::size_t SRC_N = 100;
    TempFile src, dst;
    seed_file(src.fd, SRC_N);
    seed_file(dst.fd, 4096);  // destination pre-existing and much longer

    // The CLI truncates the destination before the copy (mirrored by
    // check_copy); the pipeline must then leave it at EXACTLY SRC_N bytes.
    check_copy(src.fd, dst.fd, SRC_N, 4096, /*depth=*/3, 1, SyncPolicy::none);
    struct stat dstt{};
    SLUICE_CHECK(::fstat(dst.fd, &dstt) == 0);
    SLUICE_CHECK(dstt.st_size == static_cast<off_t>(SRC_N));
}

// ---------------------------------------------------------------------------
// Edge sizes at a fixed depth, across depths: 0,1,B-1,B,B+1,depth*B,depth*B+1
// plus a multi-round size (forces slot reuse).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(pipeline_integration_edge_sizes_per_depth) {
    constexpr std::size_t B = 4096;
    for (std::size_t depth : {1u, 2u, 3u, 4u, 8u}) {
        std::size_t sizes[] = {
            0, 1, B - 1, B, B + 1, depth * B, depth * B + 1, depth * B * 3 + 7,
        };
        for (std::size_t n : sizes) {
            TempFile src, dst;
            seed_file(src.fd, n);
            check_copy(src.fd, dst.fd, n, B, depth, 1, SyncPolicy::none);
        }
    }
}

// ---------------------------------------------------------------------------
// Multiple buffer sizes at depth>1, including small buffers that force many
// rounds of slot reuse. The workload is derived PER CASE from the buffer
// size, the pipeline depth, and the number of slot-reuse rounds to cover —
// NOT a single fixed byte count shared by every buffer. The previous uniform
// N=100003 made buf=1 spawn ~100k chunks x (1 read + 1 write) x 2 depths ~=
// 400k OS thread lifecycles in Debug: a de-facto thread soak that took ~48s
// locally, minutes on shared runners (looking like a hang), and exhausted
// kernel task limits on CI. The default matrix now stays in the hundreds of
// ops while preserving every coverage property below.
//
// SLUICE_PIPELINE_BUFSTRESS_N re-opts a run into the explicit large workload
// (Version B nightly / manual stress). It is honored ONLY when it is a
// present, fully-valid positive decimal integer that fits size_t; any other
// value (absent, empty, zero, junk, trailing junk, sign, overflow) falls
// back to the small per-case matrix size, so an invalid override can never
// accidentally amplify a workload (the pre-fix parser fell back to 100003
// even in sanitizer builds).
// ---------------------------------------------------------------------------
namespace {

constexpr std::size_t kBufferMatrixReuseRounds = 8;

// Byte count for one buffer/depth case: pipeline_depth * reuse-rounds + 1
// full chunks, plus a partial final chunk when buffer_size > 1. The `+1`
// chunk guarantees the file spans more than one full pipeline window (multi-
// round slot reuse); the tail proves the last buffer is drained short. The
// constants are tiny by design, so the arithmetic cannot overflow.
std::size_t buffer_matrix_case_size(std::size_t buffer_size,
                                    std::size_t pipeline_depth) {
    const std::size_t chunks =
        pipeline_depth * kBufferMatrixReuseRounds + 1;
    const std::size_t tail = buffer_size > 1 ? buffer_size / 2 : 0;
    return buffer_size * chunks + tail;
}

// Strict override parser (see the comment above). Returns the override only
// when the variable exists and is a fully-valid positive decimal integer
// that fits size_t; nullopt otherwise, so the caller falls back to its
// scenario default.
std::optional<std::size_t> stress_override_n() {
    const char* s = std::getenv("SLUICE_PIPELINE_BUFSTRESS_N");
    if (s == nullptr || *s == '\0' || *s == '+') return std::nullopt;
    std::size_t v = 0;
    bool any_digit = false;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return std::nullopt;  // junk / trailing junk
        const std::size_t d = static_cast<std::size_t>(*p - '0');
        if (v > (SIZE_MAX - d) / 10) return std::nullopt;  // overflow
        v = v * 10 + d;
        any_digit = true;
    }
    return (any_digit && v > 0) ? std::optional<std::size_t>{v}
                                : std::nullopt;
}

// RAII set/unset of a process env var, restoring the previous value on scope
// exit. The harness runs every case in one process, so a leaked variable
// would pollute later cases.
class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value) : name_(name) {
        const char* prev = std::getenv(name);
        had_prev_ = prev != nullptr;
        if (had_prev_) prev_ = prev;
        if (value == nullptr) {
            ::unsetenv(name);
        } else {
            ::setenv(name, value, 1);
        }
    }
    ~ScopedEnvVar() {
        if (had_prev_) {
            ::setenv(name_.c_str(), prev_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }
    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string name_;
    bool had_prev_ = false;
    std::string prev_;
};

}  // namespace

SLUICE_TEST_CASE(pipeline_integration_buffer_sizes) {
    const auto override_n = stress_override_n();

    for (std::size_t buf : {1u, 7u, 512u, 4096u, 65536u}) {
        for (std::size_t depth : {2u, 4u}) {
            const std::size_t n =
                override_n.value_or(buffer_matrix_case_size(buf, depth));

            // Self-check the DEFAULT case size only (an explicit override is
            // the caller's chosen scale, not subject to the matrix coverage
            // contract): the file must span > 4 full pipeline windows
            // (multi-round slot reuse) and, for buf > 1, must end with a
            // partial final chunk. These verify the test input, not the
            // production copy.
            if (!override_n.has_value()) {
                SLUICE_CHECK_MSG(n > buf * depth * 4,
                                 "case too small for multi-round slot reuse");
                SLUICE_CHECK_MSG(buf == 1 || n % buf != 0,
                                 "case size has no partial final chunk");
            }

            // Case-level progress diagnostics: at most two lines per
            // combination, so a slow run shows exactly which buf/depth is
            // in flight (the harness prints/flushes per-case stdout).
            const auto t0 = std::chrono::steady_clock::now();
            std::printf("pipeline buffer case begin: buf=%zu depth=%zu n=%zu\n",
                        buf, depth, n);
            std::fflush(stdout);

            TempFile src, dst;
            seed_file(src.fd, n);
            check_copy(src.fd, dst.fd, n, buf, depth, 1, SyncPolicy::none);

            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
            std::printf("pipeline buffer case done:  buf=%zu depth=%zu n=%zu "
                        "elapsed_ms=%lld\n",
                        buf, depth, n, static_cast<long long>(ms));
        }
    }
}

// ---------------------------------------------------------------------------
// The override parser contract (see stress_override_n): only a present,
// fully-valid positive decimal integer that fits size_t is honored. Absent,
// empty, zero, junk, trailing junk, sign, and overflow must all yield
// nullopt — an invalid override must never accidentally escalate a workload.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(pipeline_stress_override_parsing) {
    constexpr const char* kVar = "SLUICE_PIPELINE_BUFSTRESS_N";

    { ScopedEnvVar env(kVar, nullptr);  // unset
      SLUICE_CHECK(!stress_override_n().has_value()); }
    { ScopedEnvVar env(kVar, "");  // empty
      SLUICE_CHECK(!stress_override_n().has_value()); }
    { ScopedEnvVar env(kVar, "0");  // zero
      SLUICE_CHECK(!stress_override_n().has_value()); }
    { ScopedEnvVar env(kVar, "00");  // zero with padding
      SLUICE_CHECK(!stress_override_n().has_value()); }
    { ScopedEnvVar env(kVar, "abc");  // non-numeric
      SLUICE_CHECK(!stress_override_n().has_value()); }
    { ScopedEnvVar env(kVar, "100abc");  // trailing junk
      SLUICE_CHECK(!stress_override_n().has_value()); }
    { ScopedEnvVar env(kVar, "-100");  // sign
      SLUICE_CHECK(!stress_override_n().has_value()); }
    { ScopedEnvVar env(kVar, "+100");  // sign
      SLUICE_CHECK(!stress_override_n().has_value()); }
    { ScopedEnvVar env(kVar, " 100");  // leading whitespace
      SLUICE_CHECK(!stress_override_n().has_value()); }

    { ScopedEnvVar env(kVar, "1");
      SLUICE_CHECK(stress_override_n().has_value());
      SLUICE_CHECK(stress_override_n().value() == 1); }
    { ScopedEnvVar env(kVar, "100003");
      SLUICE_CHECK(stress_override_n().has_value());
      SLUICE_CHECK(stress_override_n().value() == 100003); }

    // SIZE_MAX itself is valid; SIZE_MAX + 1 must be rejected (no silent
    // truncation). "+1" is an increment of the final digit for every byte-
    // multiple size_t width (2^32-1 = 4294967295, 2^64-1 =
    // 18446744073709551615 — both end in 5), so the strings are computed
    // portably from SIZE_MAX.
    const std::string max_str = std::to_string(SIZE_MAX);
    std::string overflow_str = max_str;
    overflow_str.back() = static_cast<char>(overflow_str.back() + 1);
    { ScopedEnvVar env(kVar, max_str.c_str());
      SLUICE_CHECK(stress_override_n().has_value());
      SLUICE_CHECK(stress_override_n().value() == SIZE_MAX); }
    { ScopedEnvVar env(kVar, overflow_str.c_str());
      SLUICE_CHECK(!stress_override_n().has_value()); }
}

// ---------------------------------------------------------------------------
// Sync policies after a pipelined copy (data reaches the destination file).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(pipeline_integration_sync_policies) {
    constexpr std::size_t N = 4096 * 3;
    for (SyncPolicy sp : {SyncPolicy::none, SyncPolicy::data, SyncPolicy::all}) {
        for (std::size_t depth : {1u, 2u, 3u}) {
            TempFile src, dst;
            seed_file(src.fd, N);
            check_copy(src.fd, dst.fd, N, 4096, depth, 1, sp);
        }
    }
}

// ---------------------------------------------------------------------------
// Multi-worker Runtime + depth>1: correctness still holds with >1 worker.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(pipeline_integration_multi_worker) {
    constexpr std::size_t N = 4096 * 6;
    for (unsigned workers : {1u, 2u, 4u}) {
        for (std::size_t depth : {2u, 4u}) {
            TempFile src, dst;
            seed_file(src.fd, N);
            check_copy(src.fd, dst.fd, N, 4096, depth, workers, SyncPolicy::none);
        }
    }
}

// ---------------------------------------------------------------------------
// Deterministic proof: depth>1 yields >= 2 concurrent reads against the REAL
// ThreadPoolBackend. We use a backend-injected observer that counts distinct
// simultaneously-outstanding read ops by snapshotting under the backend mutex.
//
// ThreadPoolBackend does not expose internal stats, so we wrap it: a thin
// AsyncBackend decorator that delegates every call to a real ThreadPoolBackend
// while counting the peak number of simultaneously-outstanding reads it has
// been asked to submit (submit_read before the matching completion is reaped).
//
// Lifetime: the copy entry point takes OWNERSHIP of the injected backend
// (copy_task.hpp: "the caller supplies the AsyncBackend" — it is destroyed when
// run_pipelined_copy_with_backend returns). Querying the probe object after the
// call would be a use-after-free, so the peak is published into a shared
// atomic cell owned by the test; the probe itself may die with the copy.
// ---------------------------------------------------------------------------
namespace {

class ReadConcurrencyProbe : public sluice::async::AsyncBackend {
public:
    ReadConcurrencyProbe(std::unique_ptr<AsyncBackend> inner,
                         std::shared_ptr<std::atomic<std::size_t>> peak,
                         std::size_t capacity = kTrackedCapacity)
        : inner_(std::move(inner)), peak_(std::move(peak)) {
        // Pre-reserve the tracking vector so push_back() can never throw
        // AFTER the inner backend has accepted a submit (a throw there would
        // leave the op untracked while the backend runs it — a lost-write to
        // the probe's accounting). The pipeline is bounded by its depth, which
        // is far below this fixed capacity.
        live_read_completions_.reserve(capacity);
    }

    Result<void> submit_read(sluice::async::ReadOp op,
                             sluice::async::Completion<std::size_t>& c) override {
        // 1. Ask the INNER backend FIRST: a submit that fails must never be
        //    counted as live (it never became outstanding in the backend).
        auto r = inner_->submit_read(op, c);
        if (!r.has_value()) return r;
        // 2. Only a successfully-submitted op enters the tracking set, and all
        //    accounting state changes happen under the same mutex. The vector
        //    capacity was pre-reserved in the constructor, so push_back cannot
        //    throw after the backend accepted the op.
        {
            std::lock_guard<std::mutex> lk(mtx_);
            live_read_completions_.push_back(&c);
            ++live_reads_;
            if (live_reads_ > peak_->load()) peak_->store(live_reads_);
        }
        return {};
    }
    Result<void> submit_write(sluice::async::WriteOp op,
                              sluice::async::Completion<std::size_t>& c) override {
        return inner_->submit_write(op, c);
    }
    Result<void> submit_sync_data(sluice::async::SyncDataOp op,
                                  sluice::async::Completion<void>& c) override {
        return inner_->submit_sync_data(op, c);
    }
    Result<void> submit_sync_all(sluice::async::SyncAllOp op,
                                 sluice::async::Completion<void>& c) override {
        return inner_->submit_sync_all(op, c);
    }
    std::size_t poll() override {
        std::size_t n = inner_->poll();
        reap_ready_reads();
        return n;
    }
    Result<std::size_t> wait_one() override {
        auto r = inner_->wait_one();
        reap_ready_reads();
        return r;
    }
    void cancel(sluice::async::Completion<std::size_t>& c) override {
        inner_->cancel(c);
    }
    void cancel(sluice::async::Completion<void>& c) override {
        inner_->cancel(c);
    }
    std::size_t outstanding() const noexcept override {
        return inner_->outstanding();
    }

    // Test-only accessors (the probe is test-local).
    std::size_t live_reads() {
        std::lock_guard<std::mutex> lk(mtx_);
        return live_reads_;
    }
    std::size_t tracked_count() {
        std::lock_guard<std::mutex> lk(mtx_);
        return live_read_completions_.size();
    }

private:
    static constexpr std::size_t kTrackedCapacity = 1024;

    // Decrement live_reads_ for any tracked read completion that is now ready.
    void reap_ready_reads() {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = live_read_completions_.begin();
             it != live_read_completions_.end();) {
            if ((*it)->ready()) {
                --live_reads_;
                it = live_read_completions_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::unique_ptr<AsyncBackend> inner_;
    std::shared_ptr<std::atomic<std::size_t>> peak_;
    std::mutex mtx_;
    std::size_t live_reads_ = 0;
    std::vector<sluice::async::Completion<std::size_t>*> live_read_completions_;
};

// Minimal inner backend for the probe accounting tests: accepts or rejects
// submit_read on demand and records the accepted Completions so the test can
// complete them directly. All other methods are never reached by the probe
// unit tests.
class ProbeStubBackend : public sluice::async::AsyncBackend {
public:
    bool fail_reads = false;
    int reads_submitted = 0;
    std::vector<sluice::async::Completion<std::size_t>*> completions;

    Result<void> submit_read(sluice::async::ReadOp op,
                             sluice::async::Completion<std::size_t>& c) override {
        if (fail_reads)
            return make_unexpected<void>(IoError{IoError::Code::backend_error});
        c.mark_outstanding();
        completions.push_back(&c);
        ++reads_submitted;
        return {};
    }
    Result<void> submit_write(sluice::async::WriteOp op,
                              sluice::async::Completion<std::size_t>& c) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_sync_data(sluice::async::SyncDataOp op,
                                  sluice::async::Completion<void>& c) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_sync_all(sluice::async::SyncAllOp op,
                                 sluice::async::Completion<void>& c) override {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return Result<std::size_t>{0}; }
    void cancel(sluice::async::Completion<std::size_t>&) override {}
    void cancel(sluice::async::Completion<void>&) override {}
    std::size_t outstanding() const noexcept override { return 0; }
};

}  // namespace

// ---------------------------------------------------------------------------
// Probe accounting (Section 9): a FAILED inner submit_read must not be counted
// as live/outstanding, must not touch the peak, and must not stay in the
// tracking set. A SUCCESSFUL submit is tracked under the mutex and reaped when
// the Completion becomes ready.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(probe_accounting_failed_submit_not_counted) {
    auto peak = std::make_shared<std::atomic<std::size_t>>(0);
    auto stub = std::make_unique<ProbeStubBackend>();
    stub->fail_reads = true;
    auto* stub_raw = stub.get();
    ReadConcurrencyProbe probe(std::move(stub), peak);

    std::byte buf[16]{};
    sluice::async::Completion<std::size_t> c;
    auto r = probe.submit_read(sluice::async::ReadOp{0, buf, 16, 0}, c);

    // The failure is propagated verbatim...
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
    // ...and NOTHING was counted: the op never became outstanding in the
    // inner backend, so the probe must not record it.
    SLUICE_CHECK(probe.live_reads() == 0);
    SLUICE_CHECK(probe.tracked_count() == 0);
    SLUICE_CHECK(peak->load() == 0);
    SLUICE_CHECK(c.idle());
    SLUICE_CHECK(stub_raw->reads_submitted == 0);
}

SLUICE_TEST_CASE(probe_accounting_success_tracked_and_reaped) {
    auto peak = std::make_shared<std::atomic<std::size_t>>(0);
    auto stub = std::make_unique<ProbeStubBackend>();
    auto* stub_raw = stub.get();
    ReadConcurrencyProbe probe(std::move(stub), peak);

    std::byte buf[16]{};
    sluice::async::Completion<std::size_t> c;
    auto r = probe.submit_read(sluice::async::ReadOp{0, buf, 16, 0}, c);

    // The successful submit IS counted (one live read), and the peak moves.
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(stub_raw->reads_submitted == 1);
    SLUICE_CHECK(probe.live_reads() == 1);
    SLUICE_CHECK(probe.tracked_count() == 1);
    SLUICE_CHECK(peak->load() == 1);

    // Complete the read directly (the stub never completes it itself); the
    // probe's poll() reaps now-ready tracked reads and decrements the count.
    c.complete_with(Result<std::size_t>{16});
    SLUICE_CHECK(c.ready());
    probe.poll();
    SLUICE_CHECK(probe.live_reads() == 0);
    SLUICE_CHECK(probe.tracked_count() == 0);
    // The peak stays at the observed maximum.
    SLUICE_CHECK(peak->load() == 1);
}

SLUICE_TEST_CASE(pipeline_integration_real_multi_outstanding_reads) {
    constexpr std::size_t B = 4096;
    constexpr std::size_t N = B * 16;  // large enough to fill a depth-4 window
    TempFile src, dst;
    seed_file(src.fd, N);

    auto peak = std::make_shared<std::atomic<std::size_t>>(0);
    auto inner = std::make_unique<sluice::async::ThreadPoolBackend>();
    std::unique_ptr<sluice::async::AsyncBackend> backend(
        new ReadConcurrencyProbe(std::move(inner), peak));

    auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/4, 1,
                                             SyncPolicy::none, std::move(backend));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value().bytes_copied == N);
    SLUICE_CHECK(files_equal(src.fd, dst.fd, N));
    // The real backend saw >= 2 reads outstanding simultaneously. (peak may not
    // reach the full depth on small/fast files, but must exceed the depth=1
    // baseline of 1 to prove read-ahead is real.)
    SLUICE_CHECK_MSG(peak->load() >= 2,
                     "ThreadPoolBackend never saw >= 2 concurrent reads; "
                     "Version B read-ahead is not producing real concurrency");
}

// ---------------------------------------------------------------------------
// depth=1 against the real backend: peak concurrent reads == 1 (Version A
// behavior preserved).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(pipeline_integration_depth1_single_read) {
    constexpr std::size_t B = 4096;
    constexpr std::size_t N = B * 8;
    TempFile src, dst;
    seed_file(src.fd, N);

    auto peak = std::make_shared<std::atomic<std::size_t>>(0);
    auto inner = std::make_unique<sluice::async::ThreadPoolBackend>();
    std::unique_ptr<sluice::async::AsyncBackend> backend(
        new ReadConcurrencyProbe(std::move(inner), peak));

    auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/1, 1,
                                             SyncPolicy::none, std::move(backend));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value().bytes_copied == N);
    SLUICE_CHECK(files_equal(src.fd, dst.fd, N));
    SLUICE_CHECK_MSG(peak->load() == 1,
                     "depth=1 must keep at most 1 read outstanding");
}

SLUICE_MAIN()
