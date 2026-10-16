// Clean-room ODR discriminator, TU-A (main). Two TUs observe the same
// template specializations (Result<std::size_t>, Request<std::size_t>) and
// the layouts of every public vocabulary type; the verifier compiles TU-B
// with the OPPOSITE SLUICE_HAS_LIBURING macro view, and the linked binary
// fails unless both TUs report identical facts: the installed v2 surface is
// macro-view independent, so no consumer TU can silently diverge from the
// archive's view.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdio>

void odr_b_facts(std::size_t* facts, unsigned* view);

namespace {

constexpr std::size_t kFacts = 12;

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

    std::size_t local[kFacts] = {};
    std::size_t remote[kFacts] = {};
    const unsigned view_local = macro_view();
    unsigned view_remote = 99;

    local[0] = sizeof(sluice::async::AsyncIoContext);
    local[1] = alignof(sluice::async::AsyncIoContext);
    local[2] = sizeof(sluice::async::RequestId);
    local[3] = alignof(sluice::async::RequestId);
    local[4] = sizeof(sluice::async::Request<std::size_t>);

    sluice::Result<std::size_t> ok{std::size_t{7}};
    sluice::Result<std::size_t> err = sluice::make_unexpected<std::size_t>(
        sluice::IoError{sluice::IoError::Code::not_found});
    local[5] = sizeof(sluice::Result<std::size_t>);

    local[6] = sizeof(sluice::async::RequestResultData);
    local[7] = sizeof(sluice::async::ProgressPass);
    local[8] = sizeof(sluice::async::UringConfig);
    local[9] = sizeof(sluice::async::ThreadPoolConfig);
    local[10] = sizeof(sluice::async::AsyncIoContext::ObserverAttachment);
    local[11] = sizeof(sluice::async::AsyncIoContext::ObserverCancelResult);

    odr_b_facts(remote, &view_remote);

    if (view_local == view_remote)
        ++failures; // the verifier must compile TU-B with the opposite view
    for (std::size_t i = 0; i < kFacts; ++i) {
        if (local[i] != remote[i])
            ++failures;
    }

    if (!ok.has_value() || ok.value() != 7)
        ++failures;
    if (err.has_value() || err.error().code != sluice::IoError::Code::not_found)
        ++failures;
    sluice::async::Request<std::size_t> request;
    if (request.valid() || request.ready())
        ++failures;
    const auto observation = request.try_result();
    if (observation.readiness != sluice::async::RequestReadiness::empty)
        ++failures;

    if (failures != 0) {
        std::fprintf(stderr, "odr_two_tu: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("odr_two_tu: PASS context_bytes=%zu request_bytes=%zu "
                "macro_views=%u/%u\n",
                local[0], local[4], view_local, view_remote);
    return 0;
}
