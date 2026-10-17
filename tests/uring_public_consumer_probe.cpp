// Consumer selection witness for the io_uring backend. This TU declares no
// build configuration of its own: it compiles only when depending on
// sluice_async delivers the library's public usage requirement
// (SLUICE_HAS_LIBURING + liburing link). The public surface is the named
// factory, not a constructible backend class.
#include <sluice/async/uring_backend.hpp>

#include <cstdio>
#include <utility>

#ifndef SLUICE_HAS_LIBURING
#error "consumer TU must receive SLUICE_HAS_LIBURING through sluice_async's public usage requirement"
#endif

int main() {
    sluice::AsyncStats stats;
    auto made = sluice::async::make_uring_context(sluice::async::UringConfig{}, &stats);
    if (!made.has_value()) {
        std::printf("uring public consumer probe failed: explicit selection reported %s\n",
                    sluice::to_string(made.error().code).data());
        return 1;
    }
    sluice::async::AsyncIoContext ctx = std::move(made).value();
    std::printf("uring public consumer probe passed: consumer TU selected io_uring through "
                "the public factory\n");
    return 0;
}
