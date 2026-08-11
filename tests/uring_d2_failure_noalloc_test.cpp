// Phase D2 — Uring failure-injection / accepted-terminal no-allocation evidence.
//
// Real mode exercises the authoritative production uring_backend.cpp with only
// read-only SLUICE_ASYNC_INTERNAL_TESTING observations. Stub mode proves build
// and API honesty only; the manifest requires evidence mode=real before this
// target can satisfy the Uring C2d record.
#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/error.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <unistd.h>
#endif

using namespace sluice::async;
using sluice::IoError;

#if defined(SLUICE_HAS_LIBURING)

namespace {

[[maybe_unused]] std::atomic<std::size_t> g_allocations{0};
[[maybe_unused]] std::atomic<bool> g_throw_all{false};

} // namespace

// TSan provides strong replacements for these operators. Keep all lifecycle,
// residue, and bounded-control checks active under TSan, but leave the allocator
// interposition proof to Debug/Release/ASan+UBSan.
#if !defined(__has_feature) || !__has_feature(thread_sanitizer)
constexpr bool kAllocProbeActive = true;

void* operator new(std::size_t n) {
    if (g_throw_all.load(std::memory_order_acquire))
        throw std::bad_alloc{};
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a) {
    if (g_throw_all.load(std::memory_order_acquire))
        throw std::bad_alloc{};
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    const std::size_t align = static_cast<std::size_t>(a);
    const std::size_t size = ((n + align - 1) / align) * align;
    if (void* p = std::aligned_alloc(align, size))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, std::size_t) noexcept { std::free(p); }
#else
constexpr bool kAllocProbeActive = false;
#endif

namespace {

constexpr int kRealSubmit = std::numeric_limits<int>::max();

class AllocationFailureWindow {
  public:
    AllocationFailureWindow() noexcept {
        if constexpr (kAllocProbeActive) {
            g_allocations.store(0, std::memory_order_relaxed);
            g_throw_all.store(true, std::memory_order_release);
        }
    }
    ~AllocationFailureWindow() {
        if constexpr (kAllocProbeActive)
            g_throw_all.store(false, std::memory_order_release);
    }
    AllocationFailureWindow(const AllocationFailureWindow&) = delete;
    AllocationFailureWindow& operator=(const AllocationFailureWindow&) = delete;
};

std::size_t measured_allocations() noexcept {
    if constexpr (kAllocProbeActive)
        return g_allocations.load(std::memory_order_relaxed);
    return 0;
}

class TempFile {
  public:
    TempFile() {
        char path[] = "/tmp/sluice_uring_d2_XXXXXX";
        fd_ = ::mkstemp(path);
        if (fd_ >= 0)
            (void)::unlink(path);
    }
    ~TempFile() {
        if (fd_ >= 0)
            (void)::close(fd_);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    int fd() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

  private:
    int fd_ = -1;
};

class PipePair {
  public:
    PipePair() { valid_ = ::pipe(fds_) == 0; }
    ~PipePair() {
        if (fds_[0] >= 0)
            (void)::close(fds_[0]);
        if (fds_[1] >= 0)
            (void)::close(fds_[1]);
    }
    PipePair(const PipePair&) = delete;
    PipePair& operator=(const PipePair&) = delete;
    bool valid() const noexcept { return valid_; }
    int read_fd() const noexcept { return fds_[0]; }
    int write_fd() const noexcept { return fds_[1]; }
    bool write_one(unsigned char value) const noexcept {
        return ::write(fds_[1], &value, 1) == 1;
    }

  private:
    int fds_[2]{-1, -1};
    bool valid_ = false;
};

class SubmitScript {
  public:
    template <std::size_t N>
    explicit SubmitScript(const std::array<int, N>& steps) noexcept
        : steps_(steps.data()), count_(N) {}

    static int invoke(void* context, io_uring* ring) noexcept {
        auto& self = *static_cast<SubmitScript*>(context);
        const std::size_t index = self.next_.fetch_add(1, std::memory_order_relaxed);
        if (index >= self.count_)
            return ::io_uring_submit(ring);
        const int step = self.steps_[index];
        if (step == kRealSubmit)
            return ::io_uring_submit(ring);
        return step;
    }

    static void before_poison_wait(void* context) noexcept {
        static_cast<SubmitScript*>(context)->poison_wait_entered_.store(
            true, std::memory_order_release);
    }

    std::size_t calls() const noexcept { return next_.load(std::memory_order_relaxed); }
    bool poison_wait_entered() const noexcept {
        return poison_wait_entered_.load(std::memory_order_acquire);
    }

  private:
    const int* steps_ = nullptr;
    std::size_t count_ = 0;
    std::atomic<std::size_t> next_{0};
    std::atomic<bool> poison_wait_entered_{false};
};

UringBackendSubmitTestHooks hooks_for(SubmitScript& script) noexcept {
    return UringBackendSubmitTestHooks{&script, &SubmitScript::invoke, nullptr};
}

UringBackendSubmitTestHooks hooks_for_poison_wait(SubmitScript& script) noexcept {
    return UringBackendSubmitTestHooks{&script, &SubmitScript::invoke, nullptr,
                                       &SubmitScript::before_poison_wait};
}

struct ProgressSnapshot {
    std::size_t slot_in_use = 0;
    std::size_t accepted = 0;
    std::size_t dispatch = 0;
    std::size_t router = 0;
    std::size_t ledger = 0;
    std::size_t sq_ready = 0;
    std::size_t controls = 0;

    friend bool operator==(const ProgressSnapshot&, const ProgressSnapshot&) = default;
};

ProgressSnapshot snapshot(const UringAsyncBackend& backend) noexcept {
    return ProgressSnapshot{backend.arena_slot_in_use(),
                            backend.arena_accepted_outstanding(),
                            backend.dispatch_size_for_test(),
                            backend.live_cookies_for_test(),
                            backend.transport_ledger_size_for_test(),
                            backend.sq_ready_for_test(),
                            backend.live_control_entries_for_test()};
}

template <class Predicate>
bool wait_until_ready(UringAsyncBackend& backend, Predicate done) {
    if (done())
        return true;
    const auto waited = backend.wait_one();
    return waited.has_value() && done();
}

bool backend_error_eio(const Completion<std::size_t>& c) noexcept {
    return c.ready() && !c.result().has_value() &&
           c.result().error().code == IoError::Code::backend_error &&
           c.result().error().os_errno == EIO;
}

bool backend_error_eio(const Completion<void>& c) noexcept {
    return c.ready() && !c.result().has_value() &&
           c.result().error().code == IoError::Code::backend_error &&
           c.result().error().os_errno == EIO;
}

} // namespace

#endif // SLUICE_HAS_LIBURING

SLUICE_TEST_CASE(uring_d2_evidence_mode) {
#if defined(SLUICE_HAS_LIBURING)
    std::fprintf(stdout,
                 "[evidence-meta] evidence=uring_c2d_failure_injection mode=real\n");
    UringAsyncBackend backend(UringConfig{1, 1});
    SLUICE_CHECK(backend.available());
#else
    std::fprintf(stdout,
                 "[evidence-meta] evidence=uring_c2d_failure_injection mode=stub\n");
    UringAsyncBackend backend;
    SLUICE_CHECK(!backend.available());
#endif
}

#if defined(SLUICE_HAS_LIBURING)

SLUICE_TEST_CASE(uring_d2_precommit_size_rejections_leave_zero_new_residue) {
    // Natural descriptor rejection after reserve.
    UringAsyncBackend invalid_backend(UringConfig{1, 1});
    SLUICE_CHECK(invalid_backend.available());
    Completion<std::size_t> invalid_completion;
    std::byte invalid_byte{};
    const ProgressSnapshot invalid_before = snapshot(invalid_backend);
    const auto invalid = invalid_backend.submit_read(
        ReadOp{-1, &invalid_byte, 1, 0}, invalid_completion);
    const ProgressSnapshot invalid_after = snapshot(invalid_backend);
    SLUICE_CHECK(!invalid.has_value());
    SLUICE_CHECK(invalid.error().code == IoError::Code::invalid_argument);
    SLUICE_CHECK(!invalid_completion.outstanding() && !invalid_completion.ready());
    SLUICE_CHECK(invalid_before == invalid_after);

    // Natural reserve/capacity rejection while one real request remains bound.
    PipePair capacity_pipe;
    SLUICE_CHECK(capacity_pipe.valid());
    UringAsyncBackend capacity_backend(UringConfig{1, 1});
    Completion<std::size_t> retained;
    Completion<std::size_t> rejected;
    std::byte retained_byte{};
    std::byte rejected_byte{};
    SLUICE_CHECK(capacity_backend
                     .submit_read(ReadOp{capacity_pipe.read_fd(), &retained_byte, 1, 0}, retained)
                     .has_value());
    const ProgressSnapshot capacity_before = snapshot(capacity_backend);
    const auto full = capacity_backend.submit_read(
        ReadOp{capacity_pipe.read_fd(), &rejected_byte, 1, 0}, rejected);
    const ProgressSnapshot capacity_after = snapshot(capacity_backend);
    const bool full_zero_residue = !full.has_value() &&
                                   full.error().code == IoError::Code::would_block &&
                                   !rejected.outstanding() && !rejected.ready() &&
                                   capacity_before == capacity_after;
    SLUICE_CHECK(capacity_pipe.write_one(0x31));
    const bool retained_ready =
        wait_until_ready(capacity_backend, [&] { return retained.ready(); });
    if (retained.ready())
        retained.reset();
    SLUICE_CHECK(full_zero_residue);
    SLUICE_CHECK(retained_ready);
    SLUICE_CHECK(capacity_backend.arena_slot_in_use() == 0);

    // Binding-CAS loss with spare capacity: the candidate slot rolls back and
    // the already-bound request plus every progress domain remain unchanged.
    PipePair binding_pipe;
    SLUICE_CHECK(binding_pipe.valid());
    UringAsyncBackend binding_backend(UringConfig{2, 2});
    Completion<std::size_t> bound;
    std::byte bound_byte{};
    std::byte second_byte{};
    SLUICE_CHECK(binding_backend
                     .submit_read(ReadOp{binding_pipe.read_fd(), &bound_byte, 1, 0}, bound)
                     .has_value());
    const ProgressSnapshot binding_before = snapshot(binding_backend);
    const auto cas_loss = binding_backend.submit_read(
        ReadOp{binding_pipe.read_fd(), &second_byte, 1, 0}, bound);
    const ProgressSnapshot binding_after = snapshot(binding_backend);
    const bool cas_zero_residue = !cas_loss.has_value() &&
                                  cas_loss.error().code == IoError::Code::invalid_state &&
                                  bound.outstanding() && binding_before == binding_after;
    SLUICE_CHECK(binding_pipe.write_one(0x32));
    const bool bound_ready = wait_until_ready(binding_backend, [&] { return bound.ready(); });
    if (bound.ready())
        bound.reset();
    SLUICE_CHECK(cas_zero_residue);
    SLUICE_CHECK(bound_ready);
    SLUICE_CHECK(binding_backend.arena_slot_in_use() == 0);
}

SLUICE_TEST_CASE(uring_d2_precommit_void_rejections_leave_zero_new_residue) {
    UringAsyncBackend invalid_backend(UringConfig{1, 1});
    Completion<void> invalid_completion;
    const ProgressSnapshot invalid_before = snapshot(invalid_backend);
    const auto invalid = invalid_backend.submit_sync_data(SyncDataOp{-1}, invalid_completion);
    const ProgressSnapshot invalid_after = snapshot(invalid_backend);
    SLUICE_CHECK(!invalid.has_value());
    SLUICE_CHECK(invalid.error().code == IoError::Code::invalid_argument);
    SLUICE_CHECK(!invalid_completion.outstanding() && !invalid_completion.ready());
    SLUICE_CHECK(invalid_before == invalid_after);

    TempFile file;
    SLUICE_CHECK(file.valid());
    UringAsyncBackend binding_backend(UringConfig{2, 2});
    Completion<void> bound;
    SLUICE_CHECK(binding_backend.submit_sync_data(SyncDataOp{file.fd()}, bound).has_value());
    const ProgressSnapshot binding_before = snapshot(binding_backend);
    const auto cas_loss = binding_backend.submit_sync_all(SyncAllOp{file.fd()}, bound);
    const ProgressSnapshot binding_after = snapshot(binding_backend);
    const bool cas_zero_residue = !cas_loss.has_value() &&
                                  cas_loss.error().code == IoError::Code::invalid_state &&
                                  bound.outstanding() && binding_before == binding_after;
    const bool ready = wait_until_ready(binding_backend, [&] { return bound.ready(); });
    if (bound.ready())
        bound.reset();
    SLUICE_CHECK(cas_zero_residue);
    SLUICE_CHECK(ready);
    SLUICE_CHECK(binding_backend.arena_slot_in_use() == 0);
}

SLUICE_TEST_CASE(uring_d2_ordinary_size_path_is_allocation_free) {
    UringAsyncBackend backend(UringConfig{2, 2});
    TempFile file;
    std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    Completion<std::size_t> completion;
    SLUICE_CHECK(backend.available() && file.valid());

    bool submit_ok = false;
    bool ready = false;
    bool value_ok = false;
    {
        AllocationFailureWindow allocation_failure;
        submit_ok = backend
                        .submit_write(WriteOp{file.fd(), bytes.data(), bytes.size(), 0}, completion)
                        .has_value();
        ready = wait_until_ready(backend, [&] { return completion.ready(); });
        value_ok = ready && completion.result().has_value() &&
                   completion.result().value() == bytes.size();
        if (ready)
            completion.reset();
    }
    const std::size_t allocations = measured_allocations();
    SLUICE_CHECK(submit_ok && ready && value_ok);
    SLUICE_CHECK(allocations == 0);
    SLUICE_CHECK(backend.outstanding() == 0 && backend.arena_slot_in_use() == 0);
}

SLUICE_TEST_CASE(uring_d2_ordinary_void_path_is_allocation_free) {
    UringAsyncBackend backend(UringConfig{1, 1});
    TempFile file;
    Completion<void> completion;
    SLUICE_CHECK(backend.available() && file.valid());

    bool submit_ok = false;
    bool ready = false;
    bool value_ok = false;
    {
        AllocationFailureWindow allocation_failure;
        submit_ok = backend.submit_sync_data(SyncDataOp{file.fd()}, completion).has_value();
        ready = wait_until_ready(backend, [&] { return completion.ready(); });
        value_ok = ready && completion.result().has_value();
        if (ready)
            completion.reset();
    }
    const std::size_t allocations = measured_allocations();
    SLUICE_CHECK(submit_ok && ready && value_ok);
    SLUICE_CHECK(allocations == 0);
    SLUICE_CHECK(backend.outstanding() == 0 && backend.arena_slot_in_use() == 0);
}

SLUICE_TEST_CASE(uring_d2_permanent_recovery_size_and_void_are_allocation_free) {
    constexpr std::array steps{-EIO};
    SubmitScript script(steps);
    UringAsyncBackend backend(UringConfig{2, 2}, hooks_for(script));
    TempFile file;
    std::array<std::byte, 1> byte{std::byte{0x41}};
    Completion<std::size_t> size_completion;
    Completion<void> void_completion;
    SLUICE_CHECK(backend.available() && file.valid());

    bool size_submit = false;
    bool void_submit = false;
    bool ready = false;
    bool size_error = false;
    bool void_error = false;
    {
        AllocationFailureWindow allocation_failure;
        size_submit = backend
                          .submit_write(WriteOp{file.fd(), byte.data(), byte.size(), 0},
                                        size_completion)
                          .has_value();
        void_submit = backend.submit_sync_data(SyncDataOp{file.fd()}, void_completion).has_value();
        ready = wait_until_ready(
            backend, [&] { return size_completion.ready() && void_completion.ready(); });
        size_error = backend_error_eio(size_completion);
        void_error = backend_error_eio(void_completion);
        if (size_completion.ready())
            size_completion.reset();
        if (void_completion.ready())
            void_completion.reset();
    }
    const std::size_t allocations = measured_allocations();
    SLUICE_CHECK(size_submit && void_submit && ready && size_error && void_error);
    SLUICE_CHECK(allocations == 0);
    SLUICE_CHECK(script.calls() == 1);
    SLUICE_CHECK(backend.outstanding() == 0 && backend.arena_slot_in_use() == 0);
}

SLUICE_TEST_CASE(uring_d2_poison_rejects_after_capacity_is_recycled) {
    constexpr std::array steps{-EIO};
    SubmitScript script(steps);
    UringAsyncBackend backend(UringConfig{1, 1}, hooks_for(script));
    TempFile file;
    std::array<std::byte, 1> byte{std::byte{0x49}};
    Completion<std::size_t> recovered;
    Completion<std::size_t> rejected;
    SLUICE_CHECK(backend.available() && file.valid());
    SLUICE_CHECK(
        backend.submit_write(WriteOp{file.fd(), byte.data(), 1, 0}, recovered).has_value());
    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(backend_error_eio(recovered));
    recovered.reset();
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);

    const ProgressSnapshot before = snapshot(backend);
    const auto result =
        backend.submit_write(WriteOp{file.fd(), byte.data(), 1, 1}, rejected);
    const ProgressSnapshot after = snapshot(backend);
    SLUICE_CHECK(!result.has_value());
    SLUICE_CHECK(result.error().code == IoError::Code::backend_error);
    SLUICE_CHECK(result.error().os_errno == EIO);
    SLUICE_CHECK(!rejected.outstanding() && !rejected.ready());
    SLUICE_CHECK(before == after);
}

SLUICE_TEST_CASE(uring_d2_pending_cancel_and_class_a_recovery_have_one_winner_each) {
    constexpr std::array steps{0, -EIO};
    SubmitScript script(steps);
    UringAsyncBackend backend(UringConfig{2, 1}, hooks_for(script));
    TempFile file;
    std::array<std::byte, 1> older_byte{std::byte{0x51}};
    std::array<std::byte, 1> local_byte{std::byte{0x52}};
    Completion<std::size_t> older;
    Completion<std::size_t> local;
    SLUICE_CHECK(backend.available() && file.valid());

    SLUICE_CHECK(
        backend.submit_write(WriteOp{file.fd(), older_byte.data(), 1, 0}, older).has_value());
    SLUICE_CHECK(
        backend.submit_write(WriteOp{file.fd(), local_byte.data(), 1, 1}, local).has_value());
    SLUICE_CHECK(backend.dispatch_size_for_test() == 1);
    SLUICE_CHECK(backend.live_cookies_for_test() == 1);

    bool both_ready = false;
    bool older_error = false;
    bool local_canceled = false;
    std::size_t second_reap = 0;
    {
        AllocationFailureWindow allocation_failure;
        backend.cancel(local);
        (void)backend.poll();
        both_ready = older.ready() && local.ready();
        older_error = backend_error_eio(older);
        local_canceled = local.ready() && !local.result().has_value() &&
                         local.result().error().code == IoError::Code::canceled;
        second_reap = backend.poll();
        if (older.ready())
            older.reset();
        if (local.ready())
            local.reset();
    }
    const std::size_t allocations = measured_allocations();
    SLUICE_CHECK(both_ready && older_error && local_canceled);
    SLUICE_CHECK(second_reap == 0);
    SLUICE_CHECK(allocations == 0);
    SLUICE_CHECK(backend.outstanding() == 0 && backend.arena_slot_in_use() == 0);
}

SLUICE_TEST_CASE(uring_d2_poison_wait_never_submits_quarantined_write) {
    constexpr std::array steps{kRealSubmit, -EIO};
    SubmitScript script(steps);
    UringAsyncBackend backend(UringConfig{2, 2}, hooks_for_poison_wait(script));
    PipePair pipe;
    TempFile file;
    std::byte read_byte{};
    std::array<std::byte, 1> write_byte{std::byte{0x64}};
    Completion<std::size_t> old_class_c;
    Completion<std::size_t> quarantined;
    SLUICE_CHECK(backend.available() && pipe.valid() && file.valid());

    SLUICE_CHECK(
        backend.submit_read(ReadOp{pipe.read_fd(), &read_byte, 1, 0}, old_class_c).has_value());
    SLUICE_CHECK(backend.poll() == 0); // old read is real submitted Class C
    SLUICE_CHECK(backend
                     .submit_write(WriteOp{file.fd(), write_byte.data(), 1, 0}, quarantined)
                     .has_value());
    SLUICE_CHECK(backend.poll() == 1); // write is proven Class A and retired
    SLUICE_CHECK(backend_error_eio(quarantined));

    // Transport-state proof: the scripted -EIO never entered io_uring_submit
    // (SubmitScript returns the injected step verbatim; only kRealSubmit calls
    // liburing), so the quarantined Class-A write SQE is still staged in the
    // application-side SQ. A correct to_submit=0 wait must not consume or flush
    // it; an M8 io_uring_submit() mutant flushes it and sq_ready drops. This is
    // the deterministic invariant the M8 mutant is killed on — independent of
    // when the kernel would settle a flushed write.
    const std::size_t sq_before = backend.sq_ready_for_test();
    SLUICE_CHECK(sq_before == 1);

    bool pipe_written = false;
    std::thread writer([&] {
        while (!script.poison_wait_entered())
            std::this_thread::yield();
        pipe_written = pipe.write_one(0x65);
    });
    const auto waited = backend.wait_one();
    writer.join();
    SLUICE_CHECK(pipe_written);
    SLUICE_CHECK(waited.has_value() && waited.value() == 1);
    SLUICE_CHECK(old_class_c.ready() && old_class_c.result().has_value());

    const std::size_t sq_after = backend.sq_ready_for_test();
    SLUICE_CHECK(sq_after == sq_before);

    // Auxiliary detector only: a forbidden submitting wait may return on the
    // older pipe CQE before the quarantined write's side effect is observable,
    // so the disk read alone is NOT deterministic evidence (the sq_ready
    // invariant above is). Drain one non-submitting poisoned poll so any
    // already-submitted mutant SQE/CQE has a chance to settle before the read.
    (void)backend.poll();
    std::byte observed{};
    const ssize_t disk_bytes = ::pread(file.fd(), &observed, 1, 0);

    old_class_c.reset();
    quarantined.reset();
    SLUICE_CHECK(disk_bytes == 0);
    SLUICE_CHECK(backend.outstanding() == 0 && backend.arena_slot_in_use() == 0);
}

SLUICE_TEST_CASE(uring_d2_repeated_cancel_control_is_bounded_and_allocation_free) {
    constexpr std::array steps{kRealSubmit, -EINTR, -EAGAIN, -EBUSY, -EIO};
    SubmitScript script(steps);
    UringAsyncBackend backend(UringConfig{1, 1}, hooks_for(script));
    PipePair pipe;
    std::byte read_byte{};
    Completion<std::size_t> completion;
    SLUICE_CHECK(backend.available() && pipe.valid());
    SLUICE_CHECK(
        backend.submit_read(ReadOp{pipe.read_fd(), &read_byte, 1, 0}, completion).has_value());
    SLUICE_CHECK(backend.poll() == 0); // original operation is real Class C and blocked
    SLUICE_CHECK(script.calls() == 1);

    std::size_t max_controls = 0;
    std::size_t max_ledger = 0;
    std::size_t max_sq_ready = 0;
    bool original_retained_after_poison = false;
    bool ready = false;
    bool result_ok = false;
    bool pipe_written = false;
    {
        AllocationFailureWindow allocation_failure;
        for (int round = 0; round < 4; ++round) {
            backend.cancel(completion);
            backend.cancel(completion);
            backend.cancel(completion);
            max_controls = std::max(max_controls, backend.live_control_entries_for_test());
            max_ledger = std::max(max_ledger, backend.transport_ledger_size_for_test());
            max_sq_ready = std::max(max_sq_ready, backend.sq_ready_for_test());
            (void)backend.poll();
        }
        original_retained_after_poison = completion.outstanding() && !completion.ready() &&
                                         backend.live_cookies_for_test() == 1 &&
                                         backend.live_control_entries_for_test() == 0;
        pipe_written = pipe.write_one(0x73);
        ready = wait_until_ready(backend, [&] { return completion.ready(); });
        result_ok = ready && completion.result().has_value() &&
                    completion.result().value() == 1 && read_byte == std::byte{0x73};
        if (ready)
            completion.reset();
    }
    const std::size_t allocations = measured_allocations();
    SLUICE_CHECK(max_controls == 1);
    SLUICE_CHECK(max_ledger == 1);
    SLUICE_CHECK(max_sq_ready == 1);
    SLUICE_CHECK(original_retained_after_poison && pipe_written && ready && result_ok);
    SLUICE_CHECK(script.calls() == 5);
    SLUICE_CHECK(allocations == 0);
    SLUICE_CHECK(backend.outstanding() == 0 && backend.arena_slot_in_use() == 0);
}

#else // !SLUICE_HAS_LIBURING — stub mode: build/API honesty only.

// round-4 (P1-2): the SAME pinned semantic case names register as empty
// build/API-only bodies so the manifest's exact case-set holds in EVERY mode
// (G2, the C2b/C2c/C2e pattern). Before this repair the stub build ran only
// the evidence-mode case, so a stub run was INCOMPLETE for the WRONG reason
// (case-set mismatch) instead of "mode=stub not allowed by required_modes" —
// and the aggregate could not distinguish a clean stub from a malformed one.
SLUICE_TEST_CASE(uring_d2_precommit_size_rejections_leave_zero_new_residue) {}
SLUICE_TEST_CASE(uring_d2_precommit_void_rejections_leave_zero_new_residue) {}
SLUICE_TEST_CASE(uring_d2_ordinary_size_path_is_allocation_free) {}
SLUICE_TEST_CASE(uring_d2_ordinary_void_path_is_allocation_free) {}
SLUICE_TEST_CASE(uring_d2_permanent_recovery_size_and_void_are_allocation_free) {}
SLUICE_TEST_CASE(uring_d2_poison_rejects_after_capacity_is_recycled) {}
SLUICE_TEST_CASE(uring_d2_pending_cancel_and_class_a_recovery_have_one_winner_each) {}
SLUICE_TEST_CASE(uring_d2_poison_wait_never_submits_quarantined_write) {}
SLUICE_TEST_CASE(uring_d2_repeated_cancel_control_is_bounded_and_allocation_free) {}

#endif // SLUICE_HAS_LIBURING

SLUICE_MAIN()
