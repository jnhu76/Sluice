// Clean-room ODR discriminator, TU-B. Computes the same public-layout facts
// as TU-A from the SAME specializations (Result<std::size_t>,
// Request<std::size_t>) in this TU. The verifier compiles this TU with the
// OPPOSITE SLUICE_HAS_LIBURING macro view; agreement is the proof that the
// installed surface carries no macro-sensitive layout.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/result.hpp>

#include <cstddef>

void odr_b_facts(std::size_t* facts, unsigned* view) {
    facts[0] = sizeof(sluice::async::AsyncIoContext);
    facts[1] = alignof(sluice::async::AsyncIoContext);
    facts[2] = sizeof(sluice::async::RequestId);
    facts[3] = alignof(sluice::async::RequestId);
    facts[4] = sizeof(sluice::async::Request<std::size_t>);

    sluice::Result<std::size_t> ok{std::size_t{7}};
    (void)ok.has_value();
    facts[5] = sizeof(sluice::Result<std::size_t>);

    facts[6] = sizeof(sluice::async::RequestResultData);
    facts[7] = sizeof(sluice::async::ProgressPass);
    facts[8] = sizeof(sluice::async::UringConfig);
    facts[9] = sizeof(sluice::async::ThreadPoolConfig);
    facts[10] = sizeof(sluice::async::AsyncIoContext::ObserverAttachment);
    facts[11] = sizeof(sluice::async::AsyncIoContext::ObserverCancelResult);

    *view =
#if defined(SLUICE_HAS_LIBURING)
        1;
#else
        0;
#endif
}
