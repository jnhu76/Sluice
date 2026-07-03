// Compile-time test: the six wrapper types are non-copyable AND non-movable.
// If any wrapper accidentally becomes copyable/movable (e.g. via a forgotten
// `= delete`), this file fails to compile. There is no runtime behavior.
#include "harness.hpp"

#include <sluice/buffer.hpp>
#include <sluice/fault.hpp>
#include <sluice/observed.hpp>

#include <concepts>
#include <type_traits>

// Each wrapper holds a reference to an inner Reader/Writer plus mutable cursor
// or counter state. Allowing copy/move would alias that shared state.
// C++20 has std::copy_constructible/move_constructible concepts; assignability
// uses the type_traits _v traits (no concept until C++20 didn't add assign ones).
#define SLUICE_ASSERT_NONCOPYABLE(Type)                           \
    static_assert(!std::copy_constructible<Type>);               \
    static_assert(!std::is_copy_assignable_v<Type>);             \
    static_assert(!std::move_constructible<Type>);               \
    static_assert(!std::is_move_assignable_v<Type>)

SLUICE_ASSERT_NONCOPYABLE(sluice::BufferedReader);
SLUICE_ASSERT_NONCOPYABLE(sluice::BufferedWriter);
SLUICE_ASSERT_NONCOPYABLE(sluice::FaultReader);
SLUICE_ASSERT_NONCOPYABLE(sluice::FaultWriter);
SLUICE_ASSERT_NONCOPYABLE(sluice::ObservedReader);
SLUICE_ASSERT_NONCOPYABLE(sluice::ObservedWriter);

// MemoryReader/MemoryWriter stay copyable (they own their buffer): a quick
// guard that we did NOT accidentally delete their copy semantics.
static_assert(std::copy_constructible<sluice::MemoryReader>);
static_assert(std::copy_constructible<sluice::MemoryWriter>);

SLUICE_MAIN()
