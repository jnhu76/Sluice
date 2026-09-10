#pragma once

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/file_resource.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sluice::async {

Result<std::size_t> await_read_at(const File& file, RuntimeTaskContext& ctx, std::uint64_t offset,
                                  std::span<std::byte> dst, Completion<std::size_t>& c);

}
