// Clean-room ODR probe, TU-A (main). Two translation units instantiate the
// public templates with the same macro/config view supplied by the verify
// script; the linked binary must behave consistently under one config.
#include <sluice/async/request.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdio>

// TU-B instantiates the same templates for FileInfo and reports its view.
std::size_t odr_b_result_size_bytes();
bool odr_b_request_ready_shape(bool);

int main() {
    int failures = 0;

    sluice::Result<std::size_t> ok{std::size_t{7}};
    sluice::Result<std::size_t> err = sluice::make_unexpected<std::size_t>(
        sluice::IoError{sluice::IoError::Code::not_found});
    if (!ok.has_value() || ok.value() != 7)
        ++failures;
    if (err.has_value() || err.error().code != sluice::IoError::Code::not_found)
        ++failures;

    // Request<T> move/empty shape for the TU-A instantiation.
    sluice::async::Request<std::size_t> request;
    if (request.valid() || request.ready())
        ++failures;
    const auto observation = request.try_result();
    if (observation.readiness != sluice::async::RequestReadiness::empty)
        ++failures;

    if (odr_b_result_size_bytes() != sizeof(sluice::FileInfo))
        ++failures;
    if (!odr_b_request_ready_shape(false))
        ++failures;

    if (failures != 0) {
        std::fprintf(stderr, "odr_two_tu: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("odr_two_tu: PASS\n");
    return 0;
}
