// sluice::async::select_fwd — E13 Select forward declarations.
//
// Narrow forward-declaration header that provides the public select() template
// declaration and its prerequisite forward declarations/types. Included by
// scheduler.hpp (for the friend grant) and select.hpp (for the definition).
//
// This header is deliberately NARROW: it only forward-declares the types the
// template signature requires. The full definitions live in select.hpp.
//
// By friending this pre-declared, constrained function-template entity (rather
// than a concrete friend struct like SelectBridge), no ordinary production TU
// can forge the friend grant: the template entity is uniquely identified by
// its template-head + name + signature, and only the single definition in
// select.hpp matches that entity. A TU that tries to define its own version
// of the template would violate ODR (the definition would conflict with the
// select.hpp definition in a well-formed program); this is a type-system
// guard under the well-formed C++ consumer model, not a security boundary.
#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace sluice::async {

class Scheduler;
class SelectResult;
class EventSelectCase;
class TimerSelectCase;

inline constexpr std::size_t kSelectMaxArms = 8;

template <class T>
concept SelectCaseType =
    std::same_as<std::remove_cvref_t<T>, EventSelectCase> ||
    std::same_as<std::remove_cvref_t<T>, TimerSelectCase>;

template <class... Cases>
    requires (
        sizeof...(Cases) >= 1 &&
        sizeof...(Cases) <= kSelectMaxArms &&
        (SelectCaseType<Cases> && ...)
    )
SelectResult select(Scheduler& scheduler, Cases&&... cases);

}  // namespace sluice::async
