

#pragma once

#include <sluice/copy_strategy.hpp>
#include <sluice/limit.hpp>
#include <sluice/measurement.hpp>
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sluice {







Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch,
                               CopyOptions options, CopyStats* stats = nullptr,
                               CopyDecision* decision = nullptr);





Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch,
                               CopyLimit limit, CopyStats* stats = nullptr);


Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch);


Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, CopyLimit limit);


Result<std::uint64_t> copy_all(Reader& reader, Writer& writer);

}
