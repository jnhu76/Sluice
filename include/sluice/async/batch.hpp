#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace sluice::async {

struct BatchOp {
    ReadOp read{};
    WriteOp write{};
    SyncDataOp sync_data{};
    SyncAllOp sync_all{};
    enum class Kind : std::uint8_t { read, write, sync_data, sync_all } kind = Kind::read;
};

enum class BatchResultOrigin : std::uint8_t {
    rejected,
    accepted_and_completed,
};

struct BatchResult {
    std::size_t index = 0;
    BatchResultOrigin origin = BatchResultOrigin::accepted_and_completed;
    bool is_void = false;
    std::optional<Result<std::size_t>> size_res;
    std::optional<Result<void>> void_res;
};

class Batch {
  public:
    Batch() = default;

    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    Batch(Batch&&) = delete;
    Batch& operator=(Batch&&) = delete;

    std::size_t add(BatchOp op);

    Result<std::size_t> await_one(AsyncIoContext& ctx);

    std::optional<BatchResult> next() noexcept;

    std::size_t pending_count() const noexcept { return slots_.size() - popped_; }

  private:
    struct Slot {
        BatchOp op;
        bool submitted = false;
        bool is_void = false;
        Completion<std::size_t> size_c;
        Completion<void> void_c;
        bool ready = false;
        bool popped = false;

        bool submit_rejected = false;
        std::optional<Result<std::size_t>> size_res{};
        std::optional<Result<void>> void_res{};
    };

    std::vector<std::unique_ptr<Slot>> slots_;
    std::size_t popped_ = 0;
};

} // namespace sluice::async
