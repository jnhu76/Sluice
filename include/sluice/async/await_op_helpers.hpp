












































#pragma once

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sluice::async {





struct AwaitOpTally {
    std::uint64_t ops = 0;
    std::uint64_t short_ops = 0;
};





Result<std::size_t> await_take(RuntimeTaskContext& ctx,
                               Completion<std::size_t>& c);
Result<void> await_take(RuntimeTaskContext& ctx, Completion<void>& c);







Result<void> await_drain(RuntimeTaskContext& ctx, Completion<std::size_t>& c);



Result<std::size_t> await_read_once(RuntimeTaskContext& ctx, int fd,
                                    std::span<std::byte> dst,
                                    std::uint64_t offset,
                                    Completion<std::size_t>& c);





Result<std::size_t> await_read_fill(RuntimeTaskContext& ctx, int fd,
                                    std::span<std::byte> dst,
                                    std::uint64_t offset,
                                    Completion<std::size_t>& c,
                                    AwaitOpTally* tally = nullptr);




Result<std::size_t> await_write_exact(RuntimeTaskContext& ctx, int fd,
                                      std::span<const std::byte> src,
                                      std::uint64_t offset,
                                      Completion<std::size_t>& c,
                                      AwaitOpTally* tally = nullptr);

}
