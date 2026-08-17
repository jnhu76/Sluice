// sluice-grep — bounded streaming literal search over the Sluice async
// runtime.
//
// One ApplicationRuntime + ONE task scan a list of files in CLI order: for
// each file, positional async reads into ONE reusable buffer feed the
// LineMatcher (matcher.hpp); matching lines are delivered to a caller sink
// synchronously, in file order + line order — deterministic output without
// buffering the whole result set (memory stays bounded by configuration).
//
// All code uses installed/public headers only. No SLUICE_ASYNC_INTERNAL_TESTING.
#pragma once

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/async_io_context.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sluice_grep {

// App-level resource limits (same philosophy as sluice-copy/sluice-hash).
// Memory upper bound: 1 x buffer_size + one line carry (<= max_line_bytes)
// + the per-match MatchEvent being delivered.
constexpr std::size_t kMinBufferSize = 4 * 1024;           // 4 KiB
constexpr std::size_t kMaxBufferSize = 64 * 1024 * 1024;   // 64 MiB
constexpr std::size_t kDefaultMaxLineBytes = 1 << 20;      // 1 MiB
constexpr std::size_t kMaxMaxLineBytes = 64 * 1024 * 1024; // 64 MiB
constexpr unsigned kMaxWorkers = 64;

// One input file: the path (for reporting/prefixing) and a read-only fd
// opened and validated (regular file) by the caller.
struct GrepInput {
    std::string path;
    int fd = -1;
};

// Match delivery sink. Called from the Runtime task thread, one file at a
// time, in CLI file order and ascending line order within a file. `line` is
// valid only for the call. May NOT call back into the engine.
using MatchSink =
    std::function<void(const std::string& path, std::uint64_t line_no,
                       std::string_view line)>;

// Per-file outcome. `error` empty <=> scan completed (match_count meaningful
// either way: matches found before an error are still counted and were
// already delivered).
struct GrepFileResult {
    std::string path;
    std::optional<sluice::IoError> error;
    std::uint64_t match_count = 0;
    std::uint64_t lines_scanned = 0;
    bool dropped_long_lines = false;
};

// Scan every input, in order, inside one Runtime (ThreadPoolBackend).
// A per-file error does NOT stop later files. `sink` receives matches as
// they are found (streaming, not buffered). config violations
// (buffer/line-cap/worker bounds, empty-config) produce per-file
// invalid_state without running anything.
std::vector<GrepFileResult> grep_files(
    const std::string& pattern, std::vector<GrepInput> inputs,
    std::size_t buffer_size, std::size_t max_line_bytes, unsigned workers,
    MatchSink sink);

// Backend-injecting variant for deterministic fault tests.
std::vector<GrepFileResult> grep_files_with_backend(
    const std::string& pattern, std::vector<GrepInput> inputs,
    std::size_t buffer_size, std::size_t max_line_bytes, unsigned workers,
    MatchSink sink, std::unique_ptr<sluice::async::AsyncBackend> backend);

}  // namespace sluice_grep
