// Tests for cooperative cancellation primitives (sluice-CORE-027, T1).
//
// Derived from Zig std.Io's cancellation model (Io.zig:1183-1188, 1310-1358).
// Each case asserts ONE cooperative-cancel semantic, TDD-vertical: RED on the
// first behavior, GREEN, then add the next.
//
// These tests are pure-logic: no kernel, no threads (the atomic is exercised
// single-threaded; cross-thread happens-before is documented in cancel.hpp and
// would be verified by TSan in the future task-runtime tests).
//
// Post-audit corrective pass (2026-08-13, ADR-cancel-request-epoch): the
// request is identified by a token-side EPOCH; per-consumer acknowledgement
// records the last delivered epoch. rearm() alone re-enables delivery for the
// next cancel point (the pre-fix implementation required the consumer to also
// call reset_acknowledgement() manually — those regressions are asserted below
// and fail on the pre-fix code).
#include "harness.hpp"

#include <sluice/async/cancel.hpp>
#include <sluice/async/future.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <thread>

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

// ---- Slice 3 (T-CANCEL-REARM-1): rearm re-enables single-shot delivery -----
// Zig Io.recancel (Io.zig:1310): re-arm a previously-acknowledged cancel so
// the NEXT cancel point delivers again. The canonical use is "report partial
// progress, then re-propagate the cancel" (Zig Queue pattern, Io.zig:2029).
//
// REGRESSION (ADR-cancel-request-epoch): rearm() ALONE re-enables delivery —
// the consumer must NOT need a second, manual acknowledgement reset. The
// pre-fix implementation re-stored the token's requested bit, which was
// already set, and left the consumer's acknowledgement untouched, so the
// cancel point after rearm() never delivered.
SLUICE_TEST_CASE(cancel_rearm_re_enables_delivery) {
    CancelToken t;
    CancelState s;
    t.request();

    (void)check_cancel(t, s);  // delivers, acknowledges
    SLUICE_CHECK(check_cancel(t, s).has_value());  // no re-signal

    t.rearm();
    auto r = check_cancel(t, s);
    SLUICE_CHECK(!r.has_value());  // rearm alone re-enabled delivery
    SLUICE_CHECK(r.error().code == IoError::Code::canceled);
    SLUICE_CHECK(check_cancel(t, s).has_value());  // single-shot again
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
        SLUICE_CHECK(!s.acknowledged(t));  // nothing acknowledged
    }  // guard restores unblocked

    // After the region: the next cancel point delivers (request was pending).
    auto r = check_cancel(t, s);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::canceled);
    SLUICE_CHECK(s.acknowledged(t));  // the request was delivered
}

// ---- Slice 5 (T-CANCEL-PROTECTION-2): protection + rearm compose -----------
// Protection blocks delivery of a re-armed request exactly as it blocks the
// first delivery: a protected point observes nothing; the first unprotected
// point after rearm delivers.
SLUICE_TEST_CASE(cancel_protection_rearm_blocks_until_unprotected_point) {
    CancelToken t;
    CancelState s;
    t.request();

    {
        CancelGuard g{s, CancelProtection::blocked};
        SLUICE_CHECK(check_cancel(t, s).has_value());  // protected: no delivery
    }
    auto r1 = check_cancel(t, s);
    SLUICE_CHECK(!r1.has_value());  // unprotected point delivers

    t.rearm();
    {
        CancelGuard g{s, CancelProtection::blocked};
        // A protected point after rearm must NOT consume or deliver the
        // re-armed request — delivery is blocked, the request stays armed.
        SLUICE_CHECK(check_cancel(t, s).has_value());
    }
    auto r2 = check_cancel(t, s);
    SLUICE_CHECK(!r2.has_value());  // rearm survived the protected region
    SLUICE_CHECK(r2.error().code == IoError::Code::canceled);
    SLUICE_CHECK(check_cancel(t, s).has_value());  // single-shot again
}

// ---- Slice 6 (T-CANCEL-CLEAR-3): clear removes; clear + request is a fresh
// ---- request ---------------------------------------------------------------
// clear() removes the pending request: no cancel point delivers until the next
// request(). A later request() is a NEW request: a consumer that delivered the
// previous request delivers once more (token reuse for real). The pre-fix
// implementation kept the consumer's acknowledgement forever, so a reused
// token could never deliver again to a previously-acked consumer.
SLUICE_TEST_CASE(cancel_clear_then_request_is_a_fresh_request) {
    CancelToken t;
    CancelState s;
    t.request();
    auto r1 = check_cancel(t, s);
    SLUICE_CHECK(!r1.has_value());  // delivers
    SLUICE_CHECK(check_cancel(t, s).has_value());  // single-shot

    t.clear();
    SLUICE_CHECK(!t.is_requested());
    SLUICE_CHECK(check_cancel(t, s).has_value());  // cleared: nothing pending

    // Reuse: the same consumer delivers the NEW request exactly once.
    t.request();
    auto r2 = check_cancel(t, s);
    SLUICE_CHECK(!r2.has_value());
    SLUICE_CHECK(r2.error().code == IoError::Code::canceled);
    SLUICE_CHECK(check_cancel(t, s).has_value());  // single-shot again

    // clear() with nothing pending is a no-op.
    t.clear();
    t.clear();
    SLUICE_CHECK(!t.is_requested());
}

// ---- Slice 7 (T-CANCEL-SHARED-4): one token, two consumers -----------------
// A shareable CancelToken delivers ONCE PER CONSUMER per request (Group
// semantics: each task delivers its own canceled). rearm() re-enables BOTH
// consumers for the same request; reset_acknowledgement() re-arms only the
// one consumer. The pre-fix rearm() re-enabled neither.
SLUICE_TEST_CASE(cancel_shared_token_two_consumers_deliver_and_rearm) {
    CancelToken t;
    CancelState a;
    CancelState b;
    t.request();

    auto ra = check_cancel(t, a);
    auto rb = check_cancel(t, b);
    SLUICE_CHECK(!ra.has_value());  // A delivers
    SLUICE_CHECK(!rb.has_value());  // B delivers independently (per-consumer ack)
    SLUICE_CHECK(check_cancel(t, a).has_value());  // A single-shot
    SLUICE_CHECK(check_cancel(t, b).has_value());  // B single-shot

    t.rearm();
    auto ra2 = check_cancel(t, a);
    auto rb2 = check_cancel(t, b);
    SLUICE_CHECK(!ra2.has_value());  // rearm re-enables A
    SLUICE_CHECK(!rb2.has_value());  // ... and B

    // Per-consumer re-arm: B alone re-arms itself; A stays acknowledged.
    b.reset_acknowledgement();
    auto rb3 = check_cancel(t, b);
    SLUICE_CHECK(!rb3.has_value());  // only B delivers again
    SLUICE_CHECK(check_cancel(t, a).has_value());  // A still single-shot
}

// ---- Slice 8: request is idempotent; clear resets -------------------------
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

// ---- Slice 9 (T-CANCEL-FUTURE-5): public Future<T> consumer ----------------
// The real public consumer shape: Future::cancel() requests on the Future's
// token; a producer observes the token at its cancel points with its own
// CancelState. Deterministic by construction: the producer spins on the
// token's own atomic state (is_requested, acquire — the documented cross-
// thread handshake, no sleeps) so the request always lands before the first
// cancel point; rearm() then makes the producer deliver again (report-
// progress-then-re-propagate).
SLUICE_TEST_CASE(cancel_future_producer_cancel_points_and_rearm) {
    Future<int> f;
    CancelToken& tok = f.cancel_token();
    CancelState producer_state;
    std::atomic<int> deliveries{0};

    std::thread producer([&] {
        while (!tok.is_requested()) {
            std::this_thread::yield();  // deterministic handshake on token state
        }
        for (int i = 0; i < 4; ++i) {
            if (!check_cancel(tok, producer_state).has_value()) {
                deliveries.fetch_add(1);  // exactly the first point delivers
            }
        }
        tok.rearm();  // report-progress-then-re-propagate
        if (!check_cancel(tok, producer_state).has_value()) {
            deliveries.fetch_add(1);  // the re-armed request delivers again
        }
        f.complete_with(Result<int>{0});
    });
    (void)f.cancel();  // requests AND awaits; the producer unblocks it
    producer.join();

    SLUICE_CHECK(deliveries.load() == 2);  // one per request epoch
}

SLUICE_MAIN()
