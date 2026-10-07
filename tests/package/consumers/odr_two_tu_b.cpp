// Clean-room ODR discriminator, TU-B. Computes the same facts as TU-A from
// the SAME specializations (Result<std::size_t>, Request<std::size_t>) and
// the same macro-sensitive UringAsyncBackend layout, in this TU, under the
// verifier-supplied macro view.
#include <sluice/async/request.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/result.hpp>

#include <cstddef>

void odr_b_facts(std::size_t* facts, unsigned* view) {
    facts[0] = sizeof(sluice::async::UringAsyncBackend);
    facts[1] = alignof(sluice::async::UringAsyncBackend);

    sluice::Result<std::size_t> ok{std::size_t{7}};
    (void)ok.has_value();
    sluice::async::Request<std::size_t> request;
    (void)request.try_result();
    facts[2] = sizeof(sluice::Result<std::size_t>);
    facts[3] = sizeof(sluice::async::Request<std::size_t>);

    *view =
#if defined(SLUICE_HAS_LIBURING)
        1;
#else
        0;
#endif
}
