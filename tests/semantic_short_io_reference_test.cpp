// SEM-05 primitive and composition reference rules, checked against real
// filesystem behaviour where the platform can express the case.
//
// The reference fold is the oracle: a path that composes exact/all operations
// must reach the same accumulation and the same stop reason. The real cases below
// pin the primitive half (EOF, short transfer, zero-length) against the kernel,
// so the reference model cannot drift away from what Linux actually returns.
#include <sluice/blocking/file.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/file_resource.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileInitialContents;
using sluice::FileOpen;
using sluice::IoError;
using sluice::detail::classify_primitive;
using sluice::detail::CompositionKind;
using sluice::detail::CompositionState;
using sluice::detail::CompositionStop;
using sluice::detail::compose_error;
using sluice::detail::compose_progress;
using sluice::detail::composition_error;
using sluice::detail::FileOperation;
using sluice::detail::PrimitiveOutcome;

// ── Primitive classification ───────────────────────────────────────────────

struct PrimitiveCase {
    const char* name;
    FileOperation operation;
    std::size_t requested;
    std::size_t transferred;
    PrimitiveOutcome expected;
};

const PrimitiveCase kPrimitiveCases[] = {
    {"empty_request_is_success_zero", FileOperation::read, 0, 0, PrimitiveOutcome::empty_request},
    {"empty_write_is_success_zero", FileOperation::write, 0, 0, PrimitiveOutcome::empty_request},
    {"read_zero_at_eof_is_read_eof", FileOperation::read, 4, 0, PrimitiveOutcome::eof},
    {"write_zero_is_zero_progress", FileOperation::write, 4, 0,
     PrimitiveOutcome::zero_write_progress},
    {"full_read", FileOperation::read, 4, 4, PrimitiveOutcome::full_progress},
    {"full_write", FileOperation::write, 4, 4, PrimitiveOutcome::full_progress},
    {"short_read", FileOperation::read, 4, 3, PrimitiveOutcome::short_progress},
    {"short_write", FileOperation::write, 4, 3, PrimitiveOutcome::short_progress},
};

bool primitive_table_holds() {
    for (const PrimitiveCase& c : kPrimitiveCases) {
        if (classify_primitive(c.operation, c.requested, c.transferred) != c.expected) {
            std::fprintf(stderr, "FAIL: primitive case %s\n", c.name);
            return false;
        }
    }
    return true;
}

// ── Composition fold ──────────────────────────────────────────────────────

bool read_exact_accumulates_a_short_prefix_then_reports_eof() {
    CompositionState state;
    state = compose_progress(CompositionKind::read_exact, 10, state, 4);
    if (!state.complete() || state.confirmed_bytes != 4)
        return false;
    state = compose_progress(CompositionKind::read_exact, 10, state, 3);
    if (!state.complete() || state.confirmed_bytes != 7)
        return false;
    state = compose_progress(CompositionKind::read_exact, 10, state, 0);
    if (state.complete())
        return false;
    if (state.stop != CompositionStop::eof_before_full)
        return false;
    // The confirmed prefix survives the stop.
    if (state.confirmed_bytes != 7)
        return false;
    const auto reason = composition_error(state);
    return reason.has_value() && reason->code == IoError::Code::eof;
}

bool write_all_zero_progress_stops_and_keeps_its_prefix() {
    CompositionState state;
    state = compose_progress(CompositionKind::write_all, 10, state, 6);
    state = compose_progress(CompositionKind::write_all, 10, state, 0);
    if (state.complete() || state.stop != CompositionStop::write_no_progress)
        return false;
    if (state.confirmed_bytes != 6)
        return false;
    // Distinguishable from EOF-before-full, which is the point of the split.
    const auto reason = composition_error(state);
    return reason.has_value() && reason->code != IoError::Code::eof;
}

bool composition_never_spins_on_zero_progress() {
    // The fold is a pure step function: it can only stop, so a caller cannot
    // build a loop that keeps transferring nothing.
    CompositionState state;
    state = compose_progress(CompositionKind::write_all, 1, state, 0);
    if (!state.stopped)
        return false;
    const CompositionState again = compose_progress(CompositionKind::write_all, 1, state, 0);
    return again == state;
}

bool composition_completes_on_full_progress() {
    CompositionState state;
    state = compose_progress(CompositionKind::read_exact, 4, state, 2);
    state = compose_progress(CompositionKind::read_exact, 4, state, 2);
    if (!state.complete() || state.confirmed_bytes != 4)
        return false;
    if (state.stop != CompositionStop::complete)
        return false;
    return !composition_error(state).has_value();
}

bool composition_rejects_an_impossible_count() {
    CompositionState state;
    state = compose_progress(CompositionKind::read_exact, 4, state, 5);
    if (state.complete() || state.stop != CompositionStop::impossible_count)
        return false;
    return state.confirmed_bytes == 0;
}

bool composition_keeps_the_primitive_error() {
    CompositionState state;
    state = compose_progress(CompositionKind::write_all, 8, state, 3);
    state = compose_error(state, IoError{.code = IoError::Code::no_space, .os_errno = 28});
    if (state.complete() || state.stop != CompositionStop::primitive_error)
        return false;
    if (state.confirmed_bytes != 3)
        return false;
    const auto reason = composition_error(state);
    return reason.has_value() && reason->code == IoError::Code::no_space && reason->os_errno == 28;
}

// ── Real filesystem primitive evidence ────────────────────────────────────

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_short_io_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    if (!content.empty() &&
        ::write(fd, content.data(), content.size()) != static_cast<ssize_t>(content.size())) {
        ::close(fd);
        return {};
    }
    ::close(fd);
    return path;
}

FileOpen readable_mode() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    return mode;
}

// A real short read: the file is smaller than the request, so the primitive
// returns 0 < n < requested and must be classified as allowed progress, not as
// an error.
bool real_short_read_is_short_progress() {
    const std::string path = make_temp_file("0123456789");
    if (path.empty())
        return false;
    auto opened = File::open(path, readable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened.value());
    ::unlink(path.c_str());

    std::vector<std::byte> dst(20, std::byte{0});
    auto result = sluice::blocking::read_at(file, 0, std::span<std::byte>(dst));
    if (!result.has_value())
        return false;
    const std::size_t got = result.value();
    if (classify_primitive(FileOperation::read, 20, got) != PrimitiveOutcome::short_progress)
        return false;

    // Reading at the end of the file observes EOF as a primitive success.
    auto at_end = sluice::blocking::read_at(file, 10, std::span<std::byte>(dst));
    if (!at_end.has_value() || at_end.value() != 0)
        return false;
    if (classify_primitive(FileOperation::read, 20, at_end.value()) != PrimitiveOutcome::eof)
        return false;

    // An empty request is success 0 and observes no EOF.
    auto empty = sluice::blocking::read_at(file, 0, std::span<std::byte>());
    if (!empty.has_value() || empty.value() != 0)
        return false;
    return classify_primitive(FileOperation::read, 0, empty.value()) ==
           PrimitiveOutcome::empty_request;
}

// A full write followed by reading the bytes back is the reference composition
// result: accumulated confirmed bytes equal the requested length.
bool real_write_all_reaches_full_progress() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    auto opened = File::open(path, readable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened.value());
    ::unlink(path.c_str());

    const std::string payload = "reference bytes";
    const auto src = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());

    CompositionState state;
    std::size_t offset = 0;
    while (offset < src.size()) {
        auto written = sluice::blocking::write_at(file, offset, src.subspan(offset));
        if (!written.has_value())
            return false;
        state = compose_progress(CompositionKind::write_all, src.size(), state, written.value());
        if (state.stopped)
            return false;
        offset = state.confirmed_bytes;
    }
    if (!state.complete() || state.confirmed_bytes != src.size())
        return false;

    std::vector<std::byte> readback(src.size(), std::byte{0});
    auto read = sluice::blocking::read_at(file, 0, std::span<std::byte>(readback));
    if (!read.has_value() || read.value() != src.size())
        return false;
    return std::memcmp(readback.data(), src.data(), src.size()) == 0;
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"primitive_table_holds", primitive_table_holds},
        {"read_exact_accumulates_a_short_prefix_then_reports_eof",
         read_exact_accumulates_a_short_prefix_then_reports_eof},
        {"write_all_zero_progress_stops_and_keeps_its_prefix",
         write_all_zero_progress_stops_and_keeps_its_prefix},
        {"composition_never_spins_on_zero_progress", composition_never_spins_on_zero_progress},
        {"composition_completes_on_full_progress", composition_completes_on_full_progress},
        {"composition_rejects_an_impossible_count", composition_rejects_an_impossible_count},
        {"composition_keeps_the_primitive_error", composition_keeps_the_primitive_error},
        {"real_short_read_is_short_progress", real_short_read_is_short_progress},
        {"real_write_all_reaches_full_progress", real_write_all_reaches_full_progress},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu short-I/O reference tests passed (%zu primitive cases)\n",
                sizeof(tests) / sizeof(tests[0]), sizeof(kPrimitiveCases) / sizeof(kPrimitiveCases[0]));
    return 0;
}
