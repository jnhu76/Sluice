#pragma once

#include <cstdint>

namespace sluice {

enum class EffectCertainty : std::uint8_t {
    accounted,
    unknown,
};

struct EffectReport {
    std::uint64_t confirmed_bytes = 0;
    EffectCertainty remaining = EffectCertainty::accounted;

    friend bool operator==(const EffectReport&, const EffectReport&) noexcept = default;
};

}
