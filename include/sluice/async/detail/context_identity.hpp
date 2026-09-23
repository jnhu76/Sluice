#pragma once

#include <sluice/async/detail/request_key.hpp>

#include <atomic>
#include <cstdint>

namespace sluice::async::detail {

constexpr std::uint64_t kContextIdentityExhausted = 0;

std::uint64_t claim_context_identity(std::atomic<std::uint64_t>& cursor) noexcept;

ContextIdentity allocate_context_identity() noexcept;

}
