// Direct exact/all composition (SEM-05, ERR-02) under deterministic primitive
// injection.
//
// A real regular file will not produce short writes, a zero-progress write or an
// error after a confirmed prefix on demand, so these cases are driven through
// the test-only native-call seam at the primitive boundary. The seam fixes the
// primitive counts; whether bytes physically landed is not observable for a
// scripted call, and the unscripted integration cases live in
// direct_composition_test.
//
// The same frozen SEM-03 precedence table that the primitives are compared
// against is also driven through the composition surfaces here, because only a
// seam build can observe the no-OS-call half of the logical-no-op rule.

#include <sluice/blocking/file.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/file_resource.hpp>

#include "file_test_seams.hpp"
#include "semantic_oracle_harness.hpp"
#include "semantic_scenarios.hpp"

#include <cstddef>
#include <optional>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using sluice::File;
using sluice::IoError;
using sluice::blocking::CompositionEnd;
using sluice::blocking::CompositionOutcome;
using sluice::file_testing::kTransferCalls;
using sluice::file_testing::NativeScript;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_composition_fault_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    if (!content.empty()) {
        const ssize_t n = ::write(fd, content.data(), content.size());
        if (n != static_cast<ssize_t>(content.size())) {
            ::close(fd);
            return {};
        }
    }
    ::close(fd);
    return path;
}

std::optional<File> opened_with_content(const std::string& content, std::string& path,
                                         sluice::FileAccess access = sluice::FileAccess::read_only) {
    path = make_temp_file(content);
    if (path.empty())
        return std::nullopt;
    sluice::FileOpen mode;
    mode.existence = sluice::FileExistence::open_existing;
    mode.access = access;
    auto opened = File::open(path, mode);
    if (!opened.has_value())
        return std::nullopt;
    return std::optional<File>(std::move(opened).value());
}

// Short reads accumulate the confirmed prefix and end as EOF-before-full, not as
// an operation error.
bool short_reads_report_eof_before_full_with_the_prefix() {
    std::string path;
    std::optional<File> file_holder = opened_with_content("abcdef", path);
    if (!file_holder.has_value())
        return false;
    File& file = *file_holder;
    const int fd = file.native_handle();
    std::vector<std::byte> dst(16, std::byte{0});

    bool ok = true;
    {
        NativeScript script(kTransferCalls, fd, {{4, 0}, {2, 0}, {0, 0}});
        auto composed = sluice::blocking::read_exact_at(file, 0, dst);
        ok = ok && composed.has_value();
        const CompositionOutcome& outcome = composed.value();
        ok = ok && outcome.confirmed_bytes == 6;
        ok = ok && outcome.end == CompositionEnd::eof_before_full;
        ok = ok && !outcome.complete();
        ok = ok && script.calls() == 3;
    }
    ::unlink(path.c_str());
    return ok;
}

// Short reads keep advancing until the request is satisfied.
bool short_reads_reach_full_progress() {
    std::string path;
    std::optional<File> file_holder = opened_with_content("abcdefgh", path);
    if (!file_holder.has_value())
        return false;
    File& file = *file_holder;
    const int fd = file.native_handle();
    std::vector<std::byte> dst(8, std::byte{0});

    bool ok = true;
    {
        NativeScript script(kTransferCalls, fd, {{3, 0}, {5, 0}});
        auto composed = sluice::blocking::read_exact_at(file, 0, dst);
        ok = ok && composed.has_value();
        ok = ok && composed.value().complete();
        ok = ok && composed.value().confirmed_bytes == 8;
        ok = ok && script.calls() == 2;
    }
    ::unlink(path.c_str());
    return ok;
}

// Short writes advance by the confirmed count until every byte is written.
bool short_writes_reach_full_progress() {
    std::string path;
    std::optional<File> file_holder = opened_with_content("", path, sluice::FileAccess::read_write);
    if (!file_holder.has_value())
        return false;
    File& file = *file_holder;
    const int fd = file.native_handle();
    const std::vector<std::byte> src(5, std::byte{0x41});

    bool ok = true;
    {
        NativeScript script(kTransferCalls, fd, {{2, 0}, {3, 0}});
        auto composed = sluice::blocking::write_all_at(file, 0, src);
        ok = ok && composed.has_value();
        ok = ok && composed.value().complete();
        ok = ok && composed.value().confirmed_bytes == 5;
        ok = ok && script.calls() == 2;
    }
    ::unlink(path.c_str());
    return ok;
}

// A nonempty write that transfers nothing stops the composition after one
// attempt: zero progress must terminate, never spin.
bool zero_progress_write_stops_after_one_attempt() {
    std::string path;
    std::optional<File> file_holder = opened_with_content("", path, sluice::FileAccess::read_write);
    if (!file_holder.has_value())
        return false;
    File& file = *file_holder;
    const int fd = file.native_handle();
    const std::vector<std::byte> src(4, std::byte{0x42});

    bool ok = true;
    {
        NativeScript script(kTransferCalls, fd, {{0, 0}, {4, 0}, {4, 0}});
        auto composed = sluice::blocking::write_all_at(file, 0, src);
        ok = ok && composed.has_value();
        ok = ok && composed.value().end == CompositionEnd::write_no_progress;
        ok = ok && composed.value().confirmed_bytes == 0;
        // The remaining scripted steps are untouched: the loop stopped at the
        // first zero-progress primitive instead of retrying it.
        ok = ok && script.calls() == 1;
    }
    ::unlink(path.c_str());
    return ok;
}

// SEM-05: a composition advances its buffer and offset by the confirmed bytes.
// The seam records the operands of the last intercepted call, so a loop that
// restated the count without moving the buffer (or the offset) fails here even
// though the outcome's numbers look right.
bool composition_advances_the_buffer_by_confirmed_bytes() {
    std::string path;
    std::optional<File> file_holder =
        opened_with_content("abcdefgh", path, sluice::FileAccess::read_write);
    if (!file_holder.has_value())
        return false;
    File& file = *file_holder;
    const int fd = file.native_handle();

    std::vector<std::byte> dst(8, std::byte{0});
    std::vector<std::byte> src(5, std::byte{0x44});

    bool ok = true;
    {
        NativeScript script(kTransferCalls, fd, {{3, 0}, {5, 0}});
        auto composed = sluice::blocking::read_exact_at(file, 100, dst);
        ok = ok && composed.has_value() && composed.value().complete();
        // The second call must address the remainder of the buffer at the
        // advanced offset, not the start of it again.
        ok = ok && script.last_call().buffer == dst.data() + 3;
        ok = ok && script.last_call().count == 5;
        ok = ok && script.last_call().offset == 103;
    }
    {
        NativeScript script(kTransferCalls, fd, {{2, 0}, {3, 0}});
        auto composed = sluice::blocking::write_all_at(file, 20, src);
        ok = ok && composed.has_value() && composed.value().complete();
        ok = ok && script.last_call().buffer == src.data() + 2;
        ok = ok && script.last_call().count == 3;
        ok = ok && script.last_call().offset == 22;
    }
    {
        // The shared-cursor form advances the buffer the same way and reports no
        // offset, because the kernel owns the cursor.
        NativeScript script(kTransferCalls, fd, {{2, 0}, {3, 0}});
        auto composed = sluice::blocking::write_all(file, src);
        ok = ok && composed.has_value() && composed.value().complete();
        ok = ok && script.last_call().buffer == src.data() + 2;
        ok = ok && script.last_call().count == 3;
        ok = ok && script.last_call().offset == -1;
    }
    ::unlink(path.c_str());
    return ok;
}

// A primitive error after a confirmed prefix keeps the prefix and reports the
// primitive's own reason next to it (ERR-02).
bool error_after_confirmed_prefix_keeps_the_prefix() {
    std::string path;
    std::optional<File> file_holder = opened_with_content("", path, sluice::FileAccess::read_write);
    if (!file_holder.has_value())
        return false;
    File& file = *file_holder;
    const int fd = file.native_handle();
    const std::vector<std::byte> src(1024, std::byte{0x43});

    bool ok = true;
    {
        NativeScript script(kTransferCalls, fd, {{256, 0}, {-1, ENOSPC}});
        auto composed = sluice::blocking::write_all_at(file, 0, src);
        ok = ok && composed.has_value();
        const CompositionOutcome& outcome = composed.value();
        ok = ok && outcome.confirmed_bytes == 256;
        ok = ok && outcome.end == CompositionEnd::primitive_error;
        ok = ok && outcome.error.has_value() &&
             outcome.error->code == IoError::Code::no_space &&
             outcome.error->os_errno == ENOSPC;
        ok = ok && script.calls() == 2;
    }
    ::unlink(path.c_str());
    return ok;
}

// SEM-05: a primitive may retry an interrupted attempt that reported no
// successful count, and the composition sees only the completed counts.
bool primitive_retries_eintr_without_a_completed_count() {
    std::string path;
    std::optional<File> file_holder = opened_with_content("abcdefgh", path);
    if (!file_holder.has_value())
        return false;
    File& file = *file_holder;
    const int fd = file.native_handle();
    std::vector<std::byte> dst(4, std::byte{0});

    bool ok = true;
    {
        NativeScript script(kTransferCalls, fd, {{-1, EINTR}, {4, 0}});
        auto read = sluice::blocking::read_at(file, 0, dst);
        ok = ok && read.has_value() && read.value() == 4;
        ok = ok && script.calls() == 2;
    }
    {
        NativeScript script(kTransferCalls, fd, {{-1, EINTR}, {4, 0}});
        auto composed = sluice::blocking::read_exact_at(file, 0, dst);
        ok = ok && composed.has_value();
        ok = ok && composed.value().complete();
        ok = ok && composed.value().confirmed_bytes == 4;
        ok = ok && script.calls() == 2;
    }
    ::unlink(path.c_str());
    return ok;
}

// A logical no-op is completed without a native call. The armed script fails
// every intercepted transfer call, so "success 0 with zero intercepted calls" is
// the mechanical no-OS-call evidence for the direct path.
bool zero_length_requests_make_no_native_call() {
    std::string path;
    std::optional<File> file_holder = opened_with_content("abcdefgh", path, sluice::FileAccess::read_write);
    if (!file_holder.has_value())
        return false;
    File& file = *file_holder;
    const int fd = file.native_handle();
    std::span<std::byte> dst;
    std::span<const std::byte> src;

    bool ok = true;
    {
        NativeScript script(kTransferCalls, fd, {{-1, EIO}, {-1, EIO}});
        auto at = sluice::blocking::read_at(file, 0, dst);
        auto cursor = sluice::blocking::read(file, dst);
        auto at_write = sluice::blocking::write_at(file, 0, src);
        auto cursor_write = sluice::blocking::write(file, src);
        auto exact_at = sluice::blocking::read_exact_at(file, 0, dst);
        auto exact = sluice::blocking::read_exact(file, dst);
        auto all_at = sluice::blocking::write_all_at(file, 0, src);
        auto all = sluice::blocking::write_all(file, src);
        ok = ok && at.has_value() && at.value() == 0;
        ok = ok && cursor.has_value() && cursor.value() == 0;
        ok = ok && at_write.has_value() && at_write.value() == 0;
        ok = ok && cursor_write.has_value() && cursor_write.value() == 0;
        ok = ok && exact_at.has_value() && exact_at.value().complete();
        ok = ok && exact_at.value().confirmed_bytes == 0;
        ok = ok && exact.has_value() && exact.value().complete();
        ok = ok && all_at.has_value() && all_at.value().complete();
        ok = ok && all.has_value() && all.value().complete();
        ok = ok && script.calls() == 0;
    }
    ::unlink(path.c_str());
    return ok;
}

// A primitive that claims more than the remaining request is an invariant
// violation: the loop stops immediately instead of advancing past the buffer.
bool impossible_count_stops_immediately() {
    std::string path;
    std::optional<File> file_holder = opened_with_content("abcdefgh", path);
    if (!file_holder.has_value())
        return false;
    File& file = *file_holder;
    const int fd = file.native_handle();
    std::vector<std::byte> dst(4, std::byte{0});

    bool ok = true;
    {
        NativeScript script(kTransferCalls, fd, {{8, 0}, {8, 0}});
        auto composed = sluice::blocking::read_exact_at(file, 0, dst);
        ok = ok && composed.has_value();
        ok = ok && composed.value().end == CompositionEnd::primitive_error;
        ok = ok && composed.value().confirmed_bytes == 0;
        ok = ok && composed.value().error.has_value() &&
             composed.value().error->code == IoError::Code::invalid_state;
        ok = ok && script.calls() == 1;
    }
    ::unlink(path.c_str());
    return ok;
}

// The canonical composition semantics are the shared state: `CompositionStop`,
// the accumulated `confirmed_bytes` and the primitive's own error, decided once
// by `detail::compose_progress`/`compose_error`. Each path chooses a
// representation of that state, and two of them are compared here from one
// injected primitive sequence: the direct outcome reports the stop structurally
// and carries an error only for a primitive failure, while `composition_error` —
// a reference `IoError` projection with no production consumer — names a
// category that the root does not assign to the no-progress stop. Driving both
// from the same sequence pins the canonical stop state on both sides, so a
// change in either representation shows up here, and records the two
// representation differences instead of leaving them to read as two competing
// authorities.
bool every_stop_reason_is_pinned_against_the_oracle_error_rule() {
    using sluice::detail::CompositionKind;
    using sluice::detail::CompositionState;
    using sluice::detail::CompositionStop;

    std::string path;
    std::optional<File> file_holder =
        opened_with_content("abcdefgh", path, sluice::FileAccess::read_write);
    if (!file_holder.has_value())
        return false;
    File& file = *file_holder;
    const int fd = file.native_handle();
    std::vector<std::byte> buffer(4, std::byte{0});
    const std::vector<std::byte> src(4, std::byte{0x41});

    bool ok = true;

    // complete: neither side names a reason.
    {
        NativeScript script(kTransferCalls, fd, {{4, 0}});
        auto composed = sluice::blocking::read_exact_at(file, 0, buffer);
        CompositionState oracle;
        oracle = sluice::detail::compose_progress(CompositionKind::read_exact, 4, oracle, 4);
        ok = ok && composed.has_value() && composed.value().complete();
        ok = ok && composed.value().confirmed_bytes == 4;
        ok = ok && !composed.value().error.has_value();
        ok = ok && oracle.stop == CompositionStop::complete;
        ok = ok && !sluice::detail::composition_error(oracle).has_value();
    }

    // eof_before_full: the direct outcome keeps the stop structural and carries no
    // error, while the reference projection names `eof`. A difference of
    // representation, recorded rather than accidental.
    {
        NativeScript script(kTransferCalls, fd, {{2, 0}, {0, 0}});
        auto composed = sluice::blocking::read_exact_at(file, 0, buffer);
        CompositionState oracle;
        oracle = sluice::detail::compose_progress(CompositionKind::read_exact, 4, oracle, 2);
        oracle = sluice::detail::compose_progress(CompositionKind::read_exact, 4, oracle, 0);
        ok = ok && composed.has_value() &&
             composed.value().end == CompositionEnd::eof_before_full;
        ok = ok && composed.value().confirmed_bytes == 2;
        ok = ok && !composed.value().error.has_value();
        ok = ok && oracle.stop == CompositionStop::eof_before_full;
        const auto oracle_reason = sluice::detail::composition_error(oracle);
        ok = ok && oracle_reason.has_value() && oracle_reason->code == IoError::Code::eof;
    }

    // write_no_progress after a confirmed prefix: the prefix survives the stop on
    // both sides, and the same representation difference applies to the reason.
    {
        NativeScript script(kTransferCalls, fd, {{2, 0}, {0, 0}});
        auto composed = sluice::blocking::write_all_at(file, 0, src);
        CompositionState oracle;
        oracle = sluice::detail::compose_progress(CompositionKind::write_all, 4, oracle, 2);
        oracle = sluice::detail::compose_progress(CompositionKind::write_all, 4, oracle, 0);
        ok = ok && composed.has_value() &&
             composed.value().end == CompositionEnd::write_no_progress;
        ok = ok && composed.value().confirmed_bytes == 2;
        ok = ok && !composed.value().error.has_value();
        ok = ok && oracle.stop == CompositionStop::write_no_progress;
        const auto oracle_reason = sluice::detail::composition_error(oracle);
        ok = ok && oracle_reason.has_value() &&
             oracle_reason->code == IoError::Code::invalid_state;
    }

    // primitive_error: both sides report the primitive's own reason, native detail
    // included. The oracle state is replayed with the same conversion the
    // primitive used, so the two reasons must be equal, not merely equal in code.
    {
        NativeScript script(kTransferCalls, fd, {{2, 0}, {-1, ENOSPC}});
        auto composed = sluice::blocking::write_all_at(file, 0, src);
        CompositionState oracle;
        oracle = sluice::detail::compose_progress(CompositionKind::write_all, 4, oracle, 2);
        oracle = sluice::detail::compose_error(oracle, sluice::from_errno_value(ENOSPC));
        ok = ok && composed.has_value() &&
             composed.value().end == CompositionEnd::primitive_error;
        ok = ok && composed.value().confirmed_bytes == 2;
        ok = ok && composed.value().error.has_value() &&
             composed.value().error->code == IoError::Code::no_space &&
             composed.value().error->os_errno == ENOSPC;
        ok = ok && oracle.stop == CompositionStop::primitive_error;
        const auto oracle_reason = sluice::detail::composition_error(oracle);
        ok = ok && oracle_reason.has_value() && *oracle_reason == *composed.value().error;
    }

    // impossible_count: a count above the remaining request cannot come from a
    // primitive that honors its own contract, so it is published as a primitive
    // error whose reason is the shared rule's `invalid_state` with no native
    // detail, exactly as `composition_error` names it.
    {
        NativeScript script(kTransferCalls, fd, {{8, 0}});
        auto composed = sluice::blocking::read_exact_at(file, 0, buffer);
        CompositionState oracle;
        oracle = sluice::detail::compose_progress(CompositionKind::read_exact, 4, oracle, 8);
        ok = ok && composed.has_value() &&
             composed.value().end == CompositionEnd::primitive_error;
        ok = ok && composed.value().confirmed_bytes == 0;
        ok = ok && composed.value().error.has_value() &&
             composed.value().error->code == IoError::Code::invalid_state &&
             composed.value().error->os_errno == 0;
        ok = ok && oracle.stop == CompositionStop::impossible_count;
        const auto oracle_reason = sluice::detail::composition_error(oracle);
        ok = ok && oracle_reason.has_value() && *oracle_reason == *composed.value().error;
    }

    ::unlink(path.c_str());
    return ok;
}

// ── The frozen SEM-03 table, driven through the composition surfaces ───────

enum class Surface { positional, cursor };

sluice_semantic::Observation observe_composition(const sluice::Result<CompositionOutcome>& result) {
    if (!result.has_value())
        return sluice_semantic::observe_rejection(result.error());
    return sluice_semantic::observe_accepted();
}

sluice_semantic::Observation apply_composition(const File& file, const sluice_semantic::Input& input,
                                               Surface surface, std::span<std::byte> dst,
                                               std::span<const std::byte> src) {
    using sluice::detail::FileOperation;
    switch (input.operation) {
    case FileOperation::read:
        return observe_composition(surface == Surface::positional
                                       ? sluice::blocking::read_exact_at(file, input.offset, dst)
                                       : sluice::blocking::read_exact(file, dst));
    case FileOperation::write:
        return observe_composition(surface == Surface::positional
                                       ? sluice::blocking::write_all_at(file, input.offset, src)
                                       : sluice::blocking::write_all(file, src));
    default:
        break;
    }
    // Unreachable: the driver filters to byte operations. `not_supported` is not
    // an expectation anywhere in the table, so a leak here fails loudly.
    return sluice_semantic::observe_rejection(IoError{.code = IoError::Code::not_supported});
}

// A shared-cursor call supplies no offset, so the offset-range step has no
// operand to check there: scenarios whose verdict depends on the caller's offset
// are not expressible on that surface. They are reported as not drivable rather
// than counted as divergences, and the length half of the range rule (which both
// surfaces do take) stays covered.
bool offset_dependent(const sluice_semantic::Input& input) {
    return input.length != 0 && input.offset != 0;
}

bool composition_table_drivable(const sluice_semantic::Input& input, Surface surface) {
    if (!sluice_semantic::direct_drivable(input) ||
        !sluice::detail::is_byte_operation(input.operation))
        return false;
    return surface == Surface::positional || !offset_dependent(input);
}

sluice_semantic::Observation composition_attempt(const sluice_semantic::AccessFixtures& fixtures,
                                                 const sluice_semantic::Input& input,
                                                 Surface surface) {
    std::vector<std::byte> scratch(input.length, std::byte{0});
    const std::span<std::byte> dst(scratch.data(), input.length);
    const std::span<const std::byte> src(scratch.data(), input.length);

    // Every intercepted transfer call fails, and every descriptor is
    // intercepted: a request that completes without a native call shows up as
    // zero intercepted calls.
    NativeScript script(kTransferCalls, -1, {{-1, EIO}});
    const std::size_t before = script.calls();

    if (input.closed) {
        sluice::FileOpen mode;
        mode.existence = sluice::FileExistence::open_existing;
        mode.access = sluice::FileAccess::read_write;
        auto opened = sluice::File::open(fixtures.path(), mode);
        if (!opened.has_value())
            return sluice_semantic::observe_rejection(opened.error());
        File closed = std::move(opened).value();
        (void)closed.close();
        return apply_composition(closed, input, surface, dst, src);
    }

    const File* file = fixtures.for_access(input.access);
    if (file == nullptr)
        return sluice_semantic::observe_rejection(IoError{.code = IoError::Code::invalid_state});

    sluice_semantic::Observation observed = apply_composition(*file, input, surface, dst, src);
    if (!observed.rejected && input.length == 0) {
        observed.no_op_without_dispatch = (script.calls() == before);
    }
    return observed;
}

bool composition_surfaces_obey_the_precedence_table() {
    using sluice_semantic::Input;
    const auto* scenarios = sluice_semantic::kPrecedenceScenarios;
    const std::size_t count = sluice_semantic::kPrecedenceScenarioCount;

    sluice_semantic::AccessFixtures fixtures = sluice_semantic::AccessFixtures::create(64);
    if (!fixtures.ok()) {
        std::fprintf(stderr, "FAIL: could not create fixtures\n");
        return false;
    }

    const struct {
        const char* name;
        Surface surface;
    } surfaces[] = {
        {"read_exact_at/write_all_at", Surface::positional},
        {"read_exact/write_all", Surface::cursor},
    };

    bool ok = true;
    for (const auto& surface : surfaces) {
        std::size_t skipped = 0;
        std::vector<const char*> divergences;
        for (std::size_t i = 0; i < count; ++i) {
            if (!composition_table_drivable(scenarios[i].input, surface.surface)) {
                ++skipped;
                continue;
            }
            (void)sluice_semantic::run_path(
                surface.name, &scenarios[i], 1,
                [&](const Input& input) { return composition_attempt(fixtures, input, surface.surface); },
                &divergences);
        }
        if (!sluice_semantic::matches_recorded_divergences(surface.name, divergences, {})) {
            ok = false;
        }
        std::printf("composition %s: %zu scenarios compared, %zu not drivable%s, %zu divergences\n",
                    surface.name, count - skipped, skipped,
                    surface.surface == Surface::cursor
                        ? " (verdicts needing a caller-supplied offset)"
                        : "",
                    divergences.size());
    }
    return ok;
}

}

int main() {
    if (sluice_semantic::check_oracle_against_table(
            sluice_semantic::kPrecedenceScenarios,
            sluice_semantic::kPrecedenceScenarioCount) != 0) {
        std::fprintf(stderr, "FAIL: the shared rules disagree with the frozen precedence table\n");
        return 1;
    }

    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"short_reads_report_eof_before_full_with_the_prefix",
         short_reads_report_eof_before_full_with_the_prefix},
        {"short_reads_reach_full_progress", short_reads_reach_full_progress},
        {"short_writes_reach_full_progress", short_writes_reach_full_progress},
        {"zero_progress_write_stops_after_one_attempt", zero_progress_write_stops_after_one_attempt},
        {"composition_advances_the_buffer_by_confirmed_bytes",
         composition_advances_the_buffer_by_confirmed_bytes},
        {"error_after_confirmed_prefix_keeps_the_prefix",
         error_after_confirmed_prefix_keeps_the_prefix},
        {"primitive_retries_eintr_without_a_completed_count",
         primitive_retries_eintr_without_a_completed_count},
        {"zero_length_requests_make_no_native_call", zero_length_requests_make_no_native_call},
        {"impossible_count_stops_immediately", impossible_count_stops_immediately},
        {"every_stop_reason_is_pinned_against_the_oracle_error_rule",
         every_stop_reason_is_pinned_against_the_oracle_error_rule},
        {"composition_surfaces_obey_the_precedence_table",
         composition_surfaces_obey_the_precedence_table},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu direct composition fault-injection tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
