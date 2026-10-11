#pragma once

#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sluice_hash {

constexpr std::size_t kMinBufferSize = 4 * 1024;
constexpr std::size_t kMaxBufferSize = 64 * 1024 * 1024;
constexpr bool valid_config(std::size_t buffer_size) noexcept {
    return buffer_size >= kMinBufferSize && buffer_size <= kMaxBufferSize;
}

struct HashInput {
    std::string path;
    sluice::File file;
};

struct FileHash {
    std::string path;
    std::string hex;
    std::uint64_t bytes_hashed = 0;
    std::optional<sluice::IoError> error;
};

std::vector<FileHash> hash_files(std::vector<HashInput> inputs, std::size_t buffer_size);

} // namespace sluice_hash
