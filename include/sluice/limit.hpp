










#pragma once

#include <cstdint>

namespace sluice {

class CopyLimit {
  public:
    enum class Kind {
        unlimited,
        limited,
    };

    static constexpr CopyLimit unlimited() {
        CopyLimit l;
        l.kind_ = Kind::unlimited;
        l.remaining_ = 0;
        return l;
    }

    static constexpr CopyLimit bytes(std::uint64_t n) {
        CopyLimit l;
        l.kind_ = Kind::limited;
        l.remaining_ = n;
        return l;
    }

    static constexpr CopyLimit nothing() { return bytes(0); }

    constexpr Kind kind() const noexcept { return kind_; }
    constexpr bool is_unlimited() const noexcept { return kind_ == Kind::unlimited; }
    constexpr bool is_limited() const noexcept { return kind_ == Kind::limited; }

    constexpr std::uint64_t remaining() const noexcept { return remaining_; }

  private:




    constexpr CopyLimit() = default;

    Kind kind_ = Kind::limited;
    std::uint64_t remaining_ = 0;
};

}
