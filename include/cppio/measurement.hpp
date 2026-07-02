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

// Counts copy_all loop behavior and the reason each copy stopped.
struct CopyStats {
    std::uint64_t copy_calls = 0;
    std::uint64_t copy_loop_iterations = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t eof_stops = 0;
    std::uint64_t limit_stops = 0;
    std::uint64_t reader_error_stops = 0;
    std::uint64_t writer_error_stops = 0;
};

}  // namespace cppio
