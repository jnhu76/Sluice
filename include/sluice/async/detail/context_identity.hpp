#pragma once

#include <sluice/async/detail/request_key.hpp>

namespace sluice::async::detail {

ContextIdentity allocate_context_identity() noexcept;

}
