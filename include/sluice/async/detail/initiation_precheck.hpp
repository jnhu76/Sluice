#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/error.hpp>

#include <optional>

namespace sluice::async::detail {

// Shared by every submit entry; the rejection set must stay identical.
inline std::optional<IoError> initiation_rejection(const NativeFileRef& file,
                                                   sluice::detail::FileOperation op) {
    return sluice::detail::rejection_of(
        sluice::detail::precheck_state_op(file.fd < 0, file.access, op));
}

} // namespace sluice::async::detail
