// Divergent-macro-view negative probe: the verify script compiles this TU
// WITH -DSLUICE_HAS_LIBURING while negative_odr_view_a.cpp goes without it.
#include <sluice/async/uring_backend.hpp>

#include <cstddef>

std::size_t odr_backend_bytes_with_uring_view() {
    return sizeof(sluice::async::UringAsyncBackend);
}
