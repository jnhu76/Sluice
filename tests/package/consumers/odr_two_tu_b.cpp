// Clean-room ODR probe, TU-B. Instantiates the same public templates for a
// different T; identical macro/config view is mandatory (SLUICE_HAS_LIBURING
// ODR rule, DAG X-12).
#include <sluice/async/request.hpp>
#include <sluice/file_resource.hpp>

#include <cstddef>

std::size_t odr_b_result_size_bytes() {
    sluice::Result<sluice::FileInfo> info{sluice::FileInfo{}};
    return info.has_value() ? sizeof(sluice::FileInfo) : 0u;
}

bool odr_b_request_ready_shape(bool) {
    sluice::async::Request<sluice::FileInfo> request;
    if (request.valid())
        return false;
    const auto observation = request.try_result();
    return observation.readiness == sluice::async::RequestReadiness::empty;
}
