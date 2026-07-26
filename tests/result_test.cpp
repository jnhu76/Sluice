// Tests for IoError + Result<T>/Result<void>. Behavior-focused, not shape-focused.
#include "harness.hpp"

#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cerrno>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

// ---- IoError behavior ----

SLUICE_TEST_CASE(to_string_round_trips_every_code) {
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::eof) == std::string_view("eof"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::no_space) ==
                 std::string_view("no_space"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::canceled) ==
                 std::string_view("canceled"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::interrupted) ==
                 std::string_view("interrupted"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::would_block) ==
                 std::string_view("would_block"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::permission_denied) ==
                 std::string_view("permission_denied"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::invalid_state) ==
                 std::string_view("invalid_state"));
    SLUICE_CHECK(sluice::to_string(sluice::IoError::Code::backend_error) ==
                 std::string_view("backend_error"));
}

// from_errno_value must use portable <cerrno> macros (not hardcoded ints) and
// always preserve the original errno in os_errno regardless of the Code mapping.
SLUICE_TEST_CASE(from_errno_maps_known_macros_and_preserves_os_errno) {
    auto check = [](int errc, sluice::IoError::Code expected) {
        sluice::IoError e = sluice::from_errno_value(errc);
        SLUICE_CHECK(e.os_errno == errc); // always preserved
        SLUICE_CHECK(e.code == expected);
    };
    check(EACCES, sluice::IoError::Code::permission_denied);
    check(EPERM, sluice::IoError::Code::permission_denied);
    check(ENOSPC, sluice::IoError::Code::no_space);
    check(EINTR, sluice::IoError::Code::interrupted);
    check(EAGAIN, sluice::IoError::Code::would_block);
    check(ENOENT, sluice::IoError::Code::permission_denied);
    check(ENOTDIR, sluice::IoError::Code::permission_denied);
#ifdef ECANCELED
    check(ECANCELED, sluice::IoError::Code::canceled);
#endif
#if EWOULDBLOCK != EAGAIN
    check(EWOULDBLOCK, sluice::IoError::Code::would_block);
#endif
}

SLUICE_TEST_CASE(from_errno_unknown_value_maps_to_backend_error_preserving_os_errno) {
    sluice::IoError e = sluice::from_errno_value(9999);
    SLUICE_CHECK(e.os_errno == 9999);
    SLUICE_CHECK(e.code == sluice::IoError::Code::backend_error);
}

SLUICE_TEST_CASE(from_errno_zero_is_benign) {
    sluice::IoError e = sluice::from_errno_value(0);
    SLUICE_CHECK(e.os_errno == 0);
    SLUICE_CHECK(e.code == sluice::IoError::Code::backend_error);
}

// ---- Result<T> behavior ----

SLUICE_TEST_CASE(result_value_holds_constructed_value) {
    sluice::Result<int> r{42};
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 42);
}

SLUICE_TEST_CASE(result_error_holds_error) {
    sluice::Result<int> r =
        sluice::make_unexpected<int>(sluice::IoError{sluice::IoError::Code::eof});
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST_CASE(result_void_can_be_success) {
    sluice::Result<void> r{};
    SLUICE_CHECK(r.has_value());
}

SLUICE_TEST_CASE(result_void_can_be_error) {
    sluice::Result<void> r =
        sluice::make_unexpected<void>(sluice::IoError{sluice::IoError::Code::canceled});
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == sluice::IoError::Code::canceled);
}

SLUICE_TEST_CASE(result_value_or_returns_value_on_success) {
    sluice::Result<int> r{7};
    SLUICE_CHECK(r.value_or(99) == 7);
}

SLUICE_TEST_CASE(result_value_or_returns_fallback_on_error) {
    sluice::Result<int> r =
        sluice::make_unexpected<int>(sluice::IoError{sluice::IoError::Code::eof});
    SLUICE_CHECK(r.value_or(99) == 99);
}

SLUICE_TEST_CASE(result_can_carry_a_move_only_type_like_vector) {
    sluice::Result<std::vector<int>> r{std::vector<int>{1, 2, 3}};
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value().size() == 3);
    SLUICE_CHECK(r.value()[2] == 3);
}

SLUICE_TEST_CASE(result_value_can_be_moved_out) {
    sluice::Result<std::string> r{std::string("payload")};
    std::string out = std::move(r).value();
    SLUICE_CHECK(out == "payload");
}

// Regression for the assignment UB: a throwing copy ctor must not leave the
// Result claiming to hold a value it never constructed. Previously, has_value_
// was set to true before placement-new; if the ctor threw, the destructor ran
// value_.~T() on uninitialized storage (UB). Now has_value_ is published only
// after a successful construction.
namespace {
int g_throw_on_copy_after = -1; // copy number that should throw (-1 = never)
struct ThrowingCopy {
    int v;
    static int copies;
    ThrowingCopy(int x) : v(x) {}
    ThrowingCopy(const ThrowingCopy& o) : v(o.v) {
        if (g_throw_on_copy_after >= 0 && ++copies > g_throw_on_copy_after) {
            throw std::runtime_error("induced copy failure");
        }
    }
    ThrowingCopy& operator=(const ThrowingCopy&) = delete;
};
int ThrowingCopy::copies = 0;
} // namespace

SLUICE_TEST_CASE(result_copy_assignment_survives_throwing_ctor) {
    // The next copy of ThrowingCopy throws. We exercise assignment under
    // exceptions; if the UB were present, the destructor would corrupt state
    // and the next line could crash or trip the sanitizers.
    sluice::Result<ThrowingCopy> src{ThrowingCopy{7}};
    sluice::Result<ThrowingCopy> dst{ThrowingCopy{0}};
    ThrowingCopy::copies = 0;
    g_throw_on_copy_after = 0; // first copy (the assignment) throws
    bool threw = false;
    try {
        dst = src;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_on_copy_after = -1;
    SLUICE_CHECK(threw);
    // After the throw, dst must be safely destructible (no UB on exit). The
    // value state is unspecified-per-standard; we deliberately do NOT assert
    // any specific value — only that we got here without a sanitizer report.
    // The throwing-type's destructor is trivial, so a clean scope exit proves
    // the destructor path ran on a destroy-safe object.
    (void)dst;
}

SLUICE_TEST_CASE(result_copy_works_and_original_unchanged) {
    sluice::Result<int> original{42};
    sluice::Result<int> copied = original;
    SLUICE_CHECK(copied.has_value() && copied.value() == 42);
    SLUICE_CHECK(original.has_value() && original.value() == 42);

    sluice::Result<int> err_src =
        sluice::make_unexpected<int>(sluice::IoError{sluice::IoError::Code::eof});
    sluice::Result<int> copied_err = err_src;
    SLUICE_CHECK(!copied_err.has_value());
    SLUICE_CHECK(copied_err.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST_CASE(result_assignment_transfers_state) {
    sluice::Result<int> a{1};
    sluice::Result<int> b{2};
    a = b;
    SLUICE_CHECK(a.value() == 2 && b.value() == 2);

    b = sluice::make_unexpected<int>(sluice::IoError{sluice::IoError::Code::canceled});
    a = b;
    SLUICE_CHECK(!a.has_value());
    SLUICE_CHECK(a.error().code == sluice::IoError::Code::canceled);
}

// =============================================================================
// E15-P1-01 / E15-P1-02 regressions: assignment state machine + noexcept truth.
//
// These cases close two confirmed defects in result_storage<T>:
//   P1-01: a throwing replacement construction during assignment must not leave
//          the discriminator claiming a value is live when the storage has
//          already been destroyed. The header comment claimed this was already
//          handled; destroy() never cleared has_value_, so the post-throw
//          destructor ran value_.~T() a second time on dead storage (UB).
//   P1-02: Result<T>::operator=(Result&&) advertised noexcept conditioned on
//          is_nothrow_move_assignable_v<T>, but the body performs placement-new
//          move CONSTRUCTION (never T::operator=). A type with throwing move
//          ctor + non-throwing move assign caused a noexcept function to throw
//          and terminate the process.
//
// Lifetime observers: a T whose destructor increments/decrements global counters
// so a double-destroy or leak is observable without UB-dependent crashes.
// =============================================================================

namespace {
// Forward-declared thresholds so the structs' methods can reference them.
// -1 = never throw; otherwise the 1-indexed copy/move number that throws.
int g_throw_copy_n = -1;
int g_throw_move_n = -1;

// Lifetime counter: every construction bumps live; every destruction drops it.
// A double-destroy drops live twice; a leak leaves live > 0 at scope exit.
struct Lifetime {
    int v;
    static int live;
    static int ctor_calls;
    static int dtor_calls;
    Lifetime(int x) : v(x) { ++live; ++ctor_calls; }
    Lifetime(const Lifetime& o) : v(o.v) { ++live; ++ctor_calls; }
    Lifetime(Lifetime&& o) noexcept : v(o.v) { ++live; ++ctor_calls; }
    Lifetime& operator=(const Lifetime& o) { v = o.v; return *this; }
    Lifetime& operator=(Lifetime&& o) noexcept { v = o.v; return *this; }
    ~Lifetime() { --live; ++dtor_calls; }
};
int Lifetime::live = 0;
int Lifetime::ctor_calls = 0;
int Lifetime::dtor_calls = 0;

void reset_lifetime_counters() {
    Lifetime::live = 0;
    Lifetime::ctor_calls = 0;
    Lifetime::dtor_calls = 0;
}

// Throws on Nth copy construction (1-indexed). Each instance owns a per-object
// "already-destroyed" sentinel so a SECOND destructor call on the same storage
// is observable independent of how many other live instances exist elsewhere.
struct ThrowOnNthCopy {
    // Sentinel pattern: ctor writes ALIVE; dtor writes DEAD. A second dtor call
    // (the double-destroy UB) sees DEAD and bumps the global counter.
    static constexpr int ALIVE = 0x57; // sentinel
    static constexpr int DEAD = 0xDE;  // sentinel
    int v;
    int state; // ALIVE when live; DEAD after dtor runs
    static int copies;
    static int dt_double_destroy; // bumped if ~T() runs on already-DEAD storage
    explicit ThrowOnNthCopy(int x) : v(x), state(ALIVE) {}
    ThrowOnNthCopy(const ThrowOnNthCopy& o) : v(o.v), state(ALIVE) {
        ++copies;
        if (copies == g_throw_copy_n) throw std::runtime_error("induced copy");
    }
    ThrowOnNthCopy& operator=(const ThrowOnNthCopy&) = delete;
    ~ThrowOnNthCopy() {
        if (state == DEAD) {
            ++dt_double_destroy; // observable double-destroy
            return;
        }
        state = DEAD;
    }
};
int ThrowOnNthCopy::copies = 0;
int ThrowOnNthCopy::dt_double_destroy = 0;

// Throws on Nth move construction (1-indexed). Per-object DEAD/ALIVE sentinel
// so a second destructor call (the double-destroy UB) is observable. Non-
// throwing move ASSIGNMENT so is_nothrow_move_assignable_v<ThrowOnNthMove> is
// true while is_nothrow_move_constructible_v is false — the P1-02 trait split.
struct ThrowOnNthMove {
    static constexpr int ALIVE = 0x57;
    static constexpr int DEAD = 0xDE;
    int v;
    int state;
    static int moves;
    static int dt_double_destroy;
    explicit ThrowOnNthMove(int x) : v(x), state(ALIVE) {}
    ThrowOnNthMove(const ThrowOnNthMove& o) : v(o.v), state(ALIVE) {}
    ThrowOnNthMove(ThrowOnNthMove&& o) noexcept(false) : v(o.v), state(ALIVE) {
        ++moves;
        if (moves == g_throw_move_n) throw std::runtime_error("induced move");
    }
    // Non-throwing move assignment (so is_nothrow_move_assignable_v is true).
    ThrowOnNthMove& operator=(ThrowOnNthMove&& o) noexcept {
        v = o.v;
        return *this;
    }
    ThrowOnNthMove& operator=(const ThrowOnNthMove&) = delete;
    ~ThrowOnNthMove() {
        if (state == DEAD) { ++dt_double_destroy; return; }
        state = DEAD;
    }
};
int ThrowOnNthMove::moves = 0;
int ThrowOnNthMove::dt_double_destroy = 0;
} // namespace

// ---- Live-object / destruction counts are exact under normal use ------------

SLUICE_TEST_CASE(result_lifetime_counts_match_constructions) {
    reset_lifetime_counters();
    {
        sluice::Result<Lifetime> a{Lifetime{1}};
        sluice::Result<Lifetime> b{Lifetime{2}};
        SLUICE_CHECK(Lifetime::live == 2); // one temp + one in a (b same)
        // Note: Result{T{1}} constructs a temp Lifetime (live=1), then move-
        // constructs into storage (live=2), then temp destructs (live=1).
        // So after both: live == 2 (one per Result), ctor_calls == 4, dtor 2.
        SLUICE_CHECK(Lifetime::ctor_calls == 4);
        SLUICE_CHECK(Lifetime::dtor_calls == 2);
        SLUICE_CHECK(Lifetime::live == 2);
    }
    // After scope exit both Results destroyed cleanly: net live == 0.
    SLUICE_CHECK(Lifetime::live == 0);
    SLUICE_CHECK(Lifetime::dtor_calls == 4);
}

// ---- value->value copy assign destroys old, constructs new exactly once -----

SLUICE_TEST_CASE(result_value_to_value_copy_assign_lifetime_exact) {
    reset_lifetime_counters();
    {
        sluice::Result<Lifetime> a{Lifetime{1}};
        sluice::Result<Lifetime> b{Lifetime{2}};
        const int live_before = Lifetime::live;       // 2
        const int dtor_before = Lifetime::dtor_calls; // 2
        a = b; // destroy a's Lifetime, copy-construct b's into a
        SLUICE_CHECK(Lifetime::live == live_before);  // unchanged net
        SLUICE_CHECK(Lifetime::dtor_calls == dtor_before + 1); // a's old destroyed
        SLUICE_CHECK(a.value().v == 2);
        SLUICE_CHECK(b.value().v == 2);
    }
    SLUICE_CHECK(Lifetime::live == 0); // no leak
}

// ---- value->value move assign destroys old, move-constructs new -----------

SLUICE_TEST_CASE(result_value_to_value_move_assign_lifetime_exact) {
    reset_lifetime_counters();
    {
        sluice::Result<Lifetime> a{Lifetime{1}};
        sluice::Result<Lifetime> b{Lifetime{2}};
        const int live_before = Lifetime::live;
        const int dtor_before = Lifetime::dtor_calls;
        a = std::move(b); // destroy a's old, move-construct from b
        SLUICE_CHECK(Lifetime::live == live_before);
        SLUICE_CHECK(Lifetime::dtor_calls == dtor_before + 1);
        SLUICE_CHECK(a.value().v == 2);
        // b's storage still holds a moved-from Lifetime (live); we only assert
        // it is safely destructible at scope exit (no double-destroy).
    }
    SLUICE_CHECK(Lifetime::live == 0);
}

// ---- value->error and error->value assignments flip the discriminator ------

SLUICE_TEST_CASE(result_value_to_error_assign_clears_value) {
    reset_lifetime_counters();
    {
        sluice::Result<Lifetime> a{Lifetime{1}};
        sluice::Result<Lifetime> b =
            sluice::make_unexpected<Lifetime>(sluice::IoError{sluice::IoError::Code::eof});
        SLUICE_CHECK(Lifetime::live == 1);
        a = b; // a's Lifetime destroyed, a becomes error
        SLUICE_CHECK(Lifetime::live == 0); // a's value destroyed, b had none
        SLUICE_CHECK(!a.has_value());
        SLUICE_CHECK(a.error().code == sluice::IoError::Code::eof);
    }
    SLUICE_CHECK(Lifetime::live == 0);
}

SLUICE_TEST_CASE(result_error_to_value_copy_assign_constructs_value) {
    reset_lifetime_counters();
    {
        sluice::Result<Lifetime> a =
            sluice::make_unexpected<Lifetime>(sluice::IoError{sluice::IoError::Code::eof});
        sluice::Result<Lifetime> b{Lifetime{5}};
        SLUICE_CHECK(Lifetime::live == 1); // only b
        a = b; // a had no value, copy-construct b's into a
        SLUICE_CHECK(Lifetime::live == 2); // a now also holds one
        SLUICE_CHECK(a.has_value() && a.value().v == 5);
        SLUICE_CHECK(b.has_value() && b.value().v == 5);
    }
    SLUICE_CHECK(Lifetime::live == 0);
}

SLUICE_TEST_CASE(result_error_to_value_move_assign_constructs_value) {
    reset_lifetime_counters();
    {
        sluice::Result<Lifetime> a =
            sluice::make_unexpected<Lifetime>(sluice::IoError{sluice::IoError::Code::eof});
        sluice::Result<Lifetime> b{Lifetime{9}};
        SLUICE_CHECK(Lifetime::live == 1);
        a = std::move(b); // a had no value, move-construct b's into a
        SLUICE_CHECK(Lifetime::live == 2);
        SLUICE_CHECK(a.has_value() && a.value().v == 9);
    }
    SLUICE_CHECK(Lifetime::live == 0);
}

SLUICE_TEST_CASE(result_error_to_error_assign_no_value_lifecycle) {
    reset_lifetime_counters();
    {
        sluice::Result<Lifetime> a =
            sluice::make_unexpected<Lifetime>(sluice::IoError{sluice::IoError::Code::eof});
        sluice::Result<Lifetime> b =
            sluice::make_unexpected<Lifetime>(sluice::IoError{sluice::IoError::Code::canceled});
        a = b;
        SLUICE_CHECK(!a.has_value());
        SLUICE_CHECK(a.error().code == sluice::IoError::Code::canceled);
        SLUICE_CHECK(Lifetime::live == 0);
    }
    SLUICE_CHECK(Lifetime::live == 0);
}

// ---- E15-P1-01: throwing replacement construction must not double-destroy --
//
// Old behavior: destroy() ran ~T() but did NOT clear has_value_. The throwing
// placement-new then left has_value_ == true pointing at dead storage, so the
// destructor at scope exit ran ~T() AGAIN. With a non-trivial destructor that
// observes its own liveness, the second call is observable as a counter going
// negative / a double-destroy flag.
//
// Required post-fix: dt_double_destroy stays 0; the object is left in a valid
// error state (has_value_ == false) and is safely destructible.

SLUICE_TEST_CASE(result_copy_assign_throwing_ctor_no_double_destroy) {
    // dst holds a value; src holds a value whose copy throws. The assignment
    // must propagate the exception, leave dst in a destroy-safe state, and the
    // eventual destructor must not run ~T() on already-destroyed storage.
    ThrowOnNthCopy::copies = 0;
    ThrowOnNthCopy::dt_double_destroy = 0;
    // Construct both Results FIRST (g_throw_copy_n == -1 means no throw during
    // their own initializations). Then arm the throw for the assignment copy.
    sluice::Result<ThrowOnNthCopy> src{ThrowOnNthCopy{7}};
    sluice::Result<ThrowOnNthCopy> dst{ThrowOnNthCopy{0}};
    SLUICE_CHECK(src.has_value() && dst.has_value());
    ThrowOnNthCopy::copies = 0;   // reset so the very next copy is the trigger
    g_throw_copy_n = 1;           // NEXT copy construction throws
    bool threw = false;
    try {
        dst = src; // destroy dst's value, then copy-construct src's -> throws
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_copy_n = -1;
    SLUICE_CHECK(threw);
    // THE REGRESSION ASSERTION (P1-01): after a throwing replacement, the
    // discriminator MUST match the actually-active storage. The old value was
    // destroyed in step 1 of the assignment; the new value's construction
    // threw, so NO value is live. dst.has_value() MUST therefore be false.
    // (Old buggy behavior: destroy() failed to clear has_value_, so the
    // discriminator lied and ~result_storage() later ran ~T() on dead storage.)
    SLUICE_CHECK(dst.has_value() == false);
    // And the destructor at scope exit must not report a double-destroy.
    SLUICE_CHECK(ThrowOnNthCopy::dt_double_destroy == 0);
}

SLUICE_TEST_CASE(result_move_assign_throwing_move_ctor_no_double_destroy) {
    ThrowOnNthMove::moves = 0;
    ThrowOnNthMove::dt_double_destroy = 0;
    // Construct both first; ThrowOnNthMove has a real (noexcept-false) move
    // ctor so its own initialization won't trip the move counter.
    sluice::Result<ThrowOnNthMove> src{ThrowOnNthMove{7}};
    sluice::Result<ThrowOnNthMove> dst{ThrowOnNthMove{0}};
    SLUICE_CHECK(src.has_value() && dst.has_value());
    ThrowOnNthMove::moves = 0;     // next move is the assignment's
    g_throw_move_n = 1;
    bool threw = false;
    try {
        dst = std::move(src); // destroy dst, move-construct src's -> throws
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_move_n = -1;
    SLUICE_CHECK(threw);
    // P1-01 (discriminator truth): no value live after the throw.
    SLUICE_CHECK(dst.has_value() == false);
    SLUICE_CHECK(ThrowOnNthMove::dt_double_destroy == 0);
}

// Variant: throwing replacement into an ERROR-state Result must also be safe
// (no value to destroy, but the discriminator must remain false after throw).
SLUICE_TEST_CASE(result_copy_assign_throwing_ctor_into_error_state_safe) {
    ThrowOnNthCopy::copies = 0;
    ThrowOnNthCopy::dt_double_destroy = 0;
    sluice::Result<ThrowOnNthCopy> src{ThrowOnNthCopy{7}};
    sluice::Result<ThrowOnNthCopy> dst =
        sluice::make_unexpected<ThrowOnNthCopy>(
            sluice::IoError{sluice::IoError::Code::eof});
    ThrowOnNthCopy::copies = 0;   // next copy is the assignment's
    g_throw_copy_n = 1;
    bool threw = false;
    try {
        dst = src;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_copy_n = -1;
    SLUICE_CHECK(threw);
    // dst started as error; construction threw, so it MUST still report no
    // value (discriminator truth).
    SLUICE_CHECK(dst.has_value() == false);
    SLUICE_CHECK(ThrowOnNthCopy::dt_double_destroy == 0);
}

// ---- Throwing COPY/MOVE CONSTRUCTION (not assignment) must also be safe -----
// (Required outcome P1-01: "throwing copy and move construction leave the
// object in a valid, destructible state".)
//
// For copy-ctor: ThrowOnNthCopy has no move ctor, so Result(src) copy-constructs
// the storage, which invokes T(const T&) — that is the throw point we arm.
SLUICE_TEST_CASE(result_copy_ctor_throwing_leaves_destructible) {
    ThrowOnNthCopy::copies = 0;
    ThrowOnNthCopy::dt_double_destroy = 0;
    sluice::Result<ThrowOnNthCopy> src{ThrowOnNthCopy{7}};
    ThrowOnNthCopy::copies = 0;   // next copy is the storage copy-ctor's
    g_throw_copy_n = 1;
    bool threw = false;
    try {
        sluice::Result<ThrowOnNthCopy> dst{src}; // NOLINT
        (void)dst;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_copy_n = -1;
    SLUICE_CHECK(threw);
    SLUICE_CHECK(ThrowOnNthCopy::dt_double_destroy == 0);
}

SLUICE_TEST_CASE(result_move_ctor_throwing_leaves_destructible) {
    ThrowOnNthMove::moves = 0;
    ThrowOnNthMove::dt_double_destroy = 0;
    sluice::Result<ThrowOnNthMove> src{ThrowOnNthMove{7}};
    ThrowOnNthMove::moves = 0;     // next move is the storage move-ctor's
    g_throw_move_n = 1;
    bool threw = false;
    try {
        sluice::Result<ThrowOnNthMove> dst{std::move(src)}; // NOLINT
        (void)dst;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_move_n = -1;
    SLUICE_CHECK(threw);
    SLUICE_CHECK(ThrowOnNthMove::dt_double_destroy == 0);
}

// ---- E15-P1-02: noexcept conditions must match the actual operations -------
//
// The body of Result::operator=(Result&&) does placement-new move CONSTRUCTION
// (never T::operator=). Its noexcept must therefore be governed by
// is_nothrow_move_CONSTRUCTIBLE_v<T>, not is_nothrow_move_ASSIGNABLE_v<T>.
// Distinguish the traits with a type whose move-assign is noexcept but whose
// move-ctor throws; the public operator= must report noexcept(false).

static_assert(
    std::is_nothrow_move_assignable_v<ThrowOnNthMove>,
    "test fixture invariant: ThrowOnNthMove move-assign is noexcept");
static_assert(
    !std::is_nothrow_move_constructible_v<ThrowOnNthMove>,
    "test fixture invariant: ThrowOnNthMove move-ctor may throw");

SLUICE_TEST_CASE(result_move_assign_noexcept_matches_move_ctor_not_move_assign) {
    // The body performs move-construction; advertising noexcept(true) would
    // terminate the process if the move ctor threw. The advertised spec must
    // equal is_nothrow_move_constructible_v<T> (false here).
    constexpr bool advertised =
        noexcept(std::declval<sluice::Result<ThrowOnNthMove>&>() =
                     std::declval<sluice::Result<ThrowOnNthMove>&&>());
    SLUICE_CHECK(advertised ==
                 std::is_nothrow_move_constructible_v<ThrowOnNthMove>);
    SLUICE_CHECK(advertised == false);
}

SLUICE_TEST_CASE(result_move_assign_noexcept_true_for_nothrow_move_ctor_type) {
    // Sanity: for a type with noexcept move-ctor, the spec is noexcept(true).
    constexpr bool advertised_int =
        noexcept(std::declval<sluice::Result<int>&>() =
                     std::declval<sluice::Result<int>&&>());
    SLUICE_CHECK(advertised_int == std::is_nothrow_move_constructible_v<int>);
    SLUICE_CHECK(advertised_int == true);
}

SLUICE_TEST_CASE(result_move_ctor_noexcept_matches_move_ctor_trait) {
    constexpr bool advertised =
        std::is_nothrow_move_constructible_v<sluice::Result<ThrowOnNthMove>>;
    SLUICE_CHECK(advertised ==
                 std::is_nothrow_move_constructible_v<ThrowOnNthMove>);
}

// ---- Runtime side of E15-P1-02: a throwing move out of an Result<T> must ----
// propagate, not terminate. The noexcept(false) above is what makes this legal.
// We can only safely observe propagation when the source value's move-ctor is
// the one that throws; result_storage's move-ctor will then propagate.

SLUICE_TEST_CASE(result_move_assign_with_throwing_move_propagates_not_terminates) {
    ThrowOnNthMove::moves = 0;
    ThrowOnNthMove::dt_double_destroy = 0;
    bool threw = false;
    sluice::Result<ThrowOnNthMove> src{ThrowOnNthMove{7}};
    sluice::Result<ThrowOnNthMove> dst{ThrowOnNthMove{0}};
    ThrowOnNthMove::moves = 0;     // next move is the assignment's
    g_throw_move_n = 1;
    try {
        dst = std::move(src);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_move_n = -1;
    SLUICE_CHECK(threw);                     // propagated, not std::terminate
    SLUICE_CHECK(ThrowOnNthMove::dt_double_destroy == 0);
}

// ---- F-00 closeout: post-throw state must be a VALID error state -----------
//
// destroy_and_clear() writes a deterministic IoError{invalid_state} sentinel
// so that callers observing !has_value() can safely call error() and get a
// meaningful code. The object must also be recoverable: re-assignable to both
// value and error states.

SLUICE_TEST_CASE(result_throwing_assign_leaves_deterministic_error_state) {
    ThrowOnNthCopy::copies = 0;
    ThrowOnNthCopy::dt_double_destroy = 0;
    sluice::Result<ThrowOnNthCopy> src{ThrowOnNthCopy{7}};
    sluice::Result<ThrowOnNthCopy> dst{ThrowOnNthCopy{0}};
    ThrowOnNthCopy::copies = 0;
    g_throw_copy_n = 1;
    bool threw = false;
    try {
        dst = src;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_copy_n = -1;
    SLUICE_CHECK(threw);

    // F-00: post-throw public state must be a valid, distinguishable error.
    SLUICE_CHECK(!dst.has_value());
    SLUICE_CHECK(dst.error().code == sluice::IoError::Code::invalid_state);

    // Recoverability: re-assign to a valid value.
    ThrowOnNthCopy::copies = 0;
    g_throw_copy_n = -1;  // no throw
    sluice::Result<ThrowOnNthCopy> good{ThrowOnNthCopy{42}};
    dst = good;
    SLUICE_CHECK(dst.has_value());
    SLUICE_CHECK(dst.value().v == 42);

    // Recoverability: re-assign to an error.
    dst = sluice::make_unexpected<ThrowOnNthCopy>(
        sluice::IoError{sluice::IoError::Code::eof});
    SLUICE_CHECK(!dst.has_value());
    SLUICE_CHECK(dst.error().code == sluice::IoError::Code::eof);

    SLUICE_CHECK(ThrowOnNthCopy::dt_double_destroy == 0);
}

SLUICE_TEST_CASE(result_throwing_move_assign_leaves_deterministic_error_state) {
    ThrowOnNthMove::moves = 0;
    ThrowOnNthMove::dt_double_destroy = 0;
    sluice::Result<ThrowOnNthMove> src{ThrowOnNthMove{7}};
    sluice::Result<ThrowOnNthMove> dst{ThrowOnNthMove{0}};
    ThrowOnNthMove::moves = 0;
    g_throw_move_n = 1;
    bool threw = false;
    try {
        dst = std::move(src);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    g_throw_move_n = -1;
    SLUICE_CHECK(threw);

    // F-00: deterministic error sentinel.
    SLUICE_CHECK(!dst.has_value());
    SLUICE_CHECK(dst.error().code == sluice::IoError::Code::invalid_state);

    // Recoverability: re-assign to a valid value.
    sluice::Result<ThrowOnNthMove> good{ThrowOnNthMove{99}};
    dst = std::move(good);
    SLUICE_CHECK(dst.has_value());
    SLUICE_CHECK(dst.value().v == 99);

    SLUICE_CHECK(ThrowOnNthMove::dt_double_destroy == 0);
}

// ---- Self move-assignment must not corrupt state (no aliasing UB) -----------

SLUICE_TEST_CASE(result_self_move_assignment_safe) {
    reset_lifetime_counters();
    {
        sluice::Result<Lifetime> a{Lifetime{42}};
        Lifetime* addr = &a.value();
        // Self move-assign via pointer alias to suppress -Wself-move while still
        // producing the self-assignment case at runtime.
        sluice::Result<Lifetime>* ap = &a;
        a = std::move(*ap);
        SLUICE_CHECK(Lifetime::live == 1);
        SLUICE_CHECK(a.has_value());
        SLUICE_CHECK(&a.value() == addr);
        SLUICE_CHECK(a.value().v == 42);
    }
    SLUICE_CHECK(Lifetime::live == 0);
}

SLUICE_TEST_CASE(result_self_copy_assignment_safe) {
    reset_lifetime_counters();
    {
        sluice::Result<Lifetime> a{Lifetime{42}};
        Lifetime* addr = &a.value();
        const sluice::Result<Lifetime>& cref = a;
        a = cref; // self copy-assign
        SLUICE_CHECK(Lifetime::live == 1);
        SLUICE_CHECK(a.has_value());
        SLUICE_CHECK(&a.value() == addr);
        SLUICE_CHECK(a.value().v == 42);
    }
    SLUICE_CHECK(Lifetime::live == 0);
}

// ---- Result<void> non-regression: trivial-POD storage must keep working ----

SLUICE_TEST_CASE(result_void_assign_transfers_state) {
    sluice::Result<void> ok;
    sluice::Result<void> err =
        sluice::make_unexpected<void>(sluice::IoError{sluice::IoError::Code::eof});
    SLUICE_CHECK(ok.has_value());
    SLUICE_CHECK(!err.has_value());
    ok = err;
    SLUICE_CHECK(!ok.has_value());
    SLUICE_CHECK(ok.error().code == sluice::IoError::Code::eof);

    err = sluice::Result<void>{};
    SLUICE_CHECK(err.has_value());
}

SLUICE_MAIN()
