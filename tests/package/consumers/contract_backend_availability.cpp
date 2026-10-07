// Clean-room micro-contract probe: backend availability. Named construction
// must succeed or fail explicitly; selecting an unavailable backend never
// falls back silently. Built against the installed prefix; the macro view
// (SLUICE_HAS_LIBURING) is supplied by the verify script from the manifest so
// it matches the archive the consumer links.
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>

#include <cstdio>
#include <type_traits>
#include <cstring>
#include <memory>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

} // namespace

int main() {
    using namespace sluice::async;

    // Named ThreadPool construction: the required request profile.
    {
        auto backend = std::make_unique<ThreadPoolBackend>(
            ThreadPoolConfig{.request_capacity = 4, .worker_count = 1});
        CHECK(backend->supports_request_identity());
        CHECK(backend->signals_physical_progress());
        CHECK(backend->slot_capacity() == 4);
    }

    // Named io_uring selection. Without the liburing profile the installed
    // class shell is abstract from any external TU: the named backend cannot
    // be constructed at all, which is the explicit unavailability outcome —
    // there is no silent fallback path to trip over. With the profile,
    // construction either succeeds or fails explicitly (runtime setup error).
#if defined(SLUICE_HAS_LIBURING)
    bool uring_constructed = false;
    try {
        auto backend = std::make_unique<UringAsyncBackend>(8u);
        uring_constructed = true;
    } catch (const std::exception& error) {
        CHECK(std::strstr(error.what(), "unavailable") != nullptr ||
              std::strstr(error.what(), "io_uring") != nullptr);
    }
    if (!uring_constructed)
        std::printf("contract_backend_availability: uring runtime-unavailable (explicit)\n");
    else
        std::printf("contract_backend_availability: uring constructed\n");
#else
    static_assert(std::is_abstract_v<UringAsyncBackend>,
                  "without SLUICE_HAS_LIBURING the named io_uring backend must not be "
                  "constructible from an external consumer");
    std::printf("contract_backend_availability: uring profile absent (abstract shell)\n");
#endif

    if (g_failures != 0) {
        std::fprintf(stderr, "contract_backend_availability: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("contract_backend_availability: PASS\n");
    return 0;
}
