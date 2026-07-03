// Tests for the measurement structs: they are pure data (default-initialized to
// zero), value-like, and zero-overhead when not attached. These tests pin the
// field set and default-zero contract that the stats-wiring slices rely on.
#include "harness.hpp"

#include <sluice/measurement.hpp>

#include <cstdint>

SLUICE_TEST_CASE(syscall_stats_default_zero_and_value_like) {
    sluice::SyscallStats s;
    SLUICE_CHECK(s.read_syscalls == 0);
    SLUICE_CHECK(s.read_syscall_bytes == 0);
    SLUICE_CHECK(s.read_syscall_errors == 0);
    SLUICE_CHECK(s.write_syscalls == 0);
    SLUICE_CHECK(s.write_syscall_bytes == 0);
    SLUICE_CHECK(s.write_syscall_errors == 0);
    // Copyable value type (callers hold them by value and pass SyscallStats*).
    sluice::SyscallStats copy = s;
    (void)copy;
}

SLUICE_TEST_CASE(buffer_stats_default_zero_and_full_field_set) {
    sluice::BufferStats b;
    SLUICE_CHECK(b.read_requests == 0);
    SLUICE_CHECK(b.read_request_bytes == 0);
    SLUICE_CHECK(b.read_buffer_hits == 0);
    SLUICE_CHECK(b.read_buffer_hit_bytes == 0);
    SLUICE_CHECK(b.read_buffer_misses == 0);
    SLUICE_CHECK(b.read_refill_calls == 0);
    SLUICE_CHECK(b.read_refill_bytes == 0);
    SLUICE_CHECK(b.write_requests == 0);
    SLUICE_CHECK(b.write_request_bytes == 0);
    SLUICE_CHECK(b.write_buffered_calls == 0);
    SLUICE_CHECK(b.write_buffered_bytes == 0);
    SLUICE_CHECK(b.write_flush_calls == 0);
    SLUICE_CHECK(b.write_flush_bytes == 0);
    SLUICE_CHECK(b.write_direct_calls == 0);
    SLUICE_CHECK(b.write_direct_bytes == 0);
}

SLUICE_TEST_CASE(copy_stats_default_zero_and_full_field_set) {
    sluice::CopyStats c;
    SLUICE_CHECK(c.copy_calls == 0);
    SLUICE_CHECK(c.copy_loop_iterations == 0);
    SLUICE_CHECK(c.bytes_read == 0);
    SLUICE_CHECK(c.bytes_written == 0);
    SLUICE_CHECK(c.eof_stops == 0);
    SLUICE_CHECK(c.limit_stops == 0);
    SLUICE_CHECK(c.reader_error_stops == 0);
    SLUICE_CHECK(c.writer_error_stops == 0);
}

SLUICE_MAIN()
