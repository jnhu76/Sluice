// K2 — Completion<T> reset/reuse kernel (#197 weak-memory pilot, target 1).
//
// Checker input for GenMC (NOT an xmake target; never compiled into Sluice).
// Same provenance and spelling rules as kernel_publication.cpp:
//
//   include/sluice/async/completion.hpp @ d98d70dd57c85b5e4c59d92b384ffa95b6027e4a
//   include/sluice/async/async_io_context.hpp:345-388
//
// Kernel shape (3 threads, 2 publication rounds, bounded waits):
//
//   backend thread  : round-1 claim+publish (A1..A7)
//                     -> bounded wait for `idle` (the reset happened)
//                     -> round-2 claim+publish (A1'..A7')
//   caller thread   : ONE acquire load; if `ready` observed:
//                       P3a result reads (storage == round-1 value)
//                       C2 reset CAS -> C3 clears -> C4 idle release
//   late observer   : bounded phase-waits; when round-2 `ready` observed:
//                       P3b storage == round-2 value (never the stale round-1
//                       value or the reset sentinel), payload == round-2
//                       payload (P2 on round 2), reap_seq == 2.
//
// Properties (concrete, observation-conditional):
//   P3a if the caller acquire-observes round-1 `ready` (and therefore wins the
//       reset CAS), its plain storage reads see exactly the round-1 published
//       result — not the initial sentinel.
//   P3b if an acquire load observes round-2 `ready`, storage is exactly the
//       round-2 result (the round-1 value and the reset sentinel are both
//       distinguishable stales), and the round-2 binding payload is visible.
//
// Phase disambiguation (why the late observer waits in phases): `idle` occurs
// at program start AND after reset; `outstanding`/`ready` occur in both
// rounds. The late observer therefore waits (1) until any non-idle state
// (round 1 started), then (2) until `resetting` or `idle` (round 1 finished),
// then (3) until `ready` — which after (2) can only be round 2's.
//
// Modeling stand-in (documented divergence from production mechanics, not
// semantics): production gates round-2 reaping through the arena's
// backend-ready linkage; the kernel gates it with the bounded waits above.
// The waits never weaken a property: on budget exhaustion the thread takes a
// give-up path and its observation-conditional assertions simply do not fire.
//
// Bounds: 3 threads; 2 rounds; wait budget WAIT_BUDGET=3 per phase (each
// phase needs at most a handful of spins in any complete execution where the
// awaited state is reached — competitors have <= 6 state-changing steps; a
// complete execution where a waiter exhausts its budget is still explored via
// its give-up path, so budgets trade exploration breadth for termination,
// never property soundness); assertion class only (plus RC11 race detection).

#include <atomic>
#include <cassert>
#include <pthread.h>

enum class St : unsigned char {
    idle, binding, outstanding, publishing, ready, resetting
};

struct Storage {
    bool has_value = false;
    bool has_error = false;
    int value = 0;    // sentinels: 0 initial / 0 after reset clear; 42 r1; 99 r2
    int error = -1;
    void set(int v, int e, bool is_err) noexcept {
        if (!is_err) { value = v; has_value = true; has_error = false; }
        else { error = e; has_error = true; has_value = false; }
    }
};

std::atomic<St> st{St::idle};
Storage storage;
unsigned long reap_seq = 0;
void* release_arena = nullptr;
unsigned long bound_slot = 0;
std::atomic<unsigned long> reap_counter{0};

static const int WAIT_BUDGET = 3;

// Round-1 + round-2 claim/publish chain (exact production shapes; see
// kernel_publication.cpp for the per-line provenance of each access).
void* backend_thread(void*) {
    // --- round 1: A1..A7 ---
    St e = St::idle;
    bool ok = st.compare_exchange_strong(
        e, St::binding, std::memory_order_acq_rel, std::memory_order_acquire);
    assert(ok);
    release_arena = reinterpret_cast<void*>(1);  // payload 1
    bound_slot = 77;
    e = St::binding;
    ok = st.compare_exchange_strong(
        e, St::outstanding, std::memory_order_acq_rel, std::memory_order_acquire);
    assert(ok);
    e = St::outstanding;
    ok = st.compare_exchange_strong(
        e, St::publishing, std::memory_order_acq_rel, std::memory_order_acquire);
    assert(ok);
    storage.set(42, -1, false);
    unsigned long s1 = ++reap_counter;
    reap_seq = s1;
    assert(s1 == 1);  // exactly-once reap stamp for round 1
    st.store(St::ready, std::memory_order_release);

    // Bounded wait for the reset (C4's idle). Unambiguous: this thread itself
    // performed the round-1 ready store, so the only idle reachable now is
    // the one C4 publishes.
    bool reset_seen = false;
    for (int i = 0; i < WAIT_BUDGET; ++i) {
        if (st.load(std::memory_order_acquire) == St::idle) { reset_seen = true; break; }
    }
    if (!reset_seen) return nullptr;  // give-up path (budget exhausted)

    // --- round 2: A1'..A7' (reuse after reset) ---
    e = St::idle;
    ok = st.compare_exchange_strong(
        e, St::binding, std::memory_order_acq_rel, std::memory_order_acquire);
    assert(ok);  // single claimant: the caller never claims
    release_arena = reinterpret_cast<void*>(2);  // payload 2
    bound_slot = 88;
    e = St::binding;
    ok = st.compare_exchange_strong(
        e, St::outstanding, std::memory_order_acq_rel, std::memory_order_acquire);
    assert(ok);
    e = St::outstanding;
    ok = st.compare_exchange_strong(
        e, St::publishing, std::memory_order_acq_rel, std::memory_order_acquire);
    assert(ok);
    storage.set(99, -1, false);
    unsigned long s2 = ++reap_counter;
    reap_seq = s2;
    assert(s2 == 2);  // monotonic reap stamps across the reuse
    st.store(St::ready, std::memory_order_release);
    return nullptr;
}

// Caller: production observer + reset chain (completion.hpp:200/217-222 for
// the observation, :234-274 for reset). The round is disambiguated race-free
// by the binding payload (written before each round's ready store, so the
// acquire observation makes the payload read safe): payload 1 => round 1,
// payload 2 => round 2. The round-2 branch is PROTOCOL-UNREACHABLE in this
// kernel (the caller is the only producer of `resetting`/`idle`, and its
// observation precedes its reset chain, so a round-2 publication can never
// exist before this thread's first load); it is kept as defensive coverage
// for future kernel edits that add another reset producer.
void* caller_thread(void*) {
    St s = st.load(std::memory_order_acquire);  // B1 gate
    if (s != St::ready) return nullptr;  // missed observation: no reset (give-up)

    if (release_arena == reinterpret_cast<void*>(2)) {
        // Observed round-2 ready (missed round 1 entirely): P3b-class check.
        assert(storage.has_value);
        assert(!storage.has_error);
        assert(storage.value == 99);
        return nullptr;  // the reset authority for round 1 has passed
    }

    // P3a: acquire observation of round-1 `ready` => exact round-1 result.
    assert(storage.has_value);
    assert(!storage.has_error);
    assert(storage.value == 42);

    // C2 — completion.hpp:249-252: CAS ready -> resetting, acq_rel, acquire.
    St e = St::ready;
    bool ok = st.compare_exchange_strong(
        e, St::resetting, std::memory_order_acq_rel, std::memory_order_acquire);
    assert(ok);  // the only resetter: this thread observed ready

    // C3 — completion.hpp:265-270: plain clears (binding payload, storage,
    // reap_seq). (release_completed_binding is arena-domain and is outside
    // this kernel; the payload clear is the ordering-relevant part.)
    release_arena = nullptr;
    bound_slot = 0;
    storage = Storage{};
    reap_seq = 0;

    // C4 — completion.hpp:273: state_.store(idle, release).
    st.store(St::idle, std::memory_order_release);
    return nullptr;
}

// Late observer: phased bounded waits, then the round-2 assertions.
void* late_observer_thread(void*) {
    // Phase 1: round 1 started (any non-idle state).
    bool started = false;
    for (int i = 0; i < WAIT_BUDGET; ++i) {
        if (st.load(std::memory_order_acquire) != St::idle) { started = true; break; }
    }
    if (!started) return nullptr;

    // Phase 2: round 1 finished (resetting or the reset's idle).
    bool finished = false;
    for (int i = 0; i < WAIT_BUDGET; ++i) {
        St s = st.load(std::memory_order_acquire);
        if (s == St::resetting || s == St::idle) { finished = true; break; }
    }
    if (!finished) return nullptr;

    // Phase 3: round-2 `ready` (the only `ready` reachable after phase 2).
    bool ready2 = false;
    for (int i = 0; i < WAIT_BUDGET; ++i) {
        if (st.load(std::memory_order_acquire) == St::ready) { ready2 = true; break; }
    }
    if (!ready2) return nullptr;

    // P3b: exact round-2 result; 42 (stale round-1) and 0 (reset sentinel)
    // are both wrong and distinguishable.
    assert(storage.has_value);
    assert(!storage.has_error);
    assert(storage.value == 99);
    // P2 on round 2: round-2 payload visible.
    assert(release_arena == reinterpret_cast<void*>(2));
    assert(bound_slot == 88);
    // Reap stamps: two rounds ran; round 2 stamped the member last.
    assert(reap_seq == 2);
    return nullptr;
}

int main() {
    pthread_t a, b, c;
    pthread_create(&a, nullptr, backend_thread, nullptr);
    pthread_create(&b, nullptr, caller_thread, nullptr);
    pthread_create(&c, nullptr, late_observer_thread, nullptr);
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    pthread_join(c, nullptr);
    return 0;
}
