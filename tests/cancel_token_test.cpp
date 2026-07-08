// Tests for cooperative cancellation primitives (sluice-CORE-027, T1).
//
// Derived from Zig std.Io's cancellation model (Io.zig:1183-1188, 1310-1358).
// Each case asserts ONE cooperative-cancel semantic, TDD-vertical: RED on the
// first behavior, GREEN, then add the next.
//
// These tests are pure-logic: no kernel, no threads (the atomic is exercised
// single-threaded; cross-thread happens-before is documented in cancel.hpp and
// would be verified by TSan in the future task-runtime tests).
#include "harness.hpp"

#include <sluice/async/cancel.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

// ---- Slice 1 (tracer): request -> check_cancel delivers canceled ----------
// The core single-shot contract (ADR §7 X3, Zig Io.zig:1183-1188). A requested
// token, observed by an unblocked + unacknowledged consumer, delivers canceled
// exactly once.
SLUICE_TEST_CASE(cancel_request_then_check_delivers_canceled) {
    CancelToken t;
    CancelState s;
    SLUICE_CHECK(!t.is_requested());
    SLUICE_CHECK(check_cancel(t, s).has_value());  // no request -> ok

    t.request();
    SLUICE_CHECK(t.is_requested());
    auto r1 = check_cancel(t, s);
    SLUICE_CHECK(!r1.has_value());
    SLUICE_CHECK(r1.error().code == IoError::Code::canceled);
}

// ---- Slice 2: single-shot — second check does NOT re-signal ---------------
// After the first cancel point delivers, subsequent cancel points see the
// acknowledged state and do NOT re-deliver (Zig Io.zig:1186). Only a rearm()
// re-arms. This is what lets a consumer report Canceled once, do partial
// cleanup, then continue without re-triggering at every Io call.
SLUICE_TEST_CASE(cancel_single_shot_second_check_does_not_resignal) {
    CancelToken t;
    CancelState s;
    t.request();

    auto r1 = check_cancel(t, s);
    SLUICE_CHECK(!r1.has_value());
    SLUICE_CHECK(r1.error().code == IoError::Code::canceled);

    // Second point: same token still requested, but the consumer acknowledged.
    // Must NOT re-signal.
    auto r2 = check_cancel(t, s);
    SLUICE_CHECK(r2.has_value());  // ok, no cancel delivered
}

// ---- Slice 3: rearm re-enables single-shot delivery ----------------------
// Zig Io.recancel (Io.zig:1310): re-arm a previously-acknowledged cancel so
// the NEXT cancel point delivers again. The canonical use is "report partial
// progress, then re-propagate the cancel" (Zig Queue pattern, Io.zig:2029).
SLUICE_TEST_CASE(cancel_rearm_re_enables_delivery) {
    CancelToken t;
    CancelState s;
    t.request();

    (void)check_cancel(t, s);  // delivers, acknowledges
    SLUICE_CHECK(check_cancel(t, s).has_value());  // no re-signal

    t.rearm();
    // The token is still requested; rearm re-set the requested bit. But the
    // consumer must ALSO reset its acknowledgement to observe delivery again —
    // rearm is a token-side operation; the consumer acknowledges per-delivery.
    // The check_cancel contract: deliver iff (requested AND !acknowledged AND
    // unblocked). So reset the consumer's acknowledgement.
    s.reset_acknowledgement();
    auto r = check_cancel(t, s);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::canceled);
}

// ---- Slice 4: CancelProtection blocks delivery (not the request) ----------
// A protected region suppresses cancel DELIVERY. The request stays pending on
// the token; after the region ends, the next cancel point delivers normally
// (Zig Io.zig:1322-1344). CancelGuard is the RAII wrapper.
SLUICE_TEST_CASE(cancel_protection_blocks_delivery_not_request) {
    CancelToken t;
    CancelState s;
    t.request();
    SLUICE_CHECK(t.is_requested());  // request recorded

    {
        CancelGuard g{s, CancelProtection::blocked};
        // Inside the protected region: no cancel point delivers.
        SLUICE_CHECK(check_cancel(t, s).has_value());
        SLUICE_CHECK(check_cancel(t, s).has_value());
        SLUICE_CHECK(t.is_requested());  // request still pending, not consumed
        SLUICE_CHECK(!s.acknowledged()); // nothing acknowledged
    }  // guard restores unblocked

    // After the region: the next cancel point delivers (request was pending).
    auto r = check_cancel(t, s);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::canceled);
    SLUICE_CHECK(s.acknowledged());
}

// ---- Slice 5: request is idempotent; clear resets -------------------------
// request() may be called any number of times; clear() resets the token for
// reuse (Zig: Future.cancel is idempotent, Io.zig:1190).
SLUICE_TEST_CASE(cancel_request_idempotent_clear_resets) {
    CancelToken t;
    t.request();
    t.request();
    t.request();
    SLUICE_CHECK(t.is_requested());
    t.clear();
    SLUICE_CHECK(!t.is_requested());

    // After clear, a fresh consumer sees no request.
    CancelState s;
    SLUICE_CHECK(check_cancel(t, s).has_value());
}

SLUICE_MAIN()
