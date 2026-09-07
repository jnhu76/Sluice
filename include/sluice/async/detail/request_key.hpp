


























#pragma once

#include <cstdint>

namespace sluice::async::detail {







struct ContextIdentity {
    std::uint64_t value;

    static ContextIdentity for_testing(std::uint64_t v) noexcept { return {v}; }

    friend bool operator==(const ContextIdentity&, const ContextIdentity&) noexcept = default;
};

struct SlotIndex {
    std::uint32_t value = 0;
    friend bool operator==(const SlotIndex&, const SlotIndex&) noexcept = default;
};

struct Generation {
    std::uint64_t value = 0;
    friend bool operator==(const Generation&, const Generation&) noexcept = default;
};

struct RequestKey {
    ContextIdentity context;
    SlotIndex slot;
    Generation generation;

    friend bool operator==(const RequestKey&, const RequestKey&) noexcept = default;
};

}
