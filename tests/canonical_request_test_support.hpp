#pragma once

#include <sluice/async/request_scope.hpp>

#include <chrono>

namespace sluice::test {

template <class T>
Result<T> wait_and_take(async::RequestScope& scope, const async::ScopeTicket<T>& ticket) {
    auto waited = scope.wait_for(ticket, std::chrono::seconds(10));
    if (!waited.has_value())
        return make_unexpected<T>(waited.error());
    if (waited.value() != async::ScopeWaitStatus::ready)
        return make_unexpected<T>(IoError{IoError::Code::invalid_state});
    auto observed = scope.take(ticket);
    if (observed.readiness != async::RequestReadiness::ready)
        return make_unexpected<T>(IoError{IoError::Code::invalid_state});
    return observed.result;
}

} // namespace sluice::test
