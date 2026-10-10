#pragma once

#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sluice::async {
class AsyncBackend;
}

namespace sluice_hash {

constexpr std::size_t kMinBufferSize = 4 * 1024;
constexpr std::size_t kMaxBufferSize = 64 * 1024 * 1024;
constexpr unsigned kMaxWorkers = 64;

constexpr bool valid_config(std::size_t buffer_size, unsigned workers) noexcept {
    return buffer_size >= kMinBufferSize && buffer_size <= kMaxBufferSize && workers > 0 &&
           workers <= kMaxWorkers;
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

std::vector<FileHash> hash_files(std::vector<HashInput> inputs, std::size_t buffer_size,
                                 unsigned workers);

std::vector<FileHash> hash_files_with_backend(std::vector<HashInput> inputs,
                                              std::size_t buffer_size, unsigned workers,
                                              std::unique_ptr<sluice::async::AsyncBackend> backend);

}
