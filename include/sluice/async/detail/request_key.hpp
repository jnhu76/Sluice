// sluice::async::detail — request identity types (Phase B).
//
// ADR-explicit-io-request-contract (Accepted) Decision 1 / AC-2 / AC-14:
// every accepted I/O operation has a stable logical key composed of context
// provenance, a reusable-slot index, and a monotonically increasing per-slot
// generation. A raw Completion* MUST NOT be the sole logical identity across
// asynchronous phases (AC-14).
//
//   ContextIdentity — opaque provenance token. Constructed by the context/backend
//                     pair; ordinary application code cannot forge one that an
//                     arena will accept (the only public constructor is the
//                     for_testing() seam; production construction is an internal
//                     detail owned by the arena/context).
//   SlotIndex       — index into the bounded RequestSlot arena.
//   Generation      — per-slot ABA guard; incremented on slot release BEFORE the
//                     next key can become visible (I6). 64-bit so a stale key
//                     can NEVER collide with the current generation (I6 absolute
//                     wording): ~584 years at one release per nanosecond. The
//                     arena fail-fasts at UINT64_MAX rather than silently wrap,
//                     so the ABA property holds in perpetuity (review finding:
//                     32-bit wrap re-introduces ABA under heavy reuse).
//
// RequestKey is a trivial value type: copyable, comparable, suitable for use as
// a stable value identity (e.g. carried by-value in a ReadyEvent). It MUST NOT
// be treated as forgeable authority by itself — cancel/reap/register always
// re-validate against the slot's current generation under the leaf slot-lifecycle
// domain.
#pragma once

#include <cstdint>

namespace sluice::async::detail {

// Opaque context-provenance token. The internal representation is a 64-bit
// value; production construction is internal to RequestArena/AsyncIoContext so
// that an ordinary caller cannot synthesize a context identity the arena would
// trust. The for_testing() factory is the only public constructor and exists
// for the arena unit tests; it is never linked into a production path that
// would accept a caller-forged identity as backend authority.
struct ContextIdentity {
    std::uint64_t value;

    static ContextIdentity for_testing(std::uint64_t v) noexcept { return {v}; }

    friend bool operator==(const ContextIdentity&, const ContextIdentity&) noexcept = default;
};

struct SlotIndex {
    std::uint32_t value = 0;
    friend bool operator==(const SlotIndex&, const SlotIndex&) noexcept = default;
};

struct Generation {
    std::uint64_t value = 0;
    friend bool operator==(const Generation&, const Generation&) noexcept = default;
};

struct RequestKey {
    ContextIdentity context;
    SlotIndex slot;
    Generation generation;

    friend bool operator==(const RequestKey&, const RequestKey&) noexcept = default;
};

}  // namespace sluice::async::detail
