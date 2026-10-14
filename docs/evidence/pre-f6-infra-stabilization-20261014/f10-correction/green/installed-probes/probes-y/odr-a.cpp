#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/stackful_io_host.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/detail/uring_submit.hpp>
#include <sluice/file_resource.hpp>

#include <string>
#include <typeinfo>

namespace {

sluice::async::Request<std::size_t> request_instantiation() { return {}; }

sluice::Result<std::size_t> result_instantiation() {
    return sluice::Result<std::size_t>(std::size_t{0});
}

} // namespace

extern "C" const char* odr_description_a() {
    static const std::string text =
        std::string("RequestId=") + typeid(sluice::async::RequestId).name() +
        ",ready=" + std::to_string(request_instantiation().valid()) +
        ",result=" + std::to_string(result_instantiation().has_value());
    return text.c_str();
}

extern "C" const void* odr_chunk_entry_a() {
    return reinterpret_cast<const void*>(&sluice::detail::uring_chunk_length);
}

extern "C" const void* odr_unexpected_entry_a() {
    return reinterpret_cast<const void*>(&sluice::make_unexpected_void);
}
