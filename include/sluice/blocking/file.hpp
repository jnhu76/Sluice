#pragma once

#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sluice::blocking {

Result<std::size_t> read_at(const File& file, std::uint64_t offset, std::span<std::byte> dst);

Result<std::size_t> write_at(const File& file, std::uint64_t offset,
                             std::span<const std::byte> src);

Result<std::size_t> read(const File& file, std::span<std::byte> dst);

Result<std::size_t> write(const File& file, std::span<const std::byte> src);

Result<void> sync_data(const File& file);

Result<void> sync_all(const File& file);

Result<FileInfo> file_info(const File& file);

Result<std::uint64_t> size(const File& file);

Result<void> resize(const File& file, std::uint64_t new_size);

enum class CompositionEnd : std::uint8_t {
    complete,
    eof_before_full,
    write_no_progress,
    primitive_error,
};

enum class EffectCertainty : std::uint8_t {
    accounted,
    unknown,
};

struct CompositionOutcome {
    std::size_t confirmed_bytes = 0;
    CompositionEnd end = CompositionEnd::complete;
    EffectCertainty remaining = EffectCertainty::accounted;
    std::optional<IoError> error;

    constexpr bool complete() const noexcept { return end == CompositionEnd::complete; }
};

Result<CompositionOutcome> read_exact_at(const File& file, std::uint64_t offset,
                                         std::span<std::byte> dst);

Result<CompositionOutcome> write_all_at(const File& file, std::uint64_t offset,
                                        std::span<const std::byte> src);

Result<CompositionOutcome> read_exact(const File& file, std::span<std::byte> dst);

Result<CompositionOutcome> write_all(const File& file, std::span<const std::byte> src);

}
