#include <sluice/async/detail/fail_fast.hpp>

#include <cstdio>
#include <exception>

namespace sluice::async::detail {

[[noreturn]] void async_context_outstanding_fail_fast() noexcept {
    std::fprintf(stderr,
                 "sluice::async::AsyncIoContext: relocating or destroying a context with "
                 "outstanding public bindings (live Requests) is a contract violation\n");
    std::fflush(stderr);
    std::terminate();
}

[[noreturn]] void async_context_settlement_fail_fast() noexcept {
    std::fprintf(stderr,
                 "sluice::async::AsyncIoContext: destruction-time settlement could not "
                 "establish safe internal retirement; refusing to free storage over "
                 "unresolved obligations\n");
    std::fflush(stderr);
    std::terminate();
}

[[noreturn]] void async_context_progress_binding_fail_fast() noexcept {
    std::fprintf(stderr,
                 "sluice::async::AsyncIoContext: relocating or destroying a context with a "
                 "live progress binding (progress-owner handle, active drive, or live "
                 "external notification registration) is a contract violation\n");
    std::fflush(stderr);
    std::terminate();
}

[[noreturn]] void context_identity_exhausted_fail_fast() noexcept {
    std::terminate();
}



[[noreturn]] void request_scope_settlement_driver_fail_fast() noexcept {
    std::fprintf(stderr,
                 "sluice::async::RequestScope: the owned driver rejected a settlement "
                 "pass (contract violation or unreachable wait outcome)\n");
    std::fflush(stderr);
    std::terminate();
}

[[noreturn]] void request_scope_settlement_health_fail_fast() noexcept {
    std::fprintf(stderr,
                 "sluice::async::RequestScope: progress health failed while accepted "
                 "scope work remains unsettled; failing fast instead of returning with "
                 "live borrows\n");
    std::fflush(stderr);
    std::terminate();
}

[[noreturn]] void threadpool_non_quiescent_destruction_fail_fast() noexcept {
    std::terminate();
}

[[noreturn]] void threadpool_core_handoff_fail_fast() noexcept {
    std::terminate();
}

[[noreturn]] void uring_non_quiescent_destruction_fail_fast() noexcept {
    std::terminate();
}

[[noreturn]] void uring_core_handoff_fail_fast() noexcept {
    std::fprintf(stderr, "sluice::async::UringAsyncBackend: core handoff reached a "
                         "protocol-unreachable state (invariant violation)\n");
    std::fflush(stderr);
    std::terminate();
}

}
