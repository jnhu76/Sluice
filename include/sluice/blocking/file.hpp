#pragma once

#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sluice::blocking {

Result<std::size_t> read_at(const File& file, std::uint64_t offset,
                            std::span<std::byte> dst);

Result<std::size_t> write_at(const File& file, std::uint64_t offset,
                             std::span<const std::byte> src);

Result<std::size_t> read(const File& file, std::span<std::byte> dst);

Result<std::size_t> write(const File& file, std::span<const std::byte> src);

Result<void> sync_data(const File& file);

} // namespace sluice::blocking
