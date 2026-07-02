// Tests for the measurement structs: they are pure data (default-initialized to
// zero), value-like, and zero-overhead when not attached. These tests pin the
// field set and default-zero contract that the stats-wiring slices rely on.
#include "harness.hpp"

#include <cppio/measurement.hpp>

#include <cstdint>

CPPIO_TEST_CASE(syscall_stats_default_zero_and_value_like) {
    cppio::SyscallStats s;
    CPPIO_CHECK(s.read_syscalls == 0);
    CPPIO_CHECK(s.read_syscall_bytes == 0);
    CPPIO_CHECK(s.read_syscall_errors == 0);
    CPPIO_CHECK(s.write_syscalls == 0);
    CPPIO_CHECK(s.write_syscall_bytes == 0);
    CPPIO_CHECK(s.write_syscall_errors == 0);
    // Copyable value type (callers hold them by value and pass SyscallStats*).
    cppio::SyscallStats copy = s;
    (void)copy;
}

CPPIO_TEST_CASE(buffer_stats_default_zero_and_full_field_set) {
    cppio::BufferStats b;
    CPPIO_CHECK(b.read_requests == 0);
    CPPIO_CHECK(b.read_request_bytes == 0);
    CPPIO_CHECK(b.read_buffer_hits == 0);
    CPPIO_CHECK(b.read_buffer_hit_bytes == 0);
    CPPIO_CHECK(b.read_buffer_misses == 0);
    CPPIO_CHECK(b.read_refill_calls == 0);
    CPPIO_CHECK(b.read_refill_bytes == 0);
    CPPIO_CHECK(b.write_requests == 0);
    CPPIO_CHECK(b.write_request_bytes == 0);
    CPPIO_CHECK(b.write_buffered_calls == 0);
    CPPIO_CHECK(b.write_buffered_bytes == 0);
    CPPIO_CHECK(b.write_flush_calls == 0);
    CPPIO_CHECK(b.write_flush_bytes == 0);
    CPPIO_CHECK(b.write_direct_calls == 0);
    CPPIO_CHECK(b.write_direct_bytes == 0);
}

CPPIO_TEST_CASE(copy_stats_default_zero_and_full_field_set) {
    cppio::CopyStats c;
    CPPIO_CHECK(c.copy_calls == 0);
    CPPIO_CHECK(c.copy_loop_iterations == 0);
    CPPIO_CHECK(c.bytes_read == 0);
    CPPIO_CHECK(c.bytes_written == 0);
    CPPIO_CHECK(c.eof_stops == 0);
    CPPIO_CHECK(c.limit_stops == 0);
    CPPIO_CHECK(c.reader_error_stops == 0);
    CPPIO_CHECK(c.writer_error_stops == 0);
}

CPPIO_MAIN()
