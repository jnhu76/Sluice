// Compile-time test: the six wrapper types are non-copyable AND non-movable.
// If any wrapper accidentally becomes copyable/movable (e.g. via a forgotten
// `= delete`), this file fails to compile. There is no runtime behavior.
#include "harness.hpp"

#include <cppio/buffer.hpp>
#include <cppio/fault.hpp>
#include <cppio/observed.hpp>

#include <concepts>
#include <type_traits>

// Each wrapper holds a reference to an inner Reader/Writer plus mutable cursor
// or counter state. Allowing copy/move would alias that shared state.
// C++20 has std::copy_constructible/move_constructible concepts; assignability
// uses the type_traits _v traits (no concept until C++20 didn't add assign ones).
#define CPPIO_ASSERT_NONCOPYABLE(Type)                           \
    static_assert(!std::copy_constructible<Type>);               \
    static_assert(!std::is_copy_assignable_v<Type>);             \
    static_assert(!std::move_constructible<Type>);               \
    static_assert(!std::is_move_assignable_v<Type>)

CPPIO_ASSERT_NONCOPYABLE(cppio::BufferedReader);
CPPIO_ASSERT_NONCOPYABLE(cppio::BufferedWriter);
CPPIO_ASSERT_NONCOPYABLE(cppio::FaultReader);
CPPIO_ASSERT_NONCOPYABLE(cppio::FaultWriter);
CPPIO_ASSERT_NONCOPYABLE(cppio::ObservedReader);
CPPIO_ASSERT_NONCOPYABLE(cppio::ObservedWriter);

// MemoryReader/MemoryWriter stay copyable (they own their buffer): a quick
// guard that we did NOT accidentally delete their copy semantics.
static_assert(std::copy_constructible<cppio::MemoryReader>);
static_assert(std::copy_constructible<cppio::MemoryWriter>);

CPPIO_MAIN()
