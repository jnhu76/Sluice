// Divergent-macro-view negative probe: this TU is compiled WITHOUT
// SLUICE_HAS_LIBURING (the verify script supplies the define only to
// negative_odr_view_b.cpp).
#include <sluice/async/uring_backend.hpp>

#include <cstddef>

std::size_t odr_backend_bytes_without_uring_view() {
    return sizeof(sluice::async::UringAsyncBackend);
}
