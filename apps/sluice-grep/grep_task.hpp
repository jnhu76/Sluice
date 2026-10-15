#pragma once

#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sluice_grep {

constexpr std::size_t kMinBufferSize = 4 * 1024;
constexpr std::size_t kMaxBufferSize = 64 * 1024 * 1024;
constexpr std::size_t kDefaultMaxLineBytes = 1 << 20;
constexpr std::size_t kMaxMaxLineBytes = 64 * 1024 * 1024;

constexpr bool valid_config(std::size_t buffer_size, std::size_t max_line_bytes) noexcept {
    return buffer_size >= kMinBufferSize && buffer_size <= kMaxBufferSize && max_line_bytes > 0 &&
           max_line_bytes <= kMaxMaxLineBytes;
}

struct GrepInput {
    std::string path;
    sluice::File file;
};

using MatchSink =
    std::function<void(const std::string& path, std::uint64_t line_no, std::string_view line)>;

struct GrepFileResult {
    std::string path;
    std::optional<sluice::IoError> error;
    std::uint64_t match_count = 0;
    std::uint64_t lines_scanned = 0;
    bool dropped_long_lines = false;
};

std::vector<GrepFileResult> grep_files(const std::string& pattern, std::vector<GrepInput> inputs,
                                       std::size_t buffer_size, std::size_t max_line_bytes,
                                       MatchSink sink);

}
