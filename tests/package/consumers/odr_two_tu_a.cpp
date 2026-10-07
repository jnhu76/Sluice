// Clean-room ODR discriminator, TU-A (main). Both TUs observe the SAME
// template specializations (Result<std::size_t>, Request<std::size_t>) and
// the layout of the macro-sensitive UringAsyncBackend; the linked binary
// fails unless both TUs report identical facts under the shared macro view
// supplied by the verify script.
#include <sluice/async/request.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdio>

void odr_b_facts(std::size_t* facts, unsigned* view);

namespace {

unsigned macro_view() {
#if defined(SLUICE_HAS_LIBURING)
    return 1;
#else
    return 0;
#endif
}

} // namespace

int main() {
    int failures = 0;

    std::size_t local[4] = {};
    std::size_t remote[4] = {};
    const unsigned view_local = macro_view();
    unsigned view_remote = 99;

    local[0] = sizeof(sluice::async::UringAsyncBackend);
    local[1] = alignof(sluice::async::UringAsyncBackend);

    sluice::Result<std::size_t> ok{std::size_t{7}};
    sluice::Result<std::size_t> err = sluice::make_unexpected<std::size_t>(
        sluice::IoError{sluice::IoError::Code::not_found});
    local[2] = sizeof(sluice::Result<std::size_t>);

    sluice::async::Request<std::size_t> request;
    local[3] = sizeof(sluice::async::Request<std::size_t>);

    odr_b_facts(remote, &view_remote);

    if (view_local != view_remote)
        ++failures;
    for (std::size_t i = 0; i < 4; ++i) {
        if (local[i] != remote[i])
            ++failures;
    }

    if (!ok.has_value() || ok.value() != 7)
        ++failures;
    if (err.has_value() || err.error().code != sluice::IoError::Code::not_found)
        ++failures;
    if (request.valid() || request.ready())
        ++failures;
    const auto observation = request.try_result();
    if (observation.readiness != sluice::async::RequestReadiness::empty)
        ++failures;

    if (failures != 0) {
        std::fprintf(stderr, "odr_two_tu: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("odr_two_tu: PASS odr_backend_bytes=%zu macro_view=%u\n",
                local[0], view_local);
    return 0;
}
