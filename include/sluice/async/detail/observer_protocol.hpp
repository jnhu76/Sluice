#pragma once

#include <cstdint>

namespace sluice::async::detail {

enum class ObserverRegistration : std::uint8_t {
    armed,
    duplicate,
    already_terminal,
    not_found,
};

enum class ObserverRetirement : std::uint8_t {
    retired,
    not_registered,
    not_found,
};

}
