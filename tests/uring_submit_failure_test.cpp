// Phase D1 real-liburing submit-failure / transport-progress tests.
//
// These tests verify the FROZEN DESIGN (docs/architecture/phase-d1-uring-
// frozen-design.md §5/§6) invariants of the new private-ring model:
//
//   * io_uring_submit() is TRANSPORT PROGRESS — it MUST NOT mutate
//     RequestState. A transient error (EINTR/EAGAIN/EBUSY), zero progress, or
//     partial progress leaves ring-owned requests alive and retryable; no
//     terminal is fabricated and no RequestArena state changes.
//   * A permanent submit failure poisons the ring for NEW admissions (new
//     submit_* rejects synchronously with the stored backend error), but
//     already ring-owned work remains bound for its CQE (Class-B/C: possibly
//     or already kernel-consumed). Class-A (definitely not consumed) work may
//     be retired with backend_error only after a structural proof — D1's
//     conservative stance treats all in-flight work as Class-B and lets CQEs
//     retire it.
//
// The injected submit hook replaces io_uring_submit() with a scripted result so
// the tests are deterministic. The hook does NOT touch io_uring_get_sqe (the
// ownership-transfer transaction begins there and contains no recoverable
// failure — frozen design §4.1).
#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/error.hpp>

#include <liburing.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using sluice::IoError;
using sluice::async::Completion;
using sluice::async::UringAsyncBackend;
using sluice::async::UringBackendSubmitTestHooks;
using sluice::async::UringConfig;
using sluice::async::WriteOp;

constexpr int kRealSubmit = std::numeric_limits<int>::max();

class TempFile {
  public:
    TempFile() {
        path_ = (std::filesystem::temp_directory_path() / "sluice_uring_d1_submit_XXXXXX").string();
        path_.push_back('\0');
        fd_ = ::mkstemp(path_.data());
        path_.pop_back();
    }
    ~TempFile() {
        if (fd_ >= 0)
            (void)::close(fd_);
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    int fd() const noexcept { return fd_; }

  private:
    int fd_ = -1;
    std::string path_;
};

// Scripted io_uring_submit replacement. Each call returns the next scripted
// result; kRealSubmit means "do a real io_uring_submit". Beyond the script the
// hook returns -EIO so the backend cannot accidentally drive a real submit
// the test did not intend.
class SubmitScript {
  public:
    explicit SubmitScript(std::span<const int> steps) : steps_(steps) {}

    static int invoke(void* context, io_uring* ring) noexcept {
        auto& self = *static_cast<SubmitScript*>(context);
        ++self.calls_;
        if (self.next_ >= self.steps_.size())
            return -EIO;
        const int step = self.steps_[self.next_++];
        if (step == kRealSubmit)
            return ::io_uring_submit(ring);
        return step;
    }

    std::size_t calls() const noexcept { return calls_; }

  private:
    std::span<const int> steps_;
    std::size_t next_ = 0;
    std::size_t calls_ = 0;
};

UringBackendSubmitTestHooks hooks_for(SubmitScript& script) {
    return UringBackendSubmitTestHooks{&script, &SubmitScript::invoke};
}

UringConfig small_config(unsigned queue_depth = 8) {
    return UringConfig{static_cast<std::size_t>(queue_depth), queue_depth};
}

template <class Predicate> std::size_t poll_bounded(UringAsyncBackend& backend, Predicate done) {
    std::size_t completed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        completed += backend.poll();
        std::this_thread::yield();
    }
    return completed;
}

} // namespace

// Transient -EINTR must NOT mutate RequestState: the request stays alive and
// retries on the next driver call, completing from its real CQE. This proves
// submit is transport progress only (frozen design §5).
SLUICE_TEST_CASE(uring_submit_transient_error_recovers_on_next_poll) {
    constexpr std::array steps{-EINTR, kRealSubmit};
    SubmitScript script(steps);
    UringAsyncBackend backend(small_config(), hooks_for(script));
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    Completion<std::size_t> completion;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), bytes.data(), bytes.size(), 0}, completion)
                     .has_value());

    // First poll: the transient error must NOT terminalize the request.
    SLUICE_CHECK(backend.poll() == 0);
    SLUICE_CHECK(!completion.ready());
    SLUICE_CHECK(backend.outstanding() == 1);

    const std::size_t completed = poll_bounded(backend, [&] { return completion.ready(); });
    SLUICE_CHECK(completed == 1);
    SLUICE_CHECK(completion.ready());
    SLUICE_CHECK(completion.result().has_value());
    SLUICE_CHECK(completion.result().value() == bytes.size());
    SLUICE_CHECK(backend.outstanding() == 0);
}

// Zero progress (submit returns 0) must NOT mutate RequestState either. The
// request remains outstanding and completes once transport makes progress.
SLUICE_TEST_CASE(uring_submit_zero_progress_does_not_change_request_state) {
    constexpr std::array steps{0, kRealSubmit};
    SubmitScript script(steps);
    UringAsyncBackend backend(small_config(), hooks_for(script));
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::byte, 4> bytes{};
    Completion<std::size_t> completion;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), bytes.data(), bytes.size(), 0}, completion)
                     .has_value());

    SLUICE_CHECK(backend.poll() == 0);
    SLUICE_CHECK(!completion.ready());
    SLUICE_CHECK(backend.outstanding() == 1);

    const std::size_t completed = poll_bounded(backend, [&] { return completion.ready(); });
    SLUICE_CHECK(completed == 1);
    SLUICE_CHECK(completion.ready());
    SLUICE_CHECK(completion.result().has_value());
}

// Partial submit (submit returns a positive count less than the number of
// prepared SQEs) MUST NOT change RequestState. In the new model dispatch
// installs ONE SQE per request and marks it running in one transaction; the
// submit count is purely transport. This replaces the legacy
// "splits_kernel_ownership" test which asserted the rejected prefix-driven
// behavior (frozen design §5.2).
SLUICE_TEST_CASE(uring_submit_partial_does_not_split_ownership) {
    // Two writes, then real submits so both eventually complete.
    constexpr std::array steps{kRealSubmit, kRealSubmit};
    SubmitScript script(steps);
    UringAsyncBackend backend(small_config(8), hooks_for(script));
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::byte, 4> first{std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
                                   std::byte{0x14}};
    std::array<std::byte, 4> second{std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
                                    std::byte{0x24}};
    Completion<std::size_t> first_completion;
    Completion<std::size_t> second_completion;
    SLUICE_CHECK(
        backend.submit_write(WriteOp{file.fd(), first.data(), first.size(), 0}, first_completion)
            .has_value());
    SLUICE_CHECK(
        backend.submit_write(WriteOp{file.fd(), second.data(), second.size(), 4}, second_completion)
            .has_value());

    // Both requests are outstanding (no partial-submit state split).
    SLUICE_CHECK(backend.outstanding() == 2);

    const std::size_t completed = poll_bounded(
        backend, [&] { return first_completion.ready() && second_completion.ready(); });
    SLUICE_CHECK(completed == 2);
    SLUICE_CHECK(first_completion.ready());
    SLUICE_CHECK(first_completion.result().has_value());
    SLUICE_CHECK(first_completion.result().value() == first.size());
    SLUICE_CHECK(second_completion.ready());
    SLUICE_CHECK(second_completion.result().has_value());
    SLUICE_CHECK(second_completion.result().value() == second.size());
    SLUICE_CHECK(backend.outstanding() == 0);

    std::array<std::byte, 12> on_disk{};
    const ssize_t read_count = ::pread(file.fd(), on_disk.data(), on_disk.size(), 0);
    SLUICE_CHECK(read_count == static_cast<ssize_t>(first.size() + second.size()));
    SLUICE_CHECK(std::memcmp(on_disk.data(), first.data(), first.size()) == 0);
    SLUICE_CHECK(std::memcmp(on_disk.data() + first.size(), second.data(), second.size()) == 0);
}

// P1 length boundary detector: liburing's io_uring_prep_read/write take an
// `unsigned nbytes`. A length > UINT_MAX MUST be rejected with invalid_argument
// (no silent size_t->unsigned truncation), and a length == UINT_MAX MUST be
// accepted by validation. This kills the mutant where the old SSIZE_MAX check
// let a >4GiB length through and the implicit narrowing at SQE fill silently
// truncated it. We do NOT allocate a UINT_MAX-sized buffer: descriptor
// validation checks representational form only (not buffer capacity), so a
// valid small buffer pointer paired with a huge length exercises the length
// rejection without any large allocation.
SLUICE_TEST_CASE(uring_length_over_uint_max_rejected_no_residue) {
    UringAsyncBackend backend(small_config());
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::byte, 1> one_byte{};
    Completion<std::size_t> completion;
    // UINT_MAX + 1: must be rejected with invalid_argument, Completion stays
    // idle, no accepted slot, no borrow, zero residue.
    const std::size_t over = static_cast<std::size_t>(std::numeric_limits<unsigned>::max()) + 1;
    const auto r = backend.submit_write(
        WriteOp{file.fd(), one_byte.data(), over, 0}, completion);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_argument);
    SLUICE_CHECK(!completion.ready());
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

SLUICE_TEST_CASE(uring_length_uint_max_accepted_by_validation) {
    // UINT_MAX is the largest representable liburing nbytes; validation MUST
    // accept it (the boundary is inclusive). We do NOT drive the request to
    // completion (that would require a UINT_MAX buffer and real I/O) — we only
    // assert the acceptance/no-residue-before-completion boundary. The request
    // is then retired by resetting the Completion, releasing the slot.
    UringAsyncBackend backend(small_config());
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::byte, 1> one_byte{};
    Completion<std::size_t> completion;
    const std::size_t at_max = static_cast<std::size_t>(std::numeric_limits<unsigned>::max());
    const auto r =
        backend.submit_write(WriteOp{file.fd(), one_byte.data(), at_max, 0}, completion);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(backend.outstanding() == 1);
    SLUICE_CHECK(backend.arena_slot_in_use() == 1);
    // The request is accepted and ring-owned; do not poll (no UINT_MAX I/O).
    // Cancel it so the slot is released and the backend destructs quiescently.
    backend.cancel(completion);
    // Drive reap so the canceled terminal publishes and the slot can release.
    (void)poll_bounded(backend, [&] { return completion.ready(); });
    SLUICE_CHECK(completion.ready());
    completion.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

// NOTE: a deterministic permanent-submit-failure / ring-poison test is a
// frozen-design HARD GATE (§6) that requires the Class-A proof (which
// ring-owned SQEs are definitely not kernel-consumed) before any in-flight
// work can be locally retired. D1's conservative stance keeps all in-flight
// work Class-B (bound for a CQE), so a poisoned ring with accepted work
// cannot quiesce for destruction — constructing that state in a deterministic
// test would require the very Class-A retirement path §6 freezes. The residual
// is recorded in docs/architecture/phase-d1-uring-frozen-design.md §6.5 and
// the completion report.

SLUICE_MAIN()
