// Deterministic fault-model Reader/Writer for copy_all fuzzing.
//
// These models do NOT exercise production's MemoryReader/MemoryWriter. They are
// minimal, observable endpoints that copy_all() drives, instrumented so the
// fuzz oracle can check invariants (prefix integrity, limit safety, error
// propagation, broken-reader rejection, buffered-path ownership) regardless of
// copy_all's internal loop shape.
//
// Observability the models expose:
//   - every requested read/write size paired with the bytes already committed
//     to the destination before that request (for the per-request remaining-
//     limit invariant)
//   - total calls and bytes transferred
//   - whether a configured failure was actually injected (so the oracle knows to
//     expect an error vs. a full copy)
//   - whether the broken-reader over-report actually fired
//   - buffered-path call counts and bytes consumed (for the BufferedReadable
//     invariants CB1-CB6)
//
// Fault model ordering (§7): a configured failure is evaluated BEFORE any byte
// is transferred or position/counter advanced. The failure modes are:
//
//   AfterCalls(N): the (N+1)-th call fails before transferring bytes.
//   AfterBytes(N): once N bytes have been SUCCESSFULLY transferred, the next
//                  call fails before transferring additional bytes.
//   Disabled:      no failure.
//
// A failed call therefore never copies bytes, never advances position, and never
// increments the successful-byte counter. bytes_read/bytes_written represent
// SUCCESSFUL transfers only. Broken-reader mode intentionally reports an invalid
// count (it is a contract-violation oracle) and takes precedence over normal
// fault injection; it is documented as a distinct mode, never combined
// ambiguously with AfterCalls/AfterBytes.
//
// Additional deterministic endpoint behaviors (§20):
//   - ZeroProgress writer: returns 0 for a non-empty write (exercises
//     Writer::write_all's zero-progress contract through copy_all).
//   - EarlyEOF reader: returns clean EOF (0) after a configured number of
//     successful bytes even when more logical source bytes remain.
#pragma once

#include <sluice/buffered_readable.hpp>
#include <sluice/error.hpp>
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace fuzz {

enum class FailKind { Disabled, AfterCalls, AfterBytes };
enum class LimitKind { Unlimited, Zero, Bounded };
enum class ReaderCapability { Plain, Buffered };

// Which deferred-strategy mode to exercise. The two *Deferred variants pair a
// reserved slot with a policy; the rest are the active strategies.
enum class StrategyKind { Scratch, Auto, BufferedFirst, DeferredReject, DeferredFallback };

// An observation of a single scratch read request, paired with the destination
// bytes already committed before that request was issued. Used by the oracle to
// check the per-request remaining-limit invariant on EVERY exit path (success,
// reader error, writer error, broken-reader rejection, buffered-to-scratch
// transition) — not only on the success branch.
struct ReadObservation {
    std::size_t requested = 0;
    std::uint64_t committed_before_request = 0;
};

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

    // --- BufferedReadable coverage (§5). ---
    ReaderCapability reader_capability = ReaderCapability::Plain;
    std::size_t buffered_prefix = 0;   // 0..min(source size, kMaxBufferedPrefix)
    bool consume_fail = false;         // consume_buffered returns invalid_state

    // --- Additional deterministic behaviors (§20). ---
    bool writer_zero_progress = false; // write_some returns 0 on non-empty input
    std::uint64_t early_eof_after = 0; // 0 => disabled; else clean EOF after this many bytes
};

// Bounded maximum for the initial buffered prefix so a fuzz case stays fast and
// deterministic. The full logical source remains one continuous sequence
// (buffered prefix + scratch-readable tail); the writer output is checked against
// that complete source.
inline constexpr std::size_t kMaxBufferedPrefix = 4096;

// Decode a compact config from the front of the fuzz input. Total: an exhausted
// stream yields defaults (see CopyConfig above). The unconsumed tail is the
// source payload for the copy.
//
// Binary input layout (all little-endian):
//   scratch:u32[0..64]               limit:mod3         limit_val:u32
//   rshort:u32[1..64]                wshort:u32[1..64]
//   rfail:mod3                       rfail_thresh:u8
//   wfail:mod3                       wfail_thresh:u8
//   injected:mod3                    strategy:mod5      broken:mod2
//   capability:mod2                  buffered_prefix:u32[0..kMaxBufferedPrefix]
//   consume_fail:mod2                writer_zero_progress:mod2
//   early_eof_after:u32
//   then the source payload bytes.
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

    // --- BufferedReadable coverage. ---
    cfg.reader_capability = static_cast<ReaderCapability>(cur.take_mod(2));
    cfg.buffered_prefix = cur.take_range(0, static_cast<std::size_t>(kMaxBufferedPrefix));
    cfg.consume_fail = cur.take_mod(2) == 1;

    // --- Additional deterministic behaviors. ---
    cfg.writer_zero_progress = cur.take_mod(2) == 1;
    cfg.early_eof_after = cur.take_u32_le();
    return cfg;
}

class ModelWriter final : public sluice::Writer {
  public:
    explicit ModelWriter(const CopyConfig& cfg) : cfg_(cfg) {}

    sluice::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        ++write_calls_;
        // Failure injection ordering (§7): evaluate BEFORE transferring bytes.
        if (cfg_.writer_fail == FailKind::AfterCalls &&
            write_calls_ > cfg_.writer_fail_threshold) {
            injected_ = true;
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{.code = cfg_.injected, .os_errno = 0});
        }
        if (cfg_.writer_fail == FailKind::AfterBytes &&
            bytes_written_ >= cfg_.writer_fail_threshold) {
            injected_ = true;
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{.code = cfg_.injected, .os_errno = 0});
        }

        // §20.1: zero-progress writer. Returns 0 on a non-empty request without
        // appending any bytes. This exercises Writer::write_all's zero-progress
        // contract (invalid_state) through the public copy_all surface.
        if (cfg_.writer_zero_progress && !src.empty()) {
            zero_progress_ = true;
            return std::size_t{0};
        }

        std::size_t count = std::min(src.size(), cfg_.writer_short);
        if (!src.empty()) {
            requests_.push_back(src.size());
        }
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
    bool zero_progress() const noexcept { return zero_progress_; }
    const std::vector<std::size_t>& requests() const noexcept { return requests_; }

  private:
    CopyConfig cfg_;
    std::vector<std::byte> buf_;
    std::size_t write_calls_ = 0;
    std::size_t bytes_written_ = 0;
    bool injected_ = false;
    bool zero_progress_ = false;
    std::vector<std::size_t> requests_;
};

// Plain Reader model (no BufferedReadable). Fault ordering is fixed: failures
// fire before any byte transfer, position advance, or counter increment.
class ModelReader final : public sluice::Reader {
  public:
    ModelReader(std::span<const std::byte> source, const CopyConfig& cfg)
        : source_(source.begin(), source.end()), cfg_(cfg) {}

    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        record_request(dst.size());
        ++read_calls_;

        // Broken-reader mode takes precedence over normal fault injection and is
        // a distinct contract: it intentionally over-reports. It is never
        // combined ambiguously with AfterCalls/AfterBytes.
        if (cfg_.broken_reader && !dst.empty()) {
            broke_ = true;
            return dst.size() + 1;
        }

        // Failure injection ordering (§7): evaluate BEFORE transferring bytes.
        if (cfg_.reader_fail == FailKind::AfterCalls &&
            read_calls_ > cfg_.reader_fail_threshold) {
            injected_ = true;
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{.code = cfg_.injected, .os_errno = 0});
        }
        if (cfg_.reader_fail == FailKind::AfterBytes &&
            bytes_read_ >= cfg_.reader_fail_threshold) {
            injected_ = true;
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{.code = cfg_.injected, .os_errno = 0});
        }

        std::size_t available = source_.size() - pos_;
        std::size_t count = std::min({available, dst.size(), cfg_.reader_short});

        // §20.2: early clean EOF. Once bytes_read_ reaches the configured cap,
        // report EOF (0) even if more logical source bytes remain. This is a
        // distinct contract from an injected Reader error or a broken-reader
        // over-report.
        if (cfg_.early_eof_after > 0 && bytes_read_ >= cfg_.early_eof_after) {
            early_eof_ = true;
            return std::size_t{0};
        }

        if (count > 0) {
            // count != 0, so both pointers are non-null.
            std::memcpy(dst.data(), source_.data() + pos_, count);
        }
        pos_ += count;
        bytes_read_ += count;
        return count;
    }

    std::size_t read_calls() const noexcept { return read_calls_; }
    std::size_t bytes_read() const noexcept { return bytes_read_; }
    std::size_t max_request() const noexcept { return max_request_; }
    bool injected() const noexcept { return injected_; }
    bool broke() const noexcept { return broke_; }
    bool early_eof() const noexcept { return early_eof_; }
    const std::vector<std::size_t>& requests() const noexcept { return requests_; }
    const std::vector<ReadObservation>& observations() const noexcept { return observations_; }

  protected:
    // Shared by ModelReader and ModelBufferedReader so the per-request remaining
    // limit observation is recorded identically for both capabilities.
    void record_request(std::size_t requested) {
        if (requested == 0) {
            return;
        }
        requests_.push_back(requested);
        observations_.push_back(ReadObservation{requested, bytes_read_});
        max_request_ = std::max(max_request_, requested);
    }

    std::vector<std::byte> source_;
    CopyConfig cfg_;
    std::size_t pos_ = 0;
    std::size_t read_calls_ = 0;
    std::size_t bytes_read_ = 0;
    std::size_t max_request_ = 0;
    bool injected_ = false;
    bool broke_ = false;
    bool early_eof_ = false;
    std::vector<std::size_t> requests_;
    std::vector<ReadObservation> observations_;
};

// Buffered Reader model implementing both Reader and BufferedReadable. Mirrors
// production's BufferedReader: there is ONE logical source position. peek_buffered
// exposes the already-buffered unread region (the configured leading prefix that
// has been "pre-fetched"); consume_buffered advances past buffered bytes without
// an inner read; read_some serves bytes from the buffered region first, then
// from the rest of the source, honoring the same fault model as ModelReader.
//
// The full logical source is one continuous sequence. read_some and
// peek/consume share the same logical cursor (logical_pos_): consuming N bytes
// via consume_buffered advances logical_pos_ by N, so a subsequent read_some
// continues exactly past those bytes (no duplication, no skip).
//
// The buffered span remains valid until the next mutating Reader operation, per
// the production BufferedReadable contract. This model keeps the prefix in a
// stable owned buffer so that invariant holds.
class ModelBufferedReader final : public sluice::Reader, public sluice::BufferedReadable {
  public:
    ModelBufferedReader(std::span<const std::byte> source, const CopyConfig& cfg)
        : source_(source.begin(), source.end()), cfg_(cfg) {
        // The buffered prefix is the leading cfg.buffered_prefix bytes of the
        // full logical source (clamped to the source size). These bytes are
        // "pre-fetched": peek_buffered exposes them and consume_buffered advances
        // the shared logical cursor past them without an inner read.
        std::size_t pre = std::min(cfg.buffered_prefix, source.size());
        buffered_.assign(source.begin(), source.begin() + pre);
    }

    // --- BufferedReadable capability. ---
    std::span<const std::byte> peek_buffered() const override {
        ++peek_calls_;
        // The buffered region is the pre-fetched prefix that logical_pos_ has not
        // yet passed. Once logical_pos_ moves past the prefix (by consume_buffered
        // or read_some), there is nothing buffered to peek.
        if (logical_pos_ >= buffered_.size()) {
            return {};
        }
        return {buffered_.data() + logical_pos_, buffered_.size() - logical_pos_};
    }

    sluice::Result<void> consume_buffered(std::size_t n) override {
        ++consume_calls_;
        if (cfg_.consume_fail) {
            consume_failed_ = true;
            return sluice::make_unexpected<void>(
                sluice::IoError{.code = sluice::IoError::Code::invalid_state});
        }
        std::size_t avail = buffered_.size() - logical_pos_;
        // avail is only nonzero within the pre-fetched prefix region.
        if (n > avail) {
            consume_failed_ = true;
            return sluice::make_unexpected<void>(
                sluice::IoError{.code = sluice::IoError::Code::invalid_state});
        }
        logical_pos_ += n;
        buffered_consumed_ += n;
        bytes_delivered_ += n;
        return {};
    }

    // --- Reader. Serves buffered bytes first, then the rest of the source. ---
    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        record_request(dst.size());
        ++read_calls_;

        // Broken-reader / fault ordering identical to ModelReader; the buffered
        // capability does not change scratch-path fault semantics.
        if (cfg_.broken_reader && !dst.empty()) {
            broke_ = true;
            return dst.size() + 1;
        }
        if (cfg_.reader_fail == FailKind::AfterCalls &&
            read_calls_ > cfg_.reader_fail_threshold) {
            injected_ = true;
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{.code = cfg_.injected, .os_errno = 0});
        }
        if (cfg_.reader_fail == FailKind::AfterBytes &&
            bytes_read_ >= cfg_.reader_fail_threshold) {
            injected_ = true;
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{.code = cfg_.injected, .os_errno = 0});
        }

        // Early EOF counts successful bytes from BOTH the buffered drain and the
        // scratch tail (bytes_delivered tracks the full logical source).
        if (cfg_.early_eof_after > 0 && bytes_delivered_ >= cfg_.early_eof_after) {
            early_eof_ = true;
            return std::size_t{0};
        }

        std::size_t available = source_.size() - logical_pos_;
        std::size_t count = std::min({available, dst.size(), cfg_.reader_short});
        if (count > 0) {
            std::memcpy(dst.data(), source_.data() + logical_pos_, count);
        }
        logical_pos_ += count;
        bytes_read_ += count;
        bytes_delivered_ += count;
        return count;
    }

    // --- Observability for the buffered invariants. ---
    std::size_t read_calls() const noexcept { return read_calls_; }
    std::size_t bytes_read() const noexcept { return bytes_read_; }
    std::size_t max_request() const noexcept { return max_request_; }
    bool injected() const noexcept { return injected_; }
    bool broke() const noexcept { return broke_; }
    bool early_eof() const noexcept { return early_eof_; }
    bool consume_failed() const noexcept { return consume_failed_; }
    std::size_t peek_calls() const noexcept { return peek_calls_; }
    std::size_t consume_calls() const noexcept { return consume_calls_; }
    std::size_t buffered_consumed() const noexcept { return buffered_consumed_; }
    std::size_t buffered_remaining() const noexcept {
        return logical_pos_ < buffered_.size() ? buffered_.size() - logical_pos_ : 0;
    }
    const std::vector<std::size_t>& requests() const noexcept { return requests_; }
    const std::vector<ReadObservation>& observations() const noexcept { return observations_; }

  private:
    void record_request(std::size_t requested) {
        if (requested == 0) {
            return;
        }
        requests_.push_back(requested);
        // committed_before_request for the per-request remaining-limit invariant
        // counts every byte delivered to the destination so far (buffered drain +
        // scratch tail). The oracle supplies the destination-committed count via
        // the writer when checking the limit; here we record the source-side
        // committed count, which the oracle cross-checks against writer bytes.
        observations_.push_back(ReadObservation{requested, bytes_delivered_});
        max_request_ = std::max(max_request_, requested);
    }

    // One continuous logical source; logical_pos_ is the shared cursor for both
    // peek/consume and read_some.
    std::vector<std::byte> source_;
    std::vector<std::byte> buffered_; // stable storage for peek_buffered() span
    std::size_t logical_pos_ = 0;
    CopyConfig cfg_;
    std::size_t read_calls_ = 0;
    std::size_t bytes_read_ = 0;        // scratch-path successful bytes
    std::size_t bytes_delivered_ = 0;   // buffered drain + scratch tail
    std::size_t buffered_consumed_ = 0; // bytes consumed via consume_buffered
    std::size_t max_request_ = 0;
    // mutable: peek_buffered() is const per the BufferedReadable contract, but
    // the observation counter is harness-only telemetry that does not affect the
    // buffered span's lifetime or contents.
    mutable std::size_t peek_calls_ = 0;
    std::size_t consume_calls_ = 0;
    bool injected_ = false;
    bool broke_ = false;
    bool early_eof_ = false;
    bool consume_failed_ = false;
    std::vector<std::size_t> requests_;
    std::vector<ReadObservation> observations_;
};

} // namespace fuzz
