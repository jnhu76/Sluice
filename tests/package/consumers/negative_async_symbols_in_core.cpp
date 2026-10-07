// Negative probe source: references the async context through the installed
// headers. The verify script compiles this TU and links it against
// libsluice_core ONLY; the link MUST FAIL (async definitions live in
// libsluice_async). A successful link would mean the core archive carries
// async symbols and the package split is broken.
#include <sluice/async/async_io_context.hpp>

void sluice_f1_negative_use_async(sluice::async::AsyncIoContext& ctx) {
    (void)ctx.outstanding();
}
