// K1 — Completion<T> publication kernel (#197 weak-memory pilot, target 1).
//
// This file is NOT part of any xmake target and is never compiled into Sluice.
// It is a checker input for GenMC (scripts/weakmem/verify-completion-weak-memory.sh)
// and mirrors the EXACT production atomic access shapes with per-line provenance:
//
//   include/sluice/async/completion.hpp @ d98d70dd57c85b5e4c59d92b384ffa95b6027e4a
//   entry path: AsyncBackend::publish / begin_binding / install_binding /
//   commit_binding — include/sluice/async/async_io_context.hpp:345-388
//
// Spelling note: GenMC's compilation path does not accept the scoped enumerator
// spelling `std::memory_order::release`. Every order below uses the unscoped
// form `std::memory_order_release` — the SAME enumerator constant of the same
// std::memory_order enumeration, an identical value with identical semantics.
//
// Kernel shape (2 threads, 1 publication round, no loops):
//
//   backend thread   : A1 claim CAS -> A2 payload -> A3 commit CAS
//                      -> A4 publish CAS -> A5 storage -> A6 reap seq -> A7 ready
//   observer thread  : one acquire load; branch on what it observes:
//                        outstanding -> P2/I2  (binding payload fully installed)
//                        ready       -> P1      (storage initialized + payload
//                                             + reap_seq stamped)
//
// Properties (concrete, observation-conditional):
//   P1  if an acquire load observes `ready`, the plain storage writes (A5) and
//       the reap-seq write (A6) are visible: storage is exactly the published
//       result, and the binding payload (A2) is visible through the sequenced
//       A3/A7 release chain.
//   P2  if an acquire load observes `outstanding`, the binding payload (A2) is
//       fully installed (production I2: "an acquire observer of `outstanding`
//       sees the full binding").
//
// Bounds: 2 threads; 1 round; 0 loop iterations; assertion class only (plus
// GenMC's RC11 data-race detection). No liveness claim.

#include <atomic>
#include <cassert>
#include <pthread.h>

// completion.hpp:422-424 — enum class State : std::uint8_t
enum class St : unsigned char {
    idle, binding, outstanding, publishing, ready, resetting
};

// completion.hpp:445-465 — Storage (T := int for the kernel; sentinel value 0,
// error sentinel -1). All fields PLAIN (non-atomic), like production.
struct Storage {
    bool has_value = false;
    bool has_error = false;
    int value = 0;
    int error = -1;
    // completion.hpp:453-456 Storage::set — plain writes
    void set(int v, int e, bool is_err) noexcept {
        if (!is_err) { value = v; has_value = true; has_error = false; }
        else { error = e; has_error = true; has_value = false; }
    }
};

// The Completion object under test — field-for-field the production layout
// relevant to publication (state_ atomic; everything else plain).
St initial_state = St::idle;  // only used for the initializer below
std::atomic<St> st{St::idle};                                   // :425
Storage storage;                                                // :439
unsigned long reap_seq = 0;                                     // :426
void* release_arena = nullptr;                                  // :434
unsigned long bound_slot = 0;                                   // :435

// completion.hpp:109-112 — process-wide monotonic reap counter. Default
// (seq_cst) ++ on std::atomic<std::uint64_t>; exact production form.
std::atomic<unsigned long> reap_counter{0};

// Publishes a successful terminal result 42 (round 1) — the exact
// publish_from_reap call sequence from completion.hpp:404-415, preceded by the
// Phase-B two-stage claim (completion.hpp:321-346, 367-371).
void* backend_thread(void*) {
    // A1 — completion.hpp:321-327 begin_binding_for_backend:
    //      CAS idle -> binding, acq_rel, failure acquire.
    St e = St::idle;
    bool ok = st.compare_exchange_strong(
        e, St::binding, std::memory_order_acq_rel, std::memory_order_acquire);
    assert(ok);  // production fail-fast shape: single claimant from idle

    // A2 — completion.hpp:367-371 install_binding_for_backend: PLAIN writes.
    release_arena = reinterpret_cast<void*>(1);
    bound_slot = 77;

    // A3 — completion.hpp:328-346 commit_binding_to_outstanding:
    //      CAS binding -> outstanding, acq_rel, failure acquire.
    //      (submit-success linearization point; I2 edge.)
    e = St::binding;
    ok = st.compare_exchange_strong(
        e, St::outstanding, std::memory_order_acq_rel, std::memory_order_acquire);
    assert(ok);

    // A4 — completion.hpp:404-411 publish_from_reap head:
    //      CAS outstanding -> publishing, acq_rel, failure acquire.
    e = St::outstanding;
    ok = st.compare_exchange_strong(
        e, St::publishing, std::memory_order_acq_rel, std::memory_order_acquire);
    assert(ok);

    // A5 — completion.hpp:412 storage_.set(std::move(res)): PLAIN writes.
    storage.set(42, -1, false);

    // A6 — completion.hpp:413 + :109-112: reap_seq_ = next_reap_seq();
    //      seq_cst RMW ++, then PLAIN member write.
    unsigned long s = ++reap_counter;
    reap_seq = s;

    // A7 — completion.hpp:414: state_.store(State::ready, release).
    st.store(St::ready, std::memory_order_release);
    return nullptr;
}

// One acquire observation — the production observer shapes are ready()
// (completion.hpp:200) and result() (completion.hpp:217-222): acquire load,
// then plain storage reads only when `ready` was observed.
void* observer_thread(void*) {
    St s = st.load(std::memory_order_acquire);

    if (s == St::outstanding) {
        // P2 / production I2: observer of `outstanding` sees the full binding.
        assert(release_arena == reinterpret_cast<void*>(1));
        assert(bound_slot == 77);
    } else if (s == St::ready) {
        // P1: acquire observation of `ready` => storage initialized exactly.
        assert(storage.has_value);
        assert(!storage.has_error);
        assert(storage.value == 42);
        assert(storage.error == -1);
        // P1 (reap-seq half): exactly one publish so far stamped the member.
        assert(reap_seq == 1);
        // P1 (payload visibility through the sequenced A3/A7 release chain).
        assert(release_arena == reinterpret_cast<void*>(1));
        assert(bound_slot == 77);
    }
    // binding / publishing / resetting: no observation-conditional claim.
    return nullptr;
}

int main() {
    (void)initial_state;
    pthread_t a, b;
    pthread_create(&a, nullptr, backend_thread, nullptr);
    pthread_create(&b, nullptr, observer_thread, nullptr);
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    return 0;
}
