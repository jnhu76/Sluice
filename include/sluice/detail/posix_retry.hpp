#pragma once

#include <cerrno>

namespace sluice::detail {

template <class Fn> auto retry_on_eintr(Fn&& fn) -> decltype(fn()) {
    for (;;) {
        auto result = fn();
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result;
    }
}

} // namespace sluice::detail
