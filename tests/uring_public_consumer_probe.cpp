// Consumer ODR witness for the uring public class definition. This TU
// declares no build configuration of its own: it compiles only when
// depending on sluice_async delivers the library's public usage requirement
// (SLUICE_HAS_LIBURING + liburing link), which is the same guarded definition
// the library TUs were compiled with.
#include <sluice/async/uring_backend.hpp>

#include <cstdio>
#include <type_traits>

#ifndef SLUICE_HAS_LIBURING
#error "consumer TU must receive SLUICE_HAS_LIBURING through sluice_async's public usage requirement"
#endif

namespace {

using sluice::async::UringAsyncBackend;
using sluice::async::UringConfig;

// The UringConfig constructor exists only inside the guarded public
// definition, so this assertion compiles only when this TU sees exactly that
// definition; a consumer missing the macro fails closed here.
static_assert(std::is_constructible_v<UringAsyncBackend, UringConfig>,
              "consumer TU must see the guarded public class definition");

} // namespace

int main() {
    {
        UringAsyncBackend backend;
        (void)backend.available();
    }
    {
        UringAsyncBackend backend(UringConfig{});
        (void)backend.available();
    }
    std::printf("uring public consumer probe passed: consumer TU constructed and destroyed "
                "the library's public class definition\n");
    return 0;
}
