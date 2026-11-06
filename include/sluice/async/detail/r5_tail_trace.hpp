#pragma once

#include <cstddef>
#include <cstdint>

#include "sluice/async/detail/request_key.hpp"

// #595 R5 research diagnostic (v2): per-op pipeline timestamps for the W03
// tail-decomposition protocol. Compiled in only with
// SLUICE_595_R5_TAIL_TRACE and activated per process by
// SLUICE_595_R5_TAIL_TRACE=1; every hook is a no-op otherwise. Research
// branch only; not part of any public or library contract.
//
// v2 concurrency contract (fixes the v1 races audited as F1):
// - Each thread that ever calls a hook owns one preallocated event buffer;
//   the buffer cursor is plain and single-writer. No online cross-thread
//   index exists: (slot, generation) joins are made offline by the analyzer.
// - A hook entry marks this writer active-in-hook, then re-checks the
//   global phase flag under seq_cst; end_phase/begin_phase clear the flag
//   under seq_cst and then wait (bounded) until every registered writer has
//   left its hook before resetting or exporting buffers, so exported event
//   ranges are complete per writer.
// - Generation is carried at full 64 bits; run identity is pid plus a
//   per-process nonce in the dump header.

namespace sluice::async::detail::tailtrace {

#if defined(SLUICE_595_R5_TAIL_TRACE)

bool enabled() noexcept;
void begin_phase() noexcept;
void end_phase(const char* tag) noexcept;
std::int64_t now_ns_i() noexcept;
std::uint32_t next_presentation() noexcept;

void note_built(std::uint32_t seq, std::uint32_t caller_window, std::int64_t t0_ns) noexcept;
void note_admitted(std::uint32_t slot, std::uint64_t generation,
                   std::uint32_t presentation) noexcept;
void note_enter_batch(const RequestKey* keys, std::size_t count) noexcept;
void note_enter_single(std::uint32_t slot, std::uint64_t generation) noexcept;
void note_cookie(std::uint32_t slot, std::uint64_t generation, std::uint64_t cookie) noexcept;
void note_cqe(std::uint32_t slot, std::uint64_t generation) noexcept;
void note_published(std::uint32_t slot, std::uint64_t generation) noexcept;
void note_taken(std::uint32_t seq, std::int64_t t5_ns) noexcept;
void note_stop_kind(const RequestKey* keys, std::size_t count, std::uint32_t stop_kind) noexcept;
void note_wake() noexcept;

#else

inline constexpr bool enabled() noexcept { return false; }
inline void begin_phase() noexcept {}
inline void end_phase(const char*) noexcept {}
inline constexpr std::int64_t now_ns_i() noexcept { return 0; }
inline constexpr std::uint32_t next_presentation() noexcept { return 0; }

inline void note_built(std::uint32_t, std::uint32_t, std::int64_t) noexcept {}
inline void note_admitted(std::uint32_t, std::uint64_t, std::uint32_t) noexcept {}
inline void note_enter_batch(const RequestKey*, std::size_t) noexcept {}
inline void note_enter_single(std::uint32_t, std::uint64_t) noexcept {}
inline void note_cookie(std::uint32_t, std::uint64_t, std::uint64_t) noexcept {}
inline void note_cqe(std::uint32_t, std::uint64_t) noexcept {}
inline void note_published(std::uint32_t, std::uint64_t) noexcept {}
inline void note_taken(std::uint32_t, std::int64_t) noexcept {}
inline void note_stop_kind(const RequestKey*, std::size_t, std::uint32_t) noexcept {}
inline void note_wake() noexcept {}

#endif

} // namespace sluice::async::detail::tailtrace
