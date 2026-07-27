// Deterministic fault-model Reader/Writer for copy_all fuzzing.
//
// These models do NOT exercise production's MemoryReader/MemoryWriter. They are
// minimal, observable endpoints that copy_all() drives, instrumented so the
// fuzz oracle can check invariants (prefix integrity, limit safety, error
// propagation, broken-reader rejection) regardless of copy_all's internal loop
// shape.
//
// Observability the models expose:
//   - every requested read/write size (for the limit-clamp oracle)
//   - total calls and bytes transferred
//   - whether a configured failure was actually injected (so the oracle knows to
//     expect an error vs. a full copy)
//   - whether the broken-reader over-report actually fired
//
// Failure injection is deterministic and total:
//   - AfterCalls(T): the (T+1)-th call fails (T=0 => first call fails).
//   - AfterBytes(T): once T bytes have been transferred, the next call fails.
//   - Disabled: no failure.
// The model records which endpoint injected a failure so the oracle can predict
// the exact error code.
#pragma once

#include <sluice/error.hpp>
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

namespace fuzz {

enum class FailKind { Disabled, AfterCalls, AfterBytes };
enum class LimitKind { Unlimited, Zero, Bounded };

// Which deferred-strategy mode to exercise. The two *Deferred variants pair a
// reserved slot with a policy; the rest are the active strategies.
enum class StrategyKind { Scratch, Auto, BufferedFirst, DeferredReject, DeferredFallback };

struct CopyConfig {
    std::size_t scratch_size = 64;     // 0..64
    LimitKind limit = LimitKind::Unlimited;
    std::uint64_t limit_value = 0;     // used when limit == Bounded
    std::size_t reader_short = 64;     // 1..64
    std::size_t writer_short = 64;     // 1..64
    FailKind reader_fail = FailKind::Disabled;
    std::uint64_t reader_fail_threshold = 0;
    FailKind writer_fail = FailKind::Disabled;
    std::uint64_t writer_fail_threshold = 0;
    sluice::IoError::Code injected = sluice::IoError::Code::canceled;
    StrategyKind strategy = StrategyKind::Auto;
    bool broken_reader = false;
};

// Decode a compact config from the front of the fuzz input. Total: an exhausted
// stream yields defaults (see CopyConfig above). The unconsumed tail is the
// source payload for the copy.
inline CopyConfig decode_config(ByteCursor& cur) {
    CopyConfig cfg;
    cfg.scratch_size = cur.take_range(0, 64);
    cfg.limit = static_cast<LimitKind>(cur.take_mod(3));
    cfg.limit_value = cur.take_u32_le();
    cfg.reader_short = cur.take_range(1, 64);
    cfg.writer_short = cur.take_range(1, 64);

    cfg.reader_fail = static_cast<FailKind>(cur.take_mod(3));
    cfg.reader_fail_threshold = cur.take_u8();
    cfg.writer_fail = static_cast<FailKind>(cur.take_mod(3));
    cfg.writer_fail_threshold = cur.take_u8();

    // Injected error: canceled / no_space / backend_error.
    switch (cur.take_mod(3)) {
    case 0:
        cfg.injected = sluice::IoError::Code::canceled;
        break;
    case 1:
        cfg.injected = sluice::IoError::Code::no_space;
        break;
    default:
        cfg.injected = sluice::IoError::Code::backend_error;
        break;
    }

    cfg.strategy = static_cast<StrategyKind>(cur.take_mod(5));
    cfg.broken_reader = cur.take_mod(2) == 1;
    return cfg;
}

class ModelWriter final : public sluice::Writer {
  public:
    explicit ModelWriter(const CopyConfig& cfg) : cfg_(cfg) {}

    sluice::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        if (!src.empty()) {
            requests_.push_back(src.size());
        }
        ++write_calls_;
        // Failure injection: after N calls. Threshold 0 => first call fails.
        if (cfg_.writer_fail == FailKind::AfterCalls &&
            write_calls_ > cfg_.writer_fail_threshold) {
            injected_ = true;
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{.code = cfg_.injected, .os_errno = 0});
        }
        // Failure injection: after N bytes transferred.
        if (cfg_.writer_fail == FailKind::AfterBytes &&
            bytes_written_ >= cfg_.writer_fail_threshold) {
            injected_ = true;
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{.code = cfg_.injected, .os_errno = 0});
        }
        std::size_t count = std::min(src.size(), cfg_.writer_short);
        if (count > 0) {
            buf_.insert(buf_.end(), src.begin(), src.begin() + count);
        }
        bytes_written_ += count;
        return count;
    }

    sluice::Result<void> flush() override { return {}; }

    const std::vector<std::byte>& bytes() const noexcept { return buf_; }
    std::size_t write_calls() const noexcept { return write_calls_; }
    std::size_t bytes_written() const noexcept { return bytes_written_; }
    bool injected() const noexcept { return injected_; }
    const std::vector<std::size_t>& requests() const noexcept { return requests_; }

  private:
    CopyConfig cfg_;
    std::vector<std::byte> buf_;
    std::size_t write_calls_ = 0;
    std::size_t bytes_written_ = 0;
    bool injected_ = false;
    std::vector<std::size_t> requests_;
};

class ModelReader final : public sluice::Reader {
  public:
    ModelReader(std::span<const std::byte> source, const CopyConfig& cfg)
        : source_(source.begin(), source.end()), cfg_(cfg) {}

    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        if (!dst.empty()) {
            requests_.push_back(dst.size());
            max_request_ = std::max(max_request_, dst.size());
        }
        ++read_calls_;
        std::size_t available = source_.size() - pos_;
        std::size_t count = std::min({available, dst.size(), cfg_.reader_short});
        if (count > 0) {
            // count != 0, so both pointers are non-null.
            std::memcpy(dst.data(), source_.data() + pos_, count);
        }
        pos_ += count;
        bytes_read_ += count;

        // Broken-reader mode: on a FULL read (dst was completely fillable and
        // there was source left), report one more byte than actually copied.
        // This dominates failure injection so the over-report is what copy_all
        // sees. The model never copies beyond dst, so the excess is fictitious.
        if (cfg_.broken_reader && dst.size() > 0 && count == dst.size()) {
            broke_ = true;
            return count + 1;
        }

        // Failure injection: after N calls. Threshold 0 => first call fails.
        if (cfg_.reader_fail == FailKind::AfterCalls &&
            read_calls_ > cfg_.reader_fail_threshold) {
            injected_ = true;
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{.code = cfg_.injected, .os_errno = 0});
        }
        // Failure injection: after N bytes transferred.
        if (cfg_.reader_fail == FailKind::AfterBytes &&
            bytes_read_ >= cfg_.reader_fail_threshold) {
            injected_ = true;
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{.code = cfg_.injected, .os_errno = 0});
        }
        return count;
    }

    std::size_t read_calls() const noexcept { return read_calls_; }
    std::size_t bytes_read() const noexcept { return bytes_read_; }
    std::size_t max_request() const noexcept { return max_request_; }
    bool injected() const noexcept { return injected_; }
    bool broke() const noexcept { return broke_; }
    const std::vector<std::size_t>& requests() const noexcept { return requests_; }

  private:
    std::vector<std::byte> source_;
    CopyConfig cfg_;
    std::size_t pos_ = 0;
    std::size_t read_calls_ = 0;
    std::size_t bytes_read_ = 0;
    std::size_t max_request_ = 0;
    bool injected_ = false;
    bool broke_ = false;
    std::vector<std::size_t> requests_;
};

} // namespace fuzz
