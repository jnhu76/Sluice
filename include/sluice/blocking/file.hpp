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

Result<void> sync_data(const File& file);

Result<std::uint64_t> size(const File& file);

Result<void> resize(const File& file, std::uint64_t new_size);

} // namespace sluice::blocking
