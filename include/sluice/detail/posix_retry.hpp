// Internal POSIX syscall retry helpers. Not part of the public API.
#pragma once

#include <cerrno>

namespace sluice::detail {

// Calls `fn()` repeatedly, retrying only when it returns a negative value AND
// errno == EINTR (the classic interrupted-syscall case for blocking I/O). Any
// other return value (success or a real error) is returned immediately.
//
// `fn` must return a signed integer-like value where < 0 means error, and must
// set errno on failure (matching ::read/::write semantics).
template <class Fn>
auto retry_on_eintr(Fn&& fn) -> decltype(fn()) {
    for (;;) {
        auto result = fn();
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result;
    }
}

}  // namespace sluice::detail
