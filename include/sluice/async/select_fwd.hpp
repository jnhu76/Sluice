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
concept SelectCaseType = std::same_as<std::remove_cvref_t<T>, EventSelectCase> ||
                         std::same_as<std::remove_cvref_t<T>, TimerSelectCase>;

template <class... Cases>
    requires(sizeof...(Cases) >= 1 && sizeof...(Cases) <= kSelectMaxArms &&
             (SelectCaseType<Cases> && ...))
SelectResult select(Scheduler& scheduler, Cases&&... cases);

} // namespace sluice::async
