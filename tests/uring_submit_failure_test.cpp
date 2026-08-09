// Phase D1 real-liburing submit-failure / transport-progress tests.
//
// These tests verify the FROZEN DESIGN (docs/architecture/phase-d1-uring-
// frozen-design.md §5/§6) invariants of the new private-ring model:
//
//   * io_uring_submit() is TRANSPORT PROGRESS — it MUST NOT mutate
//     RequestState. A transient error (EINTR/EAGAIN/EBUSY), zero progress, or
//     partial progress leaves ring-owned requests alive and retryable; no
//     terminal is fabricated and no RequestArena state changes.
//   * A permanent negative submit result poisons NEW admission. The bounded
//     physical ledger identifies the proven-zero-consumption Class-A suffix;
//     the recovery controller retires it and the still-local FIFO with
//     backend_error. Entries from an earlier positive submit remain bound for
//     their CQEs and poison progress uses to_submit=0 only.
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
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kAddressSpaceProbeActive = false;
#else
constexpr bool kAddressSpaceProbeActive = true;
#endif
#else
constexpr bool kAddressSpaceProbeActive = true;
#endif

namespace {

using sluice::IoError;
using sluice::async::Completion;
using sluice::async::ReadOp;
using sluice::async::UringAsyncBackend;
using sluice::async::UringBackendSubmitTestHooks;
using sluice::async::UringConfig;
using sluice::async::WriteOp;

constexpr int kRealSubmit = std::numeric_limits<int>::max();
// Sentinel: perform a REAL io_uring_submit() (so the kernel actually receives
// the SQEs and will produce real CQEs), but LIE to the backend and report 1
// regardless of the true submitted count. This is the scripted-partial-return
// lifecycle detector (reviewer §8.4): it proves the backend does NOT mutate
// RequestState based on the reported submit count (no accepted-prefix lifecycle
// authority) and that all original CQEs still retire normally. It is NOT a
// deterministic kernel partial submit — the kernel may have consumed all N.
constexpr int kRealSubmitReportOne = std::numeric_limits<int>::max() - 1;

class TempFile {
  public:
    TempFile() {
        path_ = (std::filesystem::temp_directory_path() / "sluice_uring_d1_submit_XXXXXX").string();
        path_.push_back('\0');
        fd_ = ::mkstemp(path_.data());
        path_.pop_back();
        SLUICE_CHECK(fd_ >= 0);
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
        if (step == kRealSubmitReportOne) {
            (void)::io_uring_submit(ring); // kernel really receives the SQEs
            return 1;                      // lie to the backend: report a partial
        }
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

class TransientWaitScript {
  public:
    static int submit(void*, io_uring*) noexcept {
        // Keep the prepared file-read SQE application-side so wait_one's
        // initial non-blocking pass cannot reap it before the scripted wait.
        return 0;
    }

    static int invoke(void* context, io_uring* ring, unsigned wait_nr) noexcept {
        auto& self = *static_cast<TransientWaitScript*>(context);
        ++self.calls_;
        if (self.calls_ == 1)
            return -EINTR;
        if (self.calls_ == 2)
            return -EAGAIN;
        if (self.calls_ == 3)
            return -EBUSY;
        if (self.calls_ == 4)
            return 0; // empty wake: no user CQE while one request is accepted
        // The fifth call performs the real flush+wait, so the file-read CQE
        // is guaranteed to exist only after every transient outcome was seen.
        return ::io_uring_submit_and_wait(ring, wait_nr);
    }

    std::size_t calls() const noexcept { return calls_; }

  private:
    std::size_t calls_ = 0;
};

UringBackendSubmitTestHooks wait_hooks_for(TransientWaitScript& script) {
    return UringBackendSubmitTestHooks{&script, &TransientWaitScript::submit,
                                       &TransientWaitScript::invoke};
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

SLUICE_TEST_CASE(uring_config_rejects_unrepresentable_capacity_before_allocation) {
    bool zero_rejected = false;
    try {
        UringAsyncBackend backend(UringConfig{0, 1});
    } catch (const std::invalid_argument&) {
        zero_rejected = true;
    }
    SLUICE_CHECK(zero_rejected);

    bool zero_queue_depth_rejected = false;
    try {
        UringAsyncBackend backend(UringConfig{1, 0});
    } catch (const std::invalid_argument&) {
        zero_queue_depth_rejected = true;
    }
    SLUICE_CHECK(zero_queue_depth_rejected);

    if constexpr (std::numeric_limits<std::size_t>::max() >
                  static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        if (!kAddressSpaceProbeActive)
            return;

        // Bound the child address space so the pre-fix constructor deterministically
        // reports allocation failure instead of reserving/touching an enormous
        // RequestArena. The fixed constructor rejects before that allocation.
        const pid_t pid = ::fork();
        SLUICE_CHECK(pid >= 0);
        if (pid == 0) {
            constexpr rlim_t limit = 256u * 1024u * 1024u;
            const rlimit address_space{limit, limit};
            if (::setrlimit(RLIMIT_AS, &address_space) != 0)
                std::_Exit(90);
            try {
                const std::size_t oversized =
                    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
                UringAsyncBackend backend(UringConfig{oversized, 1});
            } catch (const std::invalid_argument&) {
                std::_Exit(0);
            } catch (...) {
                std::_Exit(91);
            }
            std::_Exit(92);
        }

        int status = 0;
        SLUICE_CHECK(::waitpid(pid, &status, 0) == pid);
        SLUICE_CHECK(WIFEXITED(status));
        SLUICE_CHECK(WEXITSTATUS(status) == 0);
    }
}

SLUICE_TEST_CASE(uring_enqueue_sq_pressure_dispatches_fifo_front) {
    constexpr std::array steps{0, kRealSubmit, kRealSubmit, kRealSubmit, kRealSubmit};
    SubmitScript script(steps);
    UringAsyncBackend backend(UringConfig{4, 2}, hooks_for(script));
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::array<std::byte, 4>, 4> bytes{};
    std::array<Completion<std::size_t>, 4> completions;
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[i].fill(static_cast<std::byte>(i + 1));
        SLUICE_CHECK(backend
                         .submit_write(WriteOp{file.fd(), bytes[i].data(), bytes[i].size(), i * 4},
                                       completions[i])
                         .has_value());
    }

    // The third request first met a full SQ and stayed at the FIFO front. The
    // fourth enqueue's scripted real flush creates room. A correct fast path
    // dispatches request 3 before (or together with) request 4; the buggy
    // tail-specific call leaves request 3 enqueued despite available SQ room.
    const auto older_front_cookie = backend.live_cookie_for_offset_for_test(8);
    const auto newer_tail_cookie = backend.live_cookie_for_offset_for_test(12);
    const bool fifo_dispatch_order = older_front_cookie.has_value() &&
                                     newer_tail_cookie.has_value() &&
                                     *older_front_cookie < *newer_tail_cookie;

    (void)poll_bounded(backend, [&] {
        for (const auto& completion : completions) {
            if (!completion.ready())
                return false;
        }
        return true;
    });
    for (auto& completion : completions) {
        SLUICE_CHECK(completion.ready());
        completion.reset();
    }
    // Cookies are allocated monotonically at the ring-ownership transfer.
    // Merely observing both requests as ring-owned would let a tail-then-front
    // implementation pass; their cookie order proves the older front crossed
    // the ownership boundary first.
    SLUICE_CHECK(fifo_dispatch_order);
}

SLUICE_TEST_CASE(uring_wait_transients_never_return_false_drained_boundary) {
    TransientWaitScript script;
    UringAsyncBackend backend(small_config(), wait_hooks_for(script));
    if (!backend.available())
        return;

    TempFile file;
    const unsigned char seed = 0x5a;
    SLUICE_CHECK(::pwrite(file.fd(), &seed, 1, 0) == 1);
    std::byte byte{};
    Completion<std::size_t> completion;
    SLUICE_CHECK(backend.submit_read(ReadOp{file.fd(), &byte, 1, 0}, completion).has_value());

    const auto waited = backend.wait_one();
    const bool waited_for_completion = waited.has_value() && waited.value() > 0;
    const bool false_drained_boundary =
        waited.has_value() && waited.value() == 0 && backend.outstanding() == 1;

    // Keep the RED run quiescent: old code returns on one of the scripted
    // transients, so drive the remaining EAGAIN/EBUSY + empty-wake + real-wait
    // steps before asserting the bug.
    if (!completion.ready()) {
        while (!completion.ready() && script.calls() < 5)
            (void)backend.wait_one();
        (void)poll_bounded(backend, [&] { return completion.ready(); });
    }
    SLUICE_CHECK(completion.ready());
    SLUICE_CHECK(byte == std::byte{0x5a});
    completion.reset();

    SLUICE_CHECK(waited_for_completion);
    SLUICE_CHECK(!false_drained_boundary);
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(script.calls() == 5);
}

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

// Scripted-partial-return lifecycle detector (reviewer §8.4). The hook
// performs a REAL io_uring_submit() (kernel receives the SQEs) but LIES to the
// backend, reporting 1 on every call regardless of how many SQEs were actually
// submitted. This proves NO RequestState transition depends on the reported
// submit count — there is no accepted-prefix lifecycle authority smuggled back
// in — and that all original CQEs still retire normally. This is a lifecycle
// mutation detector, NOT a deterministic kernel partial submit (the kernel may
// have consumed all N SQEs); the assertion is about backend behavior on the
// reported value.
SLUICE_TEST_CASE(uring_scripted_partial_return_does_not_mutate_request_state) {
    // capacity/depth 8 so both writes can be ring-owned concurrently. The hook
    // reports 1 on each submit call.
    constexpr std::array steps{kRealSubmitReportOne, kRealSubmitReportOne,
                               kRealSubmitReportOne, kRealSubmitReportOne};
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

    // Both accepted and outstanding — the reported partial count did not
    // terminalize or drop either request.
    SLUICE_CHECK(backend.outstanding() == 2);

    // Both complete from their real CQEs despite the backend being told each
    // submit only progressed 1.
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

    // The hook deliberately violates the real syscall contract by reporting
    // only one consumed SQE even though its real submit may consume both. If
    // both CQEs arrive in the first reap, the lifecycle detector is already
    // satisfied but the synthetic transport ledger still has one reported
    // suffix. Drive exactly one more scripted prefix report so teardown
    // evidence matches the lie deterministically, independent of CQE timing.
    (void)backend.poll();
    SLUICE_CHECK(script.calls() == 2);
    first_completion.reset();
    second_completion.reset();
}

// P1 length boundary detectors: liburing's io_uring_prep_read/write take an
// `unsigned nbytes`. A length > UINT_MAX MUST be rejected with invalid_argument
// (no silent size_t->unsigned truncation), and a length == UINT_MAX MUST be
// accepted by validation. This kills the mutant where the old SSIZE_MAX check
// let a >4GiB length through and the implicit narrowing at SQE fill silently
// truncated it.
//
// The >UINT_MAX reject detector goes through the full admission path (submit_*),
// proving invalid_argument leaves Completion idle with no accepted slot, no
// borrow, and zero residue. The ==UINT_MAX accept detector uses a VALIDATION-
// ONLY seam: it exercises the exact production descriptor-validation logic
// without reserve/prepare/commit/enqueue/get_sqe/kernel touch. The previous
// form accepted a UINT_MAX write (ring-owned) then best-effort-canceled it; that
// was unsafe because submit_write immediately makes the operation ring-owned and
// running cancellation is intent-only, so the poll could submit the huge
// original operation before cancellation took effect. The validation-only seam
// removes that hazard entirely.
SLUICE_TEST_CASE(uring_length_over_uint_max_rejected_no_residue) {
    // 32-bit-safe: only run the >UINT_MAX reject detector on targets where
    // size_t can actually express UINT_MAX + 1. On a 32-bit size_t target the
    // value wraps back to 0 and there is no such input value to reject; the
    // representational boundary is vacuous there and we skip rather than assert
    // on a wrapped operand.
    if constexpr (std::numeric_limits<std::size_t>::max() >
                  static_cast<std::size_t>(std::numeric_limits<unsigned>::max())) {
        UringAsyncBackend backend(small_config());
        if (!backend.available())
            return;

        TempFile file;
        std::array<std::byte, 1> one_byte{};
        Completion<std::size_t> completion;
        // UINT_MAX + 1: must be rejected with invalid_argument, Completion stays
        // idle, no accepted slot, no borrow, zero residue.
        const std::size_t over =
            static_cast<std::size_t>(std::numeric_limits<unsigned>::max()) + std::size_t{1};
        const auto r = backend.submit_write(
            WriteOp{file.fd(), one_byte.data(), over, 0}, completion);
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_argument);
        SLUICE_CHECK(!completion.ready());
        SLUICE_CHECK(backend.outstanding() == 0);
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    }
}

SLUICE_TEST_CASE(uring_length_uint_max_accepted_by_validation_only) {
    // UINT_MAX is the largest representable liburing nbytes; validation MUST
    // accept it (the boundary is inclusive). The validation-only seam runs the
    // EXACT production descriptor-validation logic with no ring, no reserve,
    // no kernel touch — so a huge length is never made ring-owned and never
    // needs cancellation. Runs on every target (no 32-bit wrap concern: UINT_MAX
    // is representable in size_t on both 32-bit and 64-bit).
    TempFile file;
    std::array<std::byte, 1> one_byte{};
    const std::size_t at_max = static_cast<std::size_t>(std::numeric_limits<unsigned>::max());
    SLUICE_CHECK(UringAsyncBackend::validate_write_for_test(
                     WriteOp{file.fd(), one_byte.data(), at_max, 0})
                     .has_value());
    // And the reject boundary holds through the same seam on 64-bit targets.
    if constexpr (std::numeric_limits<std::size_t>::max() >
                  static_cast<std::size_t>(std::numeric_limits<unsigned>::max())) {
        const std::size_t over = at_max + std::size_t{1};
        const auto r = UringAsyncBackend::validate_write_for_test(
            WriteOp{file.fd(), one_byte.data(), over, 0});
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_argument);
    }
}

// P0-B stale-cookie detector (frozen design §7.1/§7.2). This is the central
// ABA proof. With request_capacity == 1, the CqeRouter has exactly ONE array
// slot, so a second operation necessarily REUSES that array slot. Under the
// old (pre-fix) encoding (user_data == router_slot+1), a stale CQE for op A
// would resolve through the recycled slot to op B's NEW SlotHandle and
// terminalize B with A's result. Under the cookie-keyed router (user_data ==
// never-reused op_cookie), op A's stale cookie matches NO live entry and is
// dropped; op B is unaffected and completes normally from its own CQE.
//
// Sequence:
//   A = write, cookie = cA (predicted via peek_next_cookie_for_test)
//   drive A to completion + reset (frees arena slot AND router array slot)
//   B = write, cookie = cB (predicted; reuses the same router ARRAY slot)
//   inject stale cookie cA  -> MUST be dropped (no live entry matches)
//     assert: B not ready, B's slot not terminalized, outstanding unchanged
//   then poll B to completion from its real CQE
//
// This detector FAILS against the old router_slot+1 encoding (B would be
// terminalized by the injected stale CQE).
SLUICE_TEST_CASE(uring_stale_cqe_cookie_dropped_not_misdelivered) {
    // capacity == depth == 1 forces router array slot 0 reuse for B.
    UringAsyncBackend backend(small_config(1));
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::byte, 4> a_bytes{std::byte{0xA1}, std::byte{0xA2}, std::byte{0xA3},
                                     std::byte{0xA4}};

    // Predict A's cookie: it is the value of next_cookie_ just before A is
    // dispatched (allocate_cookie_ returns next_cookie_ then increments).
    const std::uint64_t cookie_a = backend.peek_next_cookie_for_test();
    Completion<std::size_t> a_completion;
    SLUICE_CHECK(
        backend.submit_write(WriteOp{file.fd(), a_bytes.data(), a_bytes.size(), 0}, a_completion)
            .has_value());
    // Drive A fully to completion so its router array slot is retired and the
    // arena slot is released (allowing B to be accepted).
    SLUICE_CHECK(poll_bounded(backend, [&] { return a_completion.ready(); }) == 1);
    SLUICE_CHECK(a_completion.ready());
    SLUICE_CHECK(a_completion.result().has_value());
    SLUICE_CHECK(a_completion.result().value() == a_bytes.size());
    a_completion.reset(); // release arena slot; router array slot already freed at CQE

    // B reuses the same router ARRAY slot but gets a distinct, never-reused
    // cookie value. Predict it the same way.
    const std::uint64_t cookie_b = backend.peek_next_cookie_for_test();
    SLUICE_CHECK(cookie_b != cookie_a); // cookie never reused
    std::array<std::byte, 4> b_bytes{std::byte{0xB1}, std::byte{0xB2}, std::byte{0xB3},
                                     std::byte{0xB4}};
    Completion<std::size_t> b_completion;
    SLUICE_CHECK(
        backend.submit_write(WriteOp{file.fd(), b_bytes.data(), b_bytes.size(), 4}, b_completion)
            .has_value());
    SLUICE_CHECK(backend.outstanding() == 1);

    // INJECT THE STALE COOKIE (A's). Under the cookie-keyed router this matches
    // no LIVE entry (A's entry was retired) and MUST be dropped. B is currently
    // ring-owned with cookie_b; it MUST NOT be affected.
    backend.inject_cqe_for_test(cookie_a, /*res=*/999); // bogus result for A
    SLUICE_CHECK(!b_completion.ready());           // B not terminalized by stale cookie
    SLUICE_CHECK(backend.outstanding() == 1);      // B still outstanding
    SLUICE_CHECK(backend.arena_accepted_outstanding() == 1);

    // B completes normally from its own real CQE.
    SLUICE_CHECK(poll_bounded(backend, [&] { return b_completion.ready(); }) == 1);
    SLUICE_CHECK(b_completion.ready());
    SLUICE_CHECK(b_completion.result().has_value());
    SLUICE_CHECK(b_completion.result().value() == b_bytes.size());
    b_completion.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);

    // On-disk verification: A then B, A's stale CQE did not corrupt B's write.
    std::array<std::byte, 8> on_disk{};
    const ssize_t n = ::pread(file.fd(), on_disk.data(), on_disk.size(), 0);
    SLUICE_CHECK(n == 8);
    SLUICE_CHECK(std::memcmp(on_disk.data(), a_bytes.data(), 4) == 0);
    SLUICE_CHECK(std::memcmp(on_disk.data() + 4, b_bytes.data(), 4) == 0);
}

// P0-C no-rollback detector (structural). After io_uring_get_sqe() succeeds,
// dispatch_one_locked MUST NOT have an ordinary rollback path: mark_running(h)
// == false is an invariant violation (the P0-A lock discipline means no cancel
// can have terminalized h between enqueue and dispatch), so it MUST fail-fast
// rather than "drop the prepared SQE". Constructing the illegal state in
// production is not possible without a corrupting seam; the protection is
// structural (the fail-fast is unconditional). This test documents that
// invariant and verifies the happy path that exercises the no-fail region
// (get_sqe -> fill -> mark_running succeeds -> remove_exact). A death-test
// harness integration to force mark_running==false post-get_sqe is left to a
// future shared death-runner wiring for the uring test target; the invariant
// is enforced by the unconditional terminate in dispatch_one_locked.
SLUICE_TEST_CASE(uring_no_rollback_region_happy_path_exercises_mark_running) {
    UringAsyncBackend backend(small_config(4));
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    Completion<std::size_t> completion;
    // submit + poll drives: get_sqe -> fill -> set_data64 -> mark_running
    // (succeeds) -> remove_exact. The request becomes ring-owned and completes
    // from its real CQE. (Mark_running==false would terminate here per P0-C.)
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), bytes.data(), bytes.size(), 0}, completion)
                     .has_value());
    SLUICE_CHECK(backend.outstanding() == 1);
    SLUICE_CHECK(poll_bounded(backend, [&] { return completion.ready(); }) == 1);
    SLUICE_CHECK(completion.ready());
    SLUICE_CHECK(completion.result().has_value());
    SLUICE_CHECK(completion.result().value() == bytes.size());
    completion.reset();
    SLUICE_CHECK(backend.outstanding() == 0);
}

// Capability note (frozen design §4.2 / §8.1). The enqueue-after-commit race
// window (cancel terminalizes h between dispatch_->push_back and
// dispatch_one_locked) is CLOSED by holding dispatch_mtx_ across push_back →
// dispatch_one_locked in enqueue_after_commit (cancel takes the same lock). A
// deterministic cross-thread detector is not constructible in the current
// AsyncIoContext model because access_mtx_ serializes ALL backend operations
// (submit/poll/wait_one/cancel) at the context layer — there is no concurrent
// producer of a cancel() against an in-flight submit(). The protection is
// therefore verified structurally (the single critical section) and by TSan at
// the backend-internal dispatch_mtx_ boundary.

// P0-D: queue_depth=3 is rounded by Linux to an actual four-entry SQ. Four
// operation SQEs fill that physical ring; requests 5 and 6 remain in the local
// FIFO after two scripted no-progress/permanent flushes. A correct ledger is
// sized from ring.sq.ring_entries (4, not configured depth 3) and the poison
// controller retires all six accepted requests: four proven Class-A ledger
// entries plus two never-dispatched local entries. The cleanup branch lets the
// pre-fix code submit and reap everything before the RED assertion is made, so
// failure is attributable to missing recovery rather than destructor fail-fast.
SLUICE_TEST_CASE(uring_permanent_submit_failure_retires_physical_batch_and_local_fifo) {
    constexpr std::array steps{0, -EIO, kRealSubmit, kRealSubmit, kRealSubmit, kRealSubmit};
    SubmitScript script(steps);
    UringAsyncBackend backend(UringConfig{6, 3}, hooks_for(script));
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::array<std::byte, 1>, 7> bytes{};
    std::array<Completion<std::size_t>, 7> completions;
    for (std::size_t i = 0; i < 6; ++i) {
        bytes[i][0] = static_cast<std::byte>(i + 1);
        SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), bytes[i].data(), 1, i}, completions[i])
                         .has_value());
    }

    const std::size_t first_reap = backend.poll();
    bool all_recovered =
        first_reap == 6 && backend.outstanding() == 0 && backend.live_cookies_for_test() == 0;
    for (std::size_t i = 0; i < 6; ++i) {
        all_recovered = all_recovered && completions[i].ready() &&
                        !completions[i].result().has_value() &&
                        completions[i].result().error().code == IoError::Code::backend_error &&
                        completions[i].result().error().os_errno == EIO;
    }

    // Poison rejects a fresh operation synchronously with Completion idle and
    // no accepted residue. The old implementation accepts it; keep that path
    // quiescent before asserting RED.
    const auto after_poison =
        backend.submit_write(WriteOp{file.fd(), bytes[6].data(), 1, 6}, completions[6]);
    const bool admission_rejected = !after_poison.has_value() &&
                                    after_poison.error().code == IoError::Code::backend_error &&
                                    after_poison.error().os_errno == EIO && !completions[6].ready();

    if (!all_recovered || !admission_rejected) {
        (void)poll_bounded(backend, [&] {
            for (std::size_t i = 0; i < 7; ++i) {
                if (completions[i].outstanding())
                    return false;
            }
            return true;
        });
    }
    for (auto& completion : completions) {
        if (completion.ready())
            completion.reset();
    }

    SLUICE_CHECK(all_recovered);
    SLUICE_CHECK(admission_rejected);
}

// P0-D mixed Class-C/Class-A detector. A blocking pipe read is positively
// submitted first (Class-C). A later file write is the sole member of a
// permanent-failure batch (Class-A). Recovery must terminalize only the write;
// after poison, wait_one must enter with to_submit=0 so the quarantined write
// never executes while the older pipe read still completes from its real CQE.
// A spare real-submit script step keeps pre-fix cleanup quiescent and makes its
// forbidden resubmission visible as bytes written to the file.
SLUICE_TEST_CASE(uring_poison_wait_drains_old_kernel_work_without_resubmitting_class_a) {
    constexpr std::array steps{kRealSubmit, -EIO, kRealSubmit, kRealSubmit};
    SubmitScript script(steps);
    UringAsyncBackend backend(UringConfig{2, 2}, hooks_for(script));
    if (!backend.available())
        return;

    int pipe_fds[2]{-1, -1};
    SLUICE_CHECK(::pipe(pipe_fds) == 0);
    TempFile file;
    std::byte read_byte{};
    std::array<std::byte, 4> write_bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    Completion<std::size_t> read_completion;
    Completion<std::size_t> write_completion;

    SLUICE_CHECK(
        backend.submit_read(ReadOp{pipe_fds[0], &read_byte, 1, 0}, read_completion).has_value());
    SLUICE_CHECK(backend.poll() == 0); // real submit: pipe read is now Class-C and blocked

    SLUICE_CHECK(backend
                     .submit_write(WriteOp{file.fd(), write_bytes.data(), write_bytes.size(), 0},
                                   write_completion)
                     .has_value());
    const std::size_t poison_reap = backend.poll();
    const bool write_recovered =
        poison_reap == 1 && write_completion.ready() && !write_completion.result().has_value() &&
        write_completion.result().error().code == IoError::Code::backend_error &&
        write_completion.result().error().os_errno == EIO;

    const unsigned char seed = 0x6b;
    SLUICE_CHECK(::write(pipe_fds[1], &seed, 1) == 1);
    const auto waited = backend.wait_one();
    const bool old_kernel_work_drained =
        waited.has_value() && waited.value() == 1 && read_completion.ready() &&
        read_completion.result().has_value() && read_completion.result().value() == 1 &&
        read_byte == std::byte{0x6b};

    if (!write_completion.ready())
        (void)poll_bounded(backend, [&] { return write_completion.ready(); });
    std::array<std::byte, 4> on_disk{};
    const ssize_t disk_bytes = ::pread(file.fd(), on_disk.data(), on_disk.size(), 0);
    SLUICE_CHECK(disk_bytes >= 0);
    const bool quarantined_write_never_executed = disk_bytes == 0;

    if (read_completion.ready())
        read_completion.reset();
    if (write_completion.ready())
        write_completion.reset();
    (void)::close(pipe_fds[0]);
    (void)::close(pipe_fds[1]);

    SLUICE_CHECK(write_recovered);
    SLUICE_CHECK(old_kernel_work_drained);
    SLUICE_CHECK(quarantined_write_never_executed);
}

// P0-D control-quiescence detector. A positively submitted AsyncCancel is a
// backend execution reference even after the original operation CQE arrives.
// The operation result must therefore remain outstanding (not caller-visible
// ready/releasable) until the matching control CQE retires. The control identity
// uses the frozen tagged-cookie encoding: high bit set, low bits = operation
// cookie. This ordering is deliberately adversarial: original CQE first,
// informational control CQE second.
SLUICE_TEST_CASE(uring_original_cqe_waits_for_matching_control_quiescence) {
    constexpr std::array steps{2}; // operation + AsyncCancel become Class-C
    SubmitScript script(steps);
    UringAsyncBackend backend(UringConfig{1, 2}, hooks_for(script));
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::byte, 1> byte{std::byte{0x41}};
    Completion<std::size_t> completion;
    const std::uint64_t operation_cookie = backend.peek_next_cookie_for_test();
    constexpr std::uint64_t control_tag = std::uint64_t{1} << 63u;
    const std::uint64_t control_cookie = control_tag | operation_cookie;

    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), byte.data(), byte.size(), 0}, completion)
                     .has_value());
    backend.cancel(completion);        // running -> intent + one AsyncCancel SQE
    SLUICE_CHECK(backend.poll() == 0); // scripted positive submission of both SQEs

    // Adversarial CQE order: the original operation reports first. A correct
    // backend retains its terminal in bounded router scratch and withholds reap
    // until the matching control reference is retired.
    backend.inject_cqe_for_test(operation_cookie, /*res=*/1);
    const std::size_t before_control = backend.poll();
    const bool withheld_for_control = before_control == 0 && completion.outstanding() &&
                                      !completion.ready() && backend.outstanding() == 1;

    if (withheld_for_control) {
        backend.inject_cqe_for_test(control_cookie, /*res=*/-ENOENT);
    } else {
        // Keep the pre-fix RED run quiescent: it uses the old global
        // UINT64_MAX control identity and has already published the operation.
        backend.inject_cqe_for_test(std::numeric_limits<std::uint64_t>::max(), /*res=*/-ENOENT);
    }
    const std::size_t after_control = backend.poll();
    const bool published_after_control =
        completion.ready() && completion.result().has_value() && completion.result().value() == 1;
    if (completion.ready())
        completion.reset();

    SLUICE_CHECK(withheld_for_control);
    SLUICE_CHECK(after_control == 1);
    SLUICE_CHECK(published_after_control);
}

// A Class-C operation may have an original CQE while its later cancel-control
// suffix is still application-side. If that control suffix then gets the
// permanent-negative Class-A proof, recovery retires only the control reference
// and publishes the already-proven operation result verbatim.
SLUICE_TEST_CASE(uring_class_a_control_suffix_releases_deferred_class_c_operation) {
    constexpr std::array steps{1, -EIO};
    SubmitScript script(steps);
    UringAsyncBackend backend(UringConfig{1, 2}, hooks_for(script));
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::byte, 1> byte{std::byte{0x51}};
    Completion<std::size_t> completion;
    const std::uint64_t operation_cookie = backend.peek_next_cookie_for_test();
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), byte.data(), byte.size(), 0}, completion)
                     .has_value());
    SLUICE_CHECK(backend.poll() == 0); // operation is positively submitted Class-C

    backend.cancel(completion); // later control remains in the physical ledger
    backend.inject_cqe_for_test(operation_cookie, /*res=*/1); // original arrives first
    SLUICE_CHECK(!completion.ready());
    SLUICE_CHECK(backend.poll() == 1); // -EIO proves only the control suffix Class-A
    SLUICE_CHECK(completion.ready());
    SLUICE_CHECK(completion.result().has_value());
    SLUICE_CHECK(completion.result().value() == 1);
    completion.reset();
}

// When operation and its cancel control are both in one permanent-negative
// batch, neither can execute. Recovery walks the monotonic ledger in order,
// defers the operation terminal across its prepared control, retires that
// control locally, and publishes exactly one backend_error.
SLUICE_TEST_CASE(uring_class_a_operation_and_control_retire_exactly_once) {
    constexpr std::array steps{-EIO};
    SubmitScript script(steps);
    UringAsyncBackend backend(UringConfig{1, 2}, hooks_for(script));
    if (!backend.available())
        return;

    TempFile file;
    std::array<std::byte, 1> byte{std::byte{0x61}};
    Completion<std::size_t> completion;
    SLUICE_CHECK(backend.submit_write(WriteOp{file.fd(), byte.data(), byte.size(), 0}, completion)
                     .has_value());
    backend.cancel(completion);

    SLUICE_CHECK(backend.poll() == 1);
    SLUICE_CHECK(completion.ready());
    SLUICE_CHECK(!completion.result().has_value());
    SLUICE_CHECK(completion.result().error().code == IoError::Code::backend_error);
    SLUICE_CHECK(completion.result().error().os_errno == EIO);
    SLUICE_CHECK(backend.outstanding() == 0);
    SLUICE_CHECK(backend.live_cookies_for_test() == 0);
    completion.reset();
}

SLUICE_MAIN()
