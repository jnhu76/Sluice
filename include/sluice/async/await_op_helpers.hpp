// sluice::async await-style operation helpers for Runtime tasks.
//
// The four applications (sluice-copy / sluice-hash / sluice-grep / sluice-tail)
// each hand-rolled the same submit -> await_completion -> result() -> reset()
// protocol plus the short-read / partial-write retry loops on top of
// RuntimeTaskContext (the #135 audit finding: "the library is a shallow module
// for its own examples"). These coordinators move that I/O PROTOCOL back into the
// library; the application keeps only its algorithm.
//
// They are the await-shaped siblings of the polling op_helpers.hpp
// (read_all/write_all drive an AsyncIoContext in a poll-loop and CANNOT be
// used from a Runtime task, which must suspend through
// RuntimeTaskContext::await_completion instead of blocking a worker).
//
// Semantics follow the audited in-repository application pattern:
//   - short read: returned as-is by await_read_once; retried positionally by
//     await_read_fill / await_write_exact.
//   - EOF: n == 0 from await_read_once; await_read_fill stops at EOF and
//     returns the bytes filled so far (a partial tail is SUCCESSFUL data —
//     deliberately different from polling read_all's eof-error parity).
//   - zero progress on a non-empty write: IoError::backend_error (an invalid
//     backend state, not an infinite retry). DELIBERATE DIVERGENCE from the
//     canonical polling write_all (src/async/op_helpers.cpp), which returns
//     invalid_state for the same condition: backend_error is what the audited
//     applications reported on this path, and these helpers exist to preserve
//     their observable behavior (changing the code would alter app-level
//     error handling for no architectural gain — both codes unambiguously
//     mean "invalid backend state, do not retry"); the note is bilateral
//     (see op_helpers.cpp).
//   - cancellation: these helpers do NOT observe the cancel token between
//     iterations. Each submitted operation is driven to its terminal; the
//     task's cooperative cancellation checks stay at ITS loop boundaries
//     (same layering as the applications before this change).
//
// Ownership contract (unchanged from the raw protocol):
//   - the caller owns the Completion and keeps it address-stable for the
//     duration of one helper call (L7);
//   - on every path where the helper returns a terminal result (success or
//     I/O error), the Completion has been reset and is idle for reuse;
//   - if await_completion itself rejects (invalid_state / canceled), the
//     Completion is left outstanding untouched — the I/O still owns its
//     borrow and must still be reaped before close (wait-cancel != I/O
//     cancel);
//   - a submit rejection returns with the Completion idle (it never entered
//     the backend).
#pragma once

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sluice::async {

// Completion-consumption tally filled by the retry coordinators (optional
// out-parameter). Lets an application keep its per-op statistics (ops
// executed, completions that came back short) without re-implementing the
// loops. One tally entry per terminal completion consumed inside the helper.
struct AwaitOpTally {
    std::uint64_t ops = 0;        // terminal completions consumed
    std::uint64_t short_ops = 0;  // completions that returned < requested
};

// Await an outstanding Completion to terminal, extract its result, and reset
// it for reuse. Returns the extracted byte result; the Completion is idle on
// every returned-terminal path. See the ownership contract above for the
// await-rejection path.
Result<std::size_t> await_take(RuntimeTaskContext& ctx,
                               Completion<std::size_t>& c);
Result<void> await_take(RuntimeTaskContext& ctx, Completion<void>& c);

// Await-drain an outstanding Completion<std::size_t>: await it to terminal,
// CONSUME and discard the result (whatever it is), and reset it. Returns the
// await failure only; a terminal I/O error is deliberately swallowed (this is
// the error-path cleanup protocol: the primary error was already captured and
// secondary outcomes are discarded). Used to drain already-submitted ops so a
// Runtime can close with zero outstanding Completions.
Result<void> await_drain(RuntimeTaskContext& ctx, Completion<std::size_t>& c);

// One-shot positional read: submit, await, extract, reset. Returns the
// completion's byte count (short reads returned as-is; 0 = EOF) or an error.
Result<std::size_t> await_read_once(RuntimeTaskContext& ctx, int fd,
                                    std::span<std::byte> dst,
                                    std::uint64_t offset,
                                    Completion<std::size_t>& c);

// Fill dst exactly, retrying short reads positionally, stopping at EOF.
// Returns the number of bytes filled: dst.size() when the buffer filled, or
// fewer when EOF was hit (0 = EOF before any byte). Never reports an error
// for a short tail.
Result<std::size_t> await_read_fill(RuntimeTaskContext& ctx, int fd,
                                    std::span<std::byte> dst,
                                    std::uint64_t offset,
                                    Completion<std::size_t>& c,
                                    AwaitOpTally* tally = nullptr);

// Write exactly src.size() bytes, retrying partial writes positionally.
// Returns src.size() on success. Zero progress while data remains is
// IoError::backend_error (invalid backend state, not an infinite retry).
Result<std::size_t> await_write_exact(RuntimeTaskContext& ctx, int fd,
                                      std::span<const std::byte> src,
                                      std::uint64_t offset,
                                      Completion<std::size_t>& c,
                                      AwaitOpTally* tally = nullptr);

}  // namespace sluice::async
