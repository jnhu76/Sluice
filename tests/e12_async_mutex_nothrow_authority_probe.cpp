// e12_async_mutex_nothrow_authority_probe
//
// Compile-time + run-time probe for the ASYNC-MUTEX-NOTHROW contract
// (ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1 §I1).
//
// Deliberately a SEPARATE probe TU, not a block in the installed
// include/sluice/async/mutex.hpp: verification evidence does not belong in the
// public installed header (it would add <utility>/<type_traits> includes,
// repeat in every downstream TU, and drag test intent into the public surface).
//
// These static_asserts do NOT replace the death tests — they only verify the
// function-type contract; the death tests (e12_async_mutex_death_test) verify
// the runtime fail-fast behavior.
#include <sluice/async/mutex.hpp>

#include <type_traits>
#include <utility>

// noexcept is part of the function type (authority §G): these assert both the
// noexcept-expression of the call and the is_nothrow_invocable trait, which is
// what downstream templates (std::unique_lock, std::lock_guard,
// std::condition_variable_any, std::scoped_lock) query.
static_assert(noexcept(std::declval<sluice::async::Mutex&>().lock()));
static_assert(noexcept(std::declval<sluice::async::Mutex&>().try_lock()));
static_assert(noexcept(std::declval<sluice::async::Mutex&>().unlock()));

static_assert(std::is_nothrow_invocable_v<decltype(&sluice::async::Mutex::lock),
                                          sluice::async::Mutex&>);
static_assert(std::is_nothrow_invocable_v<decltype(&sluice::async::Mutex::try_lock),
                                          sluice::async::Mutex&>);
static_assert(std::is_nothrow_invocable_v<decltype(&sluice::async::Mutex::unlock),
                                          sluice::async::Mutex&>);

// Concept compatibility: Mutex remains BasicLockable + Lockable so that
// std::lock_guard<Mutex>, std::unique_lock<Mutex>, and
// std::condition_variable_any still compile against it.
static_assert(std::is_nothrow_default_constructible_v<sluice::async::Mutex>);

int main() {
    sluice::async::Mutex m;
    // A trivial runtime instantiation so the binary is runnable (the
    // static_asserts are the real gate; this just makes xmake run meaningful).
    m.lock();
    m.unlock();
    (void)m.try_lock();
    m.unlock();
    return 0;
}
