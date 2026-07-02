// cppio measurement structs — optional observability hooks for the core.
//
// Each stats struct is pure data: default-initialized to zero, copyable, and
// attached to a core type via a nullable pointer (Stats*). A null pointer means
// "no measurement" and costs nothing — the wired-in call sites guard on null
// before incrementing. These do NOT change I/O semantics; they only count.
//
// Design rule (CPPIO-CORE-004): stats are caller-owned and never global. The
// core holds raw pointers; callers keep the storage alive for the reader/
// writer/copy operation's lifetime.
#pragma once

#include <cstdint>

namespace cppio {

// Counts syscalls made by the POSIX file backend (FileReader/FileWriter).
struct SyscallStats {
    std::uint64_t read_syscalls = 0;
    std::uint64_t read_syscall_bytes = 0;
    std::uint64_t read_syscall_errors = 0;
    std::uint64_t write_syscalls = 0;
    std::uint64_t write_syscall_bytes = 0;
    std::uint64_t write_syscall_errors = 0;
};

// Counts buffer hit/miss/refill activity in BufferedReader/BufferedWriter.
struct BufferStats {
    std::uint64_t read_requests = 0;
    std::uint64_t read_request_bytes = 0;
    std::uint64_t read_buffer_hits = 0;
    std::uint64_t read_buffer_hit_bytes = 0;
    std::uint64_t read_buffer_misses = 0;
    std::uint64_t read_refill_calls = 0;
    std::uint64_t read_refill_bytes = 0;

    std::uint64_t write_requests = 0;
    std::uint64_t write_request_bytes = 0;
    std::uint64_t write_buffered_calls = 0;
    std::uint64_t write_buffered_bytes = 0;
    std::uint64_t write_flush_calls = 0;
    std::uint64_t write_flush_bytes = 0;
    std::uint64_t write_direct_calls = 0;
    std::uint64_t write_direct_bytes = 0;
};

// Counts copy_all loop behavior and the reason each copy stopped. The
// fast/scratch counters (CPPIO-CORE-006E) split the copy work between the
// buffered fast path (draining already-buffered bytes via BufferedReadable) and
// the scratch read path. They are observability hooks, not throughput numbers.
struct CopyStats {
    std::uint64_t copy_calls = 0;
    std::uint64_t copy_loop_iterations = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t eof_stops = 0;
    std::uint64_t limit_stops = 0;
    std::uint64_t reader_error_stops = 0;
    std::uint64_t writer_error_stops = 0;
    // Buffered fast path: incremented each loop iteration that drains bytes from
    // peek_buffered(); *_bytes is the byte count drained.
    std::uint64_t buffered_fast_path_calls = 0;
    std::uint64_t buffered_fast_path_bytes = 0;
    // Scratch path: incremented each loop iteration that issues a read into the
    // scratch buffer (including an EOF probe that reads 0); *_bytes is the byte
    // count that was actually written through scratch.
    std::uint64_t scratch_path_calls = 0;
    std::uint64_t scratch_path_bytes = 0;
    // Strategy selection counters (CPPIO-CORE-007F). Exactly one strategy counter
    // is incremented per top-level copy_all call, recording the SELECTED strategy
    // (after Auto resolution and deferred-fallback). These answer "which
    // strategy was selected?", distinct from the path counters above which answer
    // "which path moved bytes?".
    std::uint64_t strategy_auto_calls = 0;
    std::uint64_t strategy_scratch_calls = 0;
    std::uint64_t strategy_buffered_first_calls = 0;
    // A deferred strategy that was rejected (default policy) vs fell back to Auto.
    std::uint64_t strategy_deferred_rejected_calls = 0;
    std::uint64_t strategy_deferred_fallback_calls = 0;
};

// Counts read_vec/write_vec activity. The *_fallback_calls fields distinguish
// "used the default read_some/write_some loop" (e.g. an ObservedReader around a
// MemoryReader, or any non-overriding reader) from "used a real vector syscall"
// (the FileReader/FileWriter readv/writev overrides). That split is the whole
// point of these stats: it tells the decision matrix (CPPIO-CORE-011) how often
// vector I/O actually reached the kernel as a single gather/scatter syscall vs.
// degenerated to the per-slice fallback. See docs/readv-writev-design-note.md.
struct VectorStats {
    std::uint64_t read_vec_calls = 0;
    std::uint64_t read_vec_bytes = 0;
    std::uint64_t read_vec_iovecs = 0;
    std::uint64_t read_vec_fallback_calls = 0;
    std::uint64_t write_vec_calls = 0;
    std::uint64_t write_vec_bytes = 0;
    std::uint64_t write_vec_iovecs = 0;
    std::uint64_t write_vec_fallback_calls = 0;
};

}  // namespace cppio
