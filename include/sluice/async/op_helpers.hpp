// sluice::async derived "all" helpers (sluice-CORE-018, ADR §6 O5).
//
// Loop-until-complete over Completions, mirroring the blocking read_exact/
// write_all factoring. A primitive read/write Completion reports bytes (may be
// short); the "all" variants retry until all requested bytes transfer or an
// error occurs.
//
// Semantics (mirrors blocking, ADR §8 E4):
//   - short completion: resubmit the remaining slice (loop).
//   - EOF before full: IoError::eof (whether or not partial progress was made).
//   - zero progress on non-empty input: IoError::invalid_state.
//   - any error: propagated immediately.
//
// These are COORDINATORS: they own one Completion internally, drive the context
// (submit/poll), and reposition the slice across short completions. The offset
// advances by bytes transferred so a multi-step "all" stays positional (P1).
//
// IMPORTANT: these block the caller in a poll-loop until done. They are NOT the
// high-concurrency path — that is the raw submit_* + caller-driven reaping. The
// "all" helpers exist for parity with blocking read_exact/write_all and for
// tests/benches that want the simpler sync-shaped API on top of async.
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sluice::async {

// Read exactly dst.size() bytes from fd at `offset`, looping across short
// completions. Returns the bytes read on success (== dst.size()) or an error.
// EOF before/within -> IoError::eof (E4). The Completion is internal.
Result<std::size_t> read_all(AsyncIoContext& ctx, int fd,
                             std::span<std::byte> dst, std::uint64_t offset);

// Write exactly src.size() bytes to fd at `offset`, looping across short
// completions. Returns bytes written (== src.size()) or an error. Zero progress
// on non-empty input -> IoError::invalid_state.
Result<std::size_t> write_all(AsyncIoContext& ctx, int fd,
                              std::span<const std::byte> src, std::uint64_t offset);

}  // namespace sluice::async
