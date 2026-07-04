// sluice::CopyLimit — a tiny value-like byte-count bound for copy/stream ops.
//
// Modeled after Zig std.Io's `Limit` concept (Io.zig), which is a
// sentinel-controlled enum of `.nothing` / `.unlimited` / `.limited(n)` used to
// bound `Reader.stream` and friends. This is a C++-friendly equivalent: an
// explicit Kind tag plus a remaining byte count, all constexpr-constructible.
//
// Semantics:
//   unlimited()   copy until EOF or error
//   bytes(n)      copy at most n bytes; EOF before n is success
//   nothing()     == bytes(0); success with 0 copied, never touches reader/writer
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
        l.remaining_ = 0; // not meaningful in unlimited mode
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
    // Only meaningful when is_limited(); returns 0 for nothing().
    constexpr std::uint64_t remaining() const noexcept { return remaining_; }

  private:
    // Private default ctor: the factories construct a default instance then
    // set its fields. A public default ctor would let callers write
    // `CopyLimit c;` and silently get nothing()-equivalent semantics, which is
    // a footgun — so only the class itself (and its static factories) may use it.
    constexpr CopyLimit() = default;

    Kind kind_ = Kind::limited;
    std::uint64_t remaining_ = 0;
};

} // namespace sluice
