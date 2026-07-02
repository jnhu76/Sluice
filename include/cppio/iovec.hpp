// cppio vector I/O slice types — the shape consumed by read_vec / write_vec.
//
// Mirrors the *shape* of POSIX struct iovec {void* iov_base; size_t iov_len} and
// of Zig std.Io's []const []const u8 vector args, but stays in C++ spans. These
// are non-owning views: callers keep the backing storage alive for the call.
//
// Rules (enforced by the read_vec/write_vec implementations, not here):
//   * Empty slices are allowed and skipped (never cause a syscall).
//   * Slices are processed strictly in order.
//   * No ownership is transferred.
#pragma once

#include <cstddef>
#include <span>

namespace cppio {

// Mutable destination slice for read_vec (bytes are written into .bytes).
struct IoSlice {
    std::span<std::byte> bytes;
};

// Immutable source slice for write_vec (bytes are read from .bytes).
struct ConstIoSlice {
    std::span<const std::byte> bytes;
};

}  // namespace cppio
