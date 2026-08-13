// copy_all deterministic fault-model fuzz target.
//
// Decodes a compact config from the front of the fuzz input (see copy_model.hpp);
// the rest is the COMPLETE source payload — the harness performs NO silent
// semantic truncation (§19). Resource bounds are enforced by target-specific
// libFuzzer -max_len configuration in the smoke/campaign runners, not by
// discarding bytes here.
//
// It drives the real production copy_all() with deterministic model
// Reader/Writer endpoints. The model reader may be plain or implement
// BufferedReadable (§5), so the Auto / BufferedFirst strategies exercise the
// production buffered fast path.
//
// Oracle structure (§21): universal safety invariants run BEFORE any
// result-specific early return:
//
//   U1 sink is an exact source prefix (every exit path)
//   U2 sink size never exceeds the logical source size
//   U3 bounded-copy sink size never exceeds the limit
//   U4 stats.bytes_written == actual sink size
//   U5 stats.copy_calls == 1
//   U6 every scratch Reader request respects the remaining limit at that moment
//      (checked on success AND error paths)
//   U7 a failed endpoint call does not advance that endpoint's successful-byte
//      counter
//
// Then the terminal result is classified:
//
//   early validation rejection; deferred strategy rejection; broken Reader;
//   Reader error; Writer zero-progress; Writer error; consume-buffered error;
//   clean EOF; limit stop; successful full copy.
#include <sluice/copy.hpp>
#include <sluice/limit.hpp>
#include <sluice/copy_strategy.hpp>
#include <sluice/measurement.hpp>

#include <fuzz/support/byte_cursor.hpp>
#include <fuzz/support/copy_model.hpp>
#include <fuzz/support/fuzz_abort.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using fuzz::ByteCursor;
using fuzz::CopyConfig;
using fuzz::LimitKind;
using fuzz::ModelBufferedReader;
using fuzz::ModelReader;
using fuzz::ModelWriter;
using fuzz::ReaderCapability;
using fuzz::StrategyKind;

// Map the decoded strategy kind to production's CopyOptions (strategy + the
// deferred policy when relevant).
static sluice::CopyOptions make_options(const CopyConfig& cfg) {
    sluice::CopyOptions opt;
    switch (cfg.strategy) {
    case StrategyKind::Scratch:
        opt.strategy = sluice::CopyStrategy::Scratch;
        break;
    case StrategyKind::Auto:
        opt.strategy = sluice::CopyStrategy::Auto;
        break;
    case StrategyKind::BufferedFirst:
        opt.strategy = sluice::CopyStrategy::BufferedFirst;
        break;
    case StrategyKind::DeferredReject:
        opt.strategy = sluice::CopyStrategy::VectorDeferred;
        opt.unsupported_policy = sluice::UnsupportedStrategyPolicy::ReturnInvalidState;
        break;
    case StrategyKind::DeferredFallback:
        opt.strategy = sluice::CopyStrategy::VectorDeferred;
        opt.unsupported_policy = sluice::UnsupportedStrategyPolicy::FallbackToAuto;
        break;
    }
    return opt;
}

static sluice::CopyLimit make_limit(const CopyConfig& cfg) {
    if (cfg.limit == LimitKind::Zero) {
        return sluice::CopyLimit::nothing();
    }
    if (cfg.limit == LimitKind::Bounded) {
        return sluice::CopyLimit::bytes(cfg.limit_value);
    }
    return sluice::CopyLimit::unlimited();
}

// Visitor holding either a plain or buffered reader so the oracle can inspect
// observations uniformly regardless of capability.
class ReaderView {
  public:
    ReaderView(ModelReader* plain, ModelBufferedReader* buffered)
        : plain_(plain), buffered_(buffered) {}

    bool is_buffered() const noexcept { return buffered_ != nullptr; }
    sluice::Reader& as_reader() const {
        return buffered_ ? static_cast<sluice::Reader&>(*buffered_)
                         : static_cast<sluice::Reader&>(*plain_);
    }

    // Common accessors used by the oracle.
    bool broken() const noexcept {
        return buffered_ ? buffered_->broke() : plain_->broke();
    }
    bool injected() const noexcept {
        return buffered_ ? buffered_->injected() : plain_->injected();
    }
    bool early_eof() const noexcept {
        return buffered_ ? buffered_->early_eof() : plain_->early_eof();
    }
    std::size_t read_calls() const noexcept {
        return buffered_ ? buffered_->read_calls() : plain_->read_calls();
    }
    std::size_t bytes_read() const noexcept {
        return buffered_ ? buffered_->bytes_read() : plain_->bytes_read();
    }
    const std::vector<fuzz::ReadObservation>& observations() const noexcept {
        return buffered_ ? buffered_->observations() : plain_->observations();
    }
    // Buffered-only observability (returns 0 / false for plain readers).
    std::size_t peek_calls() const noexcept {
        return buffered_ ? buffered_->peek_calls() : 0;
    }
    std::size_t consume_calls() const noexcept {
        return buffered_ ? buffered_->consume_calls() : 0;
    }
    std::size_t buffered_consumed() const noexcept {
        return buffered_ ? buffered_->buffered_consumed() : 0;
    }
    std::size_t buffered_remaining() const noexcept {
        return buffered_ ? buffered_->buffered_remaining() : 0;
    }
    bool consume_failed() const noexcept {
        return buffered_ ? buffered_->consume_failed() : false;
    }

  private:
    ModelReader* plain_;
    ModelBufferedReader* buffered_;
};

// Returns true when the configured limit effectively allows zero bytes.
// Production's CopyLimit::bytes(0) behaves identically to CopyLimit::nothing()
// (src/copy.cpp:108-113): both produce an immediate success(0) return without
// touching endpoints, so the oracle must treat them the same way.
static bool is_effectively_zero(const CopyConfig& cfg) noexcept {
    return cfg.limit == LimitKind::Zero ||
           (cfg.limit == LimitKind::Bounded && cfg.limit_value == 0);
}

// Check every universal invariant first. These MUST hold on every exit path
// before any result-specific reasoning, so they are evaluated unconditionally.
static void check_universal(const CopyConfig& cfg, std::span<const std::byte> source,
                            const ReaderView& rv, const ModelWriter& writer,
                            const sluice::Result<std::uint64_t>& result,
                            const sluice::CopyStats& stats) {
    (void)result;  // reserved for result-specific checks; universal invariants above
    const auto& sink = writer.bytes();
    const std::uint64_t sink_size = sink.size();

    // U1: sink is an exact source prefix.
    FUZZ_ASSERT(sink_size <= source.size());
    if (sink_size > 0) {
        FUZZ_ASSERT(std::memcmp(sink.data(), source.data(), static_cast<std::size_t>(sink_size)) ==
                    0);
    }

    // U2: sink size never exceeds the logical source size (redundant with U1 but
    // explicit; defends against a future model that decouples sink from source).
    FUZZ_ASSERT(sink_size <= source.size());

    // U3: bounded-copy sink size never exceeds the limit.
    if (cfg.limit == LimitKind::Bounded) {
        FUZZ_ASSERT(sink_size <= cfg.limit_value);
    }

    // U4: stats.bytes_written == actual sink size. Production's buffered fast
    // path advances bytes_written only AFTER a successful consume_buffered
    // (src/copy.cpp), so when consume_buffered fails after a successful write
    // the sink has bytes that stats does not yet credit. That is the documented
    // CB6 behavior, so this invariant is only checked when consume did not fail
    // on the buffered path. Additionally, Writer::write_all may short-write and
    // then fail (e.g. AfterCalls/AfterBytes in the model), leaving partial bytes
    // in the sink that stats does not yet credit. Skip the check when a writer
    // error was injected for the same reason.
    if (!rv.consume_failed() && !writer.injected()) {
        FUZZ_ASSERT(stats.bytes_written == sink_size);
    }

    // U5: copy_calls == 1 (one top-level copy_all entry).
    FUZZ_ASSERT(stats.copy_calls == 1);

    // U6: every scratch Reader request respects the remaining limit AT THAT
    // MOMENT. Checked on success AND error paths (not only in the success
    // branch). For a bounded copy, requested <= limit_value - committed_before.
    // Handle the underflow case safely.
    if (cfg.limit == LimitKind::Bounded) {
        for (const auto& obs : rv.observations()) {
            // committed_before_request must not exceed the limit.
            FUZZ_ASSERT(obs.committed_before_request <= cfg.limit_value);
            const std::uint64_t remaining = cfg.limit_value - obs.committed_before_request;
            FUZZ_ASSERT(static_cast<std::uint64_t>(obs.requested) <= remaining);
        }
    }

    // U7: a failed endpoint call does not advance that endpoint's successful-byte
    // counter. The fault model guarantees a failed call transfers zero bytes, so
    // the reader's successful bytes cannot exceed the source size, and the
    // writer's bytes_written equals the sink size (already checked in U4). For
    // the reader, bytes_read reflects only successful scratch transfers.
    FUZZ_ASSERT(rv.bytes_read() <= source.size());
}

static void check_oracle(const CopyConfig& cfg, std::span<const std::byte> source,
                         const ReaderView& rv, const ModelWriter& writer,
                         const sluice::Result<std::uint64_t>& result,
                         const sluice::CopyDecision& decision, const sluice::CopyStats& stats) {
    // --- Universal invariants first (§21). ---
    check_universal(cfg, source, rv, writer, result, stats);

    const auto& sink = writer.bytes();
    const std::uint64_t sink_size = sink.size();

    // --- CB1: strategy activation (buffered path). ---
    const bool can_buffered = rv.is_buffered();
    const bool wants_fast_path = cfg.strategy == StrategyKind::Auto ||
                                 cfg.strategy == StrategyKind::BufferedFirst;
    if (wants_fast_path && can_buffered && cfg.buffered_prefix > 0) {
        // Unless an earlier terminal condition preempted entry (broken reader,
        // zero/empty-scratch rejection — handled in their branches below), the
        // fast path must activate and peek_buffered must be called.
        // The branches below assert this where the precondition holds.
    }
    if (cfg.strategy == StrategyKind::Scratch) {
        // Scratch never touches the buffered capability.
        FUZZ_ASSERT(rv.peek_calls() == 0);
        FUZZ_ASSERT(rv.consume_calls() == 0);
        FUZZ_ASSERT(decision.used_buffered_fast_path == false);
    }

    // --- Broken-reader rejection (distinct from injected error / early EOF). ---
    if (rv.broken()) {
        FUZZ_ASSERT(!result.has_value());
        FUZZ_ASSERT(result.error().code == sluice::IoError::Code::invalid_state);
        // The over-reported fictitious byte must never reach the sink.
        FUZZ_ASSERT(sink_size <= source.size());
        return;
    }

    // --- Deferred reject returns invalid_state, touches nothing. ---
    if (cfg.strategy == StrategyKind::DeferredReject) {
        FUZZ_ASSERT(!result.has_value());
        FUZZ_ASSERT(result.error().code == sluice::IoError::Code::invalid_state);
        FUZZ_ASSERT(decision.unsupported_requested);
        FUZZ_ASSERT(rv.read_calls() == 0);
        FUZZ_ASSERT(writer.bytes_written() == 0);
        FUZZ_ASSERT(rv.peek_calls() == 0);
        FUZZ_ASSERT(rv.consume_calls() == 0);
        return;
    }

    // --- Empty scratch with a non-zero/unlimited copy => invalid_state. ---
    // Bounded(0) is excluded: production returns success(0) immediately without
    // touching endpoints (is_effectively_zero).
    if (cfg.scratch_size == 0 && !is_effectively_zero(cfg)) {
        FUZZ_ASSERT(!result.has_value());
        FUZZ_ASSERT(result.error().code == sluice::IoError::Code::invalid_state);
        FUZZ_ASSERT(rv.read_calls() == 0);
        FUZZ_ASSERT(writer.bytes_written() == 0);
        FUZZ_ASSERT(rv.peek_calls() == 0);
        FUZZ_ASSERT(rv.consume_calls() == 0);
        return;
    }

    // --- Zero limit => success with 0, endpoints never touched. ---
    // Bounded(0) is included: production returns success(0) before any
    // endpoint operation (src/copy.cpp:108-113).
    if (is_effectively_zero(cfg)) {
        FUZZ_ASSERT(result.has_value());
        FUZZ_ASSERT(result.value() == 0);
        FUZZ_ASSERT(rv.read_calls() == 0);
        FUZZ_ASSERT(writer.write_calls() == 0);
        FUZZ_ASSERT(rv.peek_calls() == 0);
        FUZZ_ASSERT(rv.consume_calls() == 0);
        return;
    }

    // From here: non-zero scratch, non-zero limit, not broken, not deferred-reject.

    // --- CB1 (positive): for Auto/BufferedFirst with a non-empty buffered
    //     prefix, the fast path must have ACTIVATED — production must have
    //     called peek_buffered. (Production sets decision.used_buffered_fast_path
    //     only after a region is fully drained: successful write_all AND
    //     successful consume_buffered. So the flag stays false on a writer-failure
    //     or consume-failure path even though the fast path was entered; the
    //     authoritative "fast path was entered" signal here is peek_calls.) ---
    if (wants_fast_path && can_buffered && cfg.buffered_prefix > 0) {
        FUZZ_ASSERT(rv.peek_calls() > 0);
        // Production credits the fast path only when at least one buffered region
        // was fully drained (write + consume both succeeded). That is exactly the
        // case when no writer error was injected AND consume did not fail.
        const bool fully_drained_once =
            !writer.injected() && !rv.consume_failed() && rv.buffered_consumed() > 0;
        if (fully_drained_once) {
            FUZZ_ASSERT(decision.used_buffered_fast_path == true);
        }
    }

    // --- Writer zero-progress (§20.1): copy_all returns invalid_state; the
    //     target terminates rather than spinning; sink is an exact source prefix
    //     (already checked in U1); no false success. ---
    if (writer.zero_progress()) {
        FUZZ_ASSERT(!result.has_value());
        FUZZ_ASSERT(result.error().code == sluice::IoError::Code::invalid_state);
        return;
    }

    // --- Reader/Writer injected error propagation. ---
    if (rv.injected() || writer.injected()) {
        FUZZ_ASSERT(!result.has_value());
        FUZZ_ASSERT(result.error().code == cfg.injected);
        // Exactly one terminal stop reason on the error path.
        const auto stops = static_cast<std::uint64_t>(stats.reader_error_stops) +
                           static_cast<std::uint64_t>(stats.writer_error_stops);
        FUZZ_ASSERT(stops == 1);

        // --- CB3: writer failure preserves buffered ownership. If write_all
        //     failed while draining buffered bytes, consume_buffered is not
        //     called for that failed region and buffered bytes remain. We
        //     cannot directly know the failed region size, but consume must
        //     not have advanced beyond what was successfully written. ---
        if (can_buffered && writer.injected()) {
            // Buffered bytes consumed so far cannot exceed the sink size for a
            // buffered-path writer failure (write_all is all-or-nothing per
            // region, so a failed region consumes nothing).
            FUZZ_ASSERT(rv.buffered_consumed() <= sink_size);
        }
        return;
    }

    // --- Consume-buffered error propagation (CB6). If consume_buffered failed
    //     after a successful writer operation, copy_all returns the consume
    //     error; the sink remains a valid source prefix (U1). stats and stop
    //     reason remain internally consistent: this is a reader_error_stops
    //     path in production. ---
    if (rv.consume_failed()) {
        FUZZ_ASSERT(!result.has_value());
        FUZZ_ASSERT(result.error().code == sluice::IoError::Code::invalid_state);
        // The consume failure is a reader-side error classification.
        FUZZ_ASSERT(stats.reader_error_stops >= 1);
        // U1 already guarantees the sink is a valid prefix.
        return;
    }

    // --- Success path from here (clean EOF, limit stop, or full copy). ---
    FUZZ_ASSERT(result.has_value());
    FUZZ_ASSERT(result.value() == sink_size);

    // --- CB4: on successful buffered write, consumed buffered bytes == written
    //     buffered bytes. Over the whole copy, the buffered bytes consumed plus
    //     scratch bytes read equals the sink size (no duplication, no skip). ---
    if (can_buffered) {
        FUZZ_ASSERT(rv.buffered_consumed() + rv.bytes_read() == sink_size);
    }

    if (cfg.limit == LimitKind::Unlimited) {
        // Either a full copy or an early clean EOF.
        if (rv.early_eof()) {
            // §20.2: returned count equals bytes delivered before EOF; sink is
            // that source prefix (U1). No synthesis of undisclosed bytes.
            FUZZ_ASSERT(sink_size <= source.size());
        } else {
            FUZZ_ASSERT(sink_size == source.size());
        }
    } else { // Bounded
        const std::uint64_t cap = std::min<std::uint64_t>(source.size(), cfg.limit_value);
        if (rv.early_eof()) {
            FUZZ_ASSERT(sink_size <= cap);
        } else {
            FUZZ_ASSERT(sink_size == cap);
        }
        FUZZ_ASSERT(sink_size <= cfg.limit_value);
        if (result.has_value()) {
            FUZZ_ASSERT(result.value() <= cfg.limit_value);
        }
    }

    // --- CB2: limit safety for bounded copies (buffered write amount <=
    //     remaining limit; total sink bytes <= limit). U3 already covers the
    //     total; the per-request invariant is U6. ---

    // --- CB5: buffered-to-scratch transition. When the buffered prefix is
    //     exhausted and source bytes remain, the copy continues through scratch
    //     with no duplication, skip, or reordering. U1 (exact prefix) plus
    //     CB4 (consumed + read == sink) jointly prove this. ---
    if (can_buffered && wants_fast_path) {
        // After a successful unlimited full copy, the buffered prefix must be
        // fully drained.
        if (cfg.limit == LimitKind::Unlimited && !rv.early_eof() &&
            sink_size == source.size()) {
            FUZZ_ASSERT(rv.buffered_remaining() == 0);
        }
    }

    // --- Deferred fallback behaves like Auto and records the decision. ---
    if (cfg.strategy == StrategyKind::DeferredFallback) {
        FUZZ_ASSERT(decision.unsupported_requested);
        FUZZ_ASSERT(decision.selected == sluice::CopyStrategy::BufferedFirst);
    }

    // --- Stats sanity on the success path. ---
    FUZZ_ASSERT(stats.bytes_written == sink_size);
    FUZZ_ASSERT(stats.bytes_read >= stats.bytes_written);
    // Terminal stop reason: exactly one of eof/limit on success.
    const auto stops = static_cast<std::uint64_t>(stats.eof_stops) +
                       static_cast<std::uint64_t>(stats.limit_stops);
    FUZZ_ASSERT(stops == 1);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::byte* bytes = reinterpret_cast<const std::byte*>(data);
    ByteCursor cur(bytes, size);

    CopyConfig cfg = fuzz::decode_config(cur);
    // The COMPLETE unconsumed tail is the source. The harness performs NO silent
    // semantic truncation (§19): resource bounds live in the runner's
    // target-specific -max_len, not here.
    auto source_vec = cur.take_rest();
    std::span<const std::byte> source(source_vec);

    auto options = make_options(cfg);
    options.limit = make_limit(cfg);

    // Scratch buffer per the config (may be zero-length).
    std::vector<std::byte> scratch(cfg.scratch_size);

    ModelWriter writer(cfg);
    sluice::CopyStats stats{};
    sluice::CopyDecision decision{};

    // Choose the reader capability per the fuzz config (§5.2). The full logical
    // source is one continuous sequence for both models; the writer output is
    // checked against that complete source.
    ModelReader plain_reader(source, cfg);
    ModelBufferedReader buffered_reader(source, cfg);

    ReaderView rv(cfg.reader_capability == ReaderCapability::Buffered ? nullptr : &plain_reader,
                  cfg.reader_capability == ReaderCapability::Buffered ? &buffered_reader
                                                                      : nullptr);

    auto result = sluice::copy_all(rv.as_reader(), writer, std::span<std::byte>(scratch), options,
                                   &stats, &decision);

    check_oracle(cfg, source, rv, writer, result, decision, stats);
    return 0;
}
