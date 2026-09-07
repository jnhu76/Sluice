#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sluice::async {

Result<std::size_t> read_all(AsyncIoContext& ctx, int fd, std::span<std::byte> dst,
                             std::uint64_t offset);

Result<std::size_t> write_all(AsyncIoContext& ctx, int fd, std::span<const std::byte> src,
                              std::uint64_t offset);

Result<void> sync_data_all(AsyncIoContext& ctx, int fd);

Result<void> sync_all_all(AsyncIoContext& ctx, int fd);

} // namespace sluice::async
