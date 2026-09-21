// The named VERIFY-04 reference cases that the A1 slice can express, run through
// the shared oracle rather than through per-path expectations.
//
// V01-V03 are precedence cases and are fully observable on the direct path and
// on a request path. V15-V17 are effect and durability cases: their rules are
// asserted here, and the parts that need request-path capability (an unobserved
// write followed by a sync, or a request terminal that can carry an unaccounted
// remainder) are pinned as recorded divergences pointing at the request slice
// rather than claimed as covered.
#include "semantic_path_probes.hpp"

#include <sluice/async/detail/request_slot.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/detail/file_semantics.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileInitialContents;
using sluice::FileOpen;
using sluice::IoError;
using sluice::detail::CompletionState;
using sluice::detail::covers;
using sluice::detail::EffectCertainty;
using sluice::detail::FileOperation;
using sluice::detail::failed_dispatched_attempt;
using sluice::detail::IoOutcome;
using sluice::detail::MutationKind;
using sluice::detail::MutationRecord;
using sluice::detail::ordered_after_sync;
using sluice::detail::prefix_only_terminal_can_carry;
using sluice::detail::preserves_exact_state;
using sluice::detail::SyncKind;
using sluice::detail::SyncRecord;

int failures = 0;

void check(bool condition, const char* name) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", name);
    ++failures;
}

// ── V01 / V02: semantic rejections precede any admission or OS work ────────

void v01_closed_file_with_zero_buffer_is_invalid_state() {
    sluice_semantic::AccessFixtures fixtures = sluice_semantic::AccessFixtures::create(16);
    if (!fixtures.ok()) {
        check(false, "v01 fixtures");
        return;
    }

    // Direct path: the closed File is rejected before anything else.
    const sluice_semantic::Input input{true, FileAccess::read_only, FileOperation::read, 0, 0};
    const sluice_semantic::Observation direct = sluice_semantic::direct_attempt(fixtures, input);
    check(direct.rejected && direct.error.code == IoError::Code::invalid_state,
          "V01 direct: closed + zero buffer is invalid_state");

    // Request path: the rejection happens before admission, so no slot is taken.
    sluice_semantic::RequestProbe probe(std::make_unique<sluice::async::ThreadPoolBackend>(
        sluice::async::ThreadPoolConfig{4, 1}));
    const sluice_semantic::Observation request = probe.attempt(fixtures, input);
    check(request.rejected && request.error.code == IoError::Code::invalid_state,
          "V01 request: closed + zero buffer is invalid_state");
    check(probe.context().outstanding() == 0, "V01 request: no accepted work retained");
}

void v02_illegal_access_with_zero_buffer_is_invalid_argument() {
    sluice_semantic::AccessFixtures fixtures = sluice_semantic::AccessFixtures::create(16);
    if (!fixtures.ok()) {
        check(false, "v02 fixtures");
        return;
    }

    const sluice_semantic::Input read_on_write_only{
        false, FileAccess::write_only, FileOperation::read, 0, 0};
    const sluice_semantic::Input write_on_read_only{
        false, FileAccess::read_only, FileOperation::write, 0, 0};

    for (const sluice_semantic::Input& input : {read_on_write_only, write_on_read_only}) {
        const sluice_semantic::Observation direct =
            sluice_semantic::direct_attempt(fixtures, input);
        check(direct.rejected && direct.error.code == IoError::Code::invalid_argument,
              "V02 direct: illegal access + zero buffer is invalid_argument");

        sluice_semantic::RequestProbe probe(std::make_unique<sluice::async::ThreadPoolBackend>(
            sluice::async::ThreadPoolConfig{4, 1}));
        const sluice_semantic::Observation request = probe.attempt(fixtures, input);
        check(request.rejected && request.error.code == IoError::Code::invalid_argument,
              "V02 request: illegal access + zero buffer is invalid_argument");
        check(probe.context().outstanding() == 0, "V02 request: no accepted work retained");
    }
}

// ── V03: a logical no-op still needs a slot on the request path ─────────────

void v03_zero_request_with_full_table_is_admission_rejected() {
    sluice_semantic::AccessFixtures fixtures = sluice_semantic::AccessFixtures::create(16);
    if (!fixtures.ok()) {
        check(false, "v03 fixtures");
        return;
    }
    const File* file = fixtures.for_access(FileAccess::read_only);
    if (file == nullptr) {
        check(false, "v03 file");
        return;
    }

    // Same direct operation returns 0.
    check(sluice::blocking::read_at(*file, 0, std::span<std::byte>()).value_or(1) == 0,
          "V03 direct: zero-length read returns 0");

    // Request path with capacity 1: hold the only slot with an unreaped request,
    // then submit a zero-length request, which still requires acceptance.
    sluice::async::AsyncIoContext ctx(
        std::make_unique<sluice::async::ThreadPoolBackend>(
            sluice::async::ThreadPoolConfig{1, 1}));
    const sluice::async::NativeFileRef ref{*file};
    std::vector<std::byte> scratch(4, std::byte{0});

    sluice::async::Completion<std::size_t> holding;
    auto first = ctx.submit_read(sluice::async::ReadOp{ref, scratch.data(), scratch.size(), 0},
                                 holding);
    check(first.has_value(), "V03: the first request occupies the only slot");

    sluice::async::Completion<std::size_t> noop;
    auto second = ctx.submit_read(sluice::async::ReadOp{ref, scratch.data(), 0, 0}, noop);
    check(!second.has_value() && second.error().code == IoError::Code::would_block,
          "V03 request: zero-length request is admission rejected when the table is full");

    while (!holding.ready())
        (void)ctx.poll();
}

// ── V15: an unknown-effect failure is not a zero-byte claim ────────────────

void v15_unknown_effect_write_failure() {
    // The rule is evidence-driven: a dispatched attempt that failed may have
    // taken effect with no trustworthy count, whatever its direction.
    const IoOutcome outcome = failed_dispatched_attempt(
        IoError{.code = IoError::Code::backend_error, .os_errno = EIO}, 0);
    check(outcome.effect.remaining == EffectCertainty::unknown,
          "V15 rule: a dispatched failed attempt reports an unaccounted remainder");
    check(!prefix_only_terminal_can_carry(outcome),
          "V15 rule: the reference outcome is not representable in a prefix-only terminal");

    // Physical failure through the direct path, converted by the same rule.
    const int fd = ::open("/dev/full", O_WRONLY);
    if (fd < 0) {
        std::printf("NOT RUN: V15 physical write failure (/dev/full absent)\n");
        return;
    }
    ::close(fd);
    FileOpen mode;
    mode.access = FileAccess::write_only;
    mode.existence = sluice::FileExistence::open_existing;
    auto opened = File::open("/dev/full", mode);
    if (!opened.has_value()) {
        check(false, "V15 /dev/full open");
        return;
    }
    File device = std::move(opened.value());
    const std::string payload = "unknown effect";
    const auto src = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    auto written = sluice::blocking::write_at(device, 0, src);
    check(!written.has_value(), "V15 physical: the write fails");
    if (!written.has_value()) {
        const IoOutcome converted = failed_dispatched_attempt(written.error());
        check(converted.effect.remaining == EffectCertainty::unknown,
              "V15 physical: the failure becomes an unaccounted remainder");
    }

    // Characterization of the request-side storage today, labelled as such: it
    // shows what the shared terminal can carry and it fails if the error factory
    // starts preserving a count. It is NOT a pin on the V15 gap itself, because a
    // request-side fix could add an effect-certainty channel elsewhere and leave
    // this shape untouched. V15's request-side evidence is deferred, not claimed.
    const sluice::async::detail::TerminalResult error_only =
        sluice::async::detail::TerminalResult::err(
            IoError{.code = IoError::Code::backend_error, .os_errno = EIO});
    check(error_only.is_error && error_only.bytes == 0,
          "V15 characterization: TerminalResult::err records an error with a zero byte count");
    std::printf("DEFERRED: V15 request-side representation evidence (owner #400); the shared "
                "request terminal has no effect-certainty channel and its error factory writes a "
                "zero byte count\n");
}

// ── V16: a submitted write is not covered by a later sync ──────────────────

void v16_outstanding_write_is_not_covered() {
    constexpr SyncRecord data_sync{SyncKind::data, true, 10};
    constexpr SyncRecord all_sync{SyncKind::all, true, 10};
    constexpr MutationRecord submitted{MutationKind::write, CompletionState::submitted, 0};
    constexpr MutationRecord observed{MutationKind::write, CompletionState::observed, 9};

    check(!covers(data_sync, submitted) && !covers(all_sync, submitted),
          "V16: a submitted write is not covered by either sync");
    // Positive control: the same write is covered once its completion is observed
    // before the sync initiation, which is what makes V16 about ordering.
    check(covers(data_sync, observed) && covers(all_sync, observed),
          "V16 positive control: an observed write is covered");

    // The request slice owns the production schedule for this case; the direct
    // path cannot submit a write without observing it.
    std::printf("DEFERRED: V16 request-path schedule evidence (needs an unobserved write "
                "followed by a sync; owner #400)\n");
}

// ── V17: coverage is not exact-state preservation ─────────────────────────

void v17_coverage_is_not_preservation() {
    constexpr SyncRecord data_sync{SyncKind::data, true, 4};
    constexpr MutationRecord covered{MutationKind::write, CompletionState::observed, 3};
    constexpr MutationRecord conflicting{MutationKind::write, CompletionState::observed, 8};

    check(covers(data_sync, covered), "V17: the earlier write is covered");
    check(!covers(data_sync, conflicting) && ordered_after_sync(data_sync, conflicting),
          "V17: a later conflicting mutation is uncovered and ordered after the sync");
    // Covered earlier state plus a conflicting mutation ordered after the sync's
    // initiation is the pair SEM-06 names as superseding the covered state, so
    // no exact-state preservation is claimed.
    check(!preserves_exact_state(data_sync, conflicting),
          "V17: the covered state is not preserved");

    // Observable half on a real file: after a conflicting write, the bytes on
    // disk are no longer the bytes the earlier sync covered.
    char path[] = "/tmp/sluice_v17_XXXXXX";
    const int raw = ::mkstemp(path);
    if (raw < 0) {
        check(false, "V17 temp file");
        return;
    }
    ::close(raw);
    FileOpen mode;
    mode.access = FileAccess::read_write;
    auto opened = File::open(path, mode);
    ::unlink(path);
    if (!opened.has_value()) {
        check(false, "V17 open");
        return;
    }
    File file = std::move(opened.value());

    const auto write_four = [&](const char* text) {
        return sluice::blocking::write_at(
            file, 0, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text), 4));
    };
    check(write_four("AAAA").has_value() && sluice::blocking::sync_data(file).has_value(),
          "V17: first write and sync succeed");
    check(write_four("BBBB").has_value() && sluice::blocking::sync_data(file).has_value(),
          "V17: conflicting write and sync succeed");

    std::vector<std::byte> readback(4, std::byte{0});
    auto read = sluice::blocking::read_at(file, 0, std::span<std::byte>(readback));
    check(read.has_value() && read.value() == 4 &&
              std::memcmp(readback.data(), "BBBB", 4) == 0,
          "V17: the superseded bytes are no longer the file contents");
}

// ── V27: a completed resize is covered, and grants nothing by itself ───────

void v27_resize_durability() {
    constexpr SyncRecord data_sync{SyncKind::data, true, 6};
    constexpr SyncRecord all_sync{SyncKind::all, true, 6};
    constexpr MutationRecord shrink{MutationKind::resize_shrink, CompletionState::observed, 5};
    constexpr MutationRecord grow{MutationKind::resize_grow, CompletionState::observed, 5};

    check(covers(data_sync, shrink) && covers(all_sync, shrink) && covers(data_sync, grow) &&
              covers(all_sync, grow),
          "V27: completed shrink and grow are covered by both sync kinds");
    check(!sluice::detail::grants_durability_alone(shrink) &&
              !sluice::detail::grants_durability_alone(grow),
          "V27: resize alone grants no durability");

    // Real sequence through the direct path, including a write after the resize
    // so a covered write coexists with the covered file-size change.
    char path[] = "/tmp/sluice_v27_XXXXXX";
    const int raw = ::mkstemp(path);
    if (raw < 0) {
        check(false, "V27 temp file");
        return;
    }
    ::close(raw);
    FileOpen mode;
    mode.access = FileAccess::read_write;
    auto opened = File::open(path, mode);
    ::unlink(path);
    if (!opened.has_value()) {
        check(false, "V27 open");
        return;
    }
    File file = std::move(opened.value());

    check(sluice::blocking::resize(file, 8192).has_value() &&
              sluice::blocking::sync_all(file).has_value(),
          "V27 direct: grow then sync_all");
    check(sluice::blocking::resize(file, 512).has_value() &&
              sluice::blocking::sync_data(file).has_value(),
          "V27 direct: shrink then sync_data");
    auto size = sluice::blocking::size(file);
    check(size.has_value() && size.value() == 512, "V27 direct: the size change is observable");
}

} // namespace

int main() {
    v01_closed_file_with_zero_buffer_is_invalid_state();
    v02_illegal_access_with_zero_buffer_is_invalid_argument();
    v03_zero_request_with_full_table_is_admission_rejected();
    v15_unknown_effect_write_failure();
    v16_outstanding_write_is_not_covered();
    v17_coverage_is_not_preservation();
    v27_resize_durability();

    if (failures != 0) {
        std::fprintf(stderr, "%d reference case check(s) failed\n", failures);
        return 1;
    }
    std::printf("all V01 V02 V03 V15 V16 V17 V27 reference cases passed\n");
    return 0;
}
