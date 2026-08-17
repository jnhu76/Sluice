// sluice-tail — bounded last-N scan + follow-mode tailing over the Sluice
// async runtime.
//
// Unlike sluice-copy/hash/grep (finite run-to-completion tasks wrapped in a
// single blocking call), tail -f is a LONG-LIVED wait/event/cancel workload.
// The engine therefore exposes an explicit lifecycle:
//
//   TailEngine engine(fd, options, sink);
//   engine.start();            // builds + starts the Runtime, submits the task
//   engine.request_stop();     // any thread; cooperative cancel (follow end)
//   engine.wait();             // blocks until the task is terminal, then
//                               // drains + joins and returns the result
//
// Last-N: a BACKWARD scan (positional reads at descending offsets) counts
// newlines and finds the start offset of the Nth-from-last line WITHOUT
// buffering the file; a forward pass from that offset streams the tail lines
// through the same bounded line-assembly policy as sluice-grep.
//
// Follow (-f): read forward from the post-scan EOF; at EOF sleep for the
// bounded poll interval (sliced so cancellation is observed quickly) and
// re-stat; growth is read and printed, truncation resets to offset 0 with a
// diagnostic through the optional diag sink. NO busy spin: exactly one
// (re-)stat per interval while idle.
//
// Follow-descriptor semantics (documented): the fd is followed, not the path
// — rename/replacement rotation keeps the ORIGINAL inode; -F reopen-by-name
// is a later feature.
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

namespace sluice_tail {

// App-level resource limits (same philosophy as the other apps).
constexpr std::size_t kMinBufferSize = 4 * 1024;           // 4 KiB
constexpr std::size_t kMaxBufferSize = 64 * 1024 * 1024;   // 64 MiB
constexpr std::size_t kDefaultMaxLineBytes = 1 << 20;      // 1 MiB
constexpr std::size_t kMaxMaxLineBytes = 64 * 1024 * 1024; // 64 MiB
constexpr std::size_t kMaxLines = 1000 * 1000 * 1000;      // -n sanity cap
constexpr unsigned kMaxWorkers = 64;
constexpr unsigned kMinPollMs = 50;
constexpr unsigned kMaxPollMs = 5000;

struct TailOptions {
    std::size_t lines = 10;                 // -n N (0 = no initial tail)
    bool follow = false;                    // -f
    unsigned poll_interval_ms = 200;        // follow poll cadence
    std::size_t buffer_size = 64 * 1024;    // scan/read window
    std::size_t max_line_bytes = kDefaultMaxLineBytes;
    unsigned workers = 1;
};

// Data delivery: each emitted line WITHOUT its '\n' (the final line may be
// unterminated). Called from the Runtime task thread, in order.
using LineSink = std::function<void(std::string_view line)>;

// Optional diagnostics (truncation notices, skipped long lines) — stderr in
// the CLI, captured in tests.
using DiagSink = std::function<void(std::string_view msg)>;

struct TailResult {
    std::optional<sluice::IoError> error;  // empty <=> clean end (incl. cancel)
    bool stopped_by_cancel = false;        // follow ended via request_stop
    std::uint64_t lines_emitted = 0;       // initial tail + follow lines
    bool truncation_detected = false;
    bool dropped_long_lines = false;
};

class TailEngine {
public:
    // `fd` is opened read-only by the caller and must remain valid until
    // wait() returns. The engine never closes it.
    TailEngine(int fd, TailOptions options, LineSink sink,
               DiagSink diag = nullptr);
    ~TailEngine();

    TailEngine(const TailEngine&) = delete;
    TailEngine& operator=(const TailEngine&) = delete;

    // Validate options, build + start the Runtime, submit the tail task.
    // Returns invalid_state on bad options (nothing started) or Runtime
    // build/start failure.
    sluice::Result<void> start();

    // Request cooperative stop (idempotent, thread-safe, noexcept). The
    // follow loop observes it within one poll slice; the in-flight read (if
    // any) completes first per the Completion lifetime contract.
    void request_stop() noexcept;

    // Block until the task publishes its terminal outcome, then request_stop
    // + drain + join the Runtime and return the result. Exactly-once.
    sluice::Result<TailResult> wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sluice_tail
