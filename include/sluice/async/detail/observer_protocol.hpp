#pragma once

#include <cstdint>

namespace sluice::async::detail {

enum class ObserverPhase : std::uint8_t {
    unattached,
    armed,
    queued,
    delivering,
    retired,
};

enum class ObserverRegistration : std::uint8_t {
    armed,
    duplicate,
    already_terminal,
    not_found,
};

enum class ObserverDeliveryClaim : std::uint8_t {
    claimed,
    none,
};

enum class ObserverCancellation : std::uint8_t {
    retired,
    delivery_in_progress,
    not_registered,
    not_found,
};

enum class ObserverDeliveryRetirement : std::uint8_t {
    retired,
    not_registered,
    not_found,
};

}
