// copy_all deterministic fault-model fuzz target.
//
// Decodes a compact config from the front of the fuzz input (see copy_model.hpp);
// the rest is the source payload. It drives the real production copy_all() with
// deterministic model Reader/Writer endpoints and checks observable invariants
// that must hold regardless of copy_all's internal loop shape:
//
//   C1 sink prefix integrity
//   C2 limit safety
//   C3 scratch safety
//   C4 success consistency
//   C5 error propagation
//   C6 broken-reader rejection
//   C7 deferred-strategy behavior
//   C8 stats sanity
//
// The oracle uses the models' own observation of whether a failure was actually
// injected, so it predicts the right outcome without re-implementing the loop.
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
using fuzz::ModelReader;
using fuzz::ModelWriter;
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

// Check all observable invariants. Returns nothing; traps on violation.
static void check_oracle(const CopyConfig& cfg, std::span<const std::byte> source,
                         const ModelReader& reader, const ModelWriter& writer,
                         const sluice::Result<std::uint64_t>& result,
                         const sluice::CopyDecision& decision, const sluice::CopyStats& stats) {
    const auto& sink = writer.bytes();

    // --- C1: sink prefix integrity (holds at every exit). ---
    FUZZ_ASSERT(sink.size() <= source.size());
    if (!sink.empty()) {
        FUZZ_ASSERT(std::memcmp(sink.data(), source.data(), sink.size()) == 0);
    }

    // --- C6: broken-reader rejection. ---
    // If the model reader actually over-reported, copy_all must have returned
    // invalid_state BEFORE handing the fictitious excess to write_all.
    if (reader.broke()) {
        FUZZ_ASSERT(!result.has_value());
        FUZZ_ASSERT(result.error().code == sluice::IoError::Code::invalid_state);
        return;
    }

    // --- C7: deferred reject returns invalid_state, touches nothing. ---
    if (cfg.strategy == StrategyKind::DeferredReject) {
        FUZZ_ASSERT(!result.has_value());
        FUZZ_ASSERT(result.error().code == sluice::IoError::Code::invalid_state);
        FUZZ_ASSERT(decision.unsupported_requested);
        FUZZ_ASSERT(reader.read_calls() == 0);
        FUZZ_ASSERT(writer.bytes_written() == 0);
        return;
    }

    // --- C3: empty scratch with a non-zero/unlimited copy => invalid_state. ---
    if (cfg.scratch_size == 0 && cfg.limit != LimitKind::Zero) {
        FUZZ_ASSERT(!result.has_value());
        FUZZ_ASSERT(result.error().code == sluice::IoError::Code::invalid_state);
        FUZZ_ASSERT(reader.read_calls() == 0);
        FUZZ_ASSERT(writer.bytes_written() == 0);
        return;
    }

    // --- C2/C3: zero limit => success with 0, endpoints never touched. ---
    if (cfg.limit == LimitKind::Zero) {
        FUZZ_ASSERT(result.has_value());
        FUZZ_ASSERT(result.value() == 0);
        FUZZ_ASSERT(reader.read_calls() == 0);
        FUZZ_ASSERT(writer.write_calls() == 0);
        return;
    }

    // From here: non-zero scratch, non-zero limit, not broken, not deferred-reject.

    // --- C5: error propagation when a failure was actually injected. ---
    if (reader.injected() || writer.injected()) {
        FUZZ_ASSERT(!result.has_value());
        FUZZ_ASSERT(result.error().code == cfg.injected);
        // C1 already guarantees the sink is a valid source prefix.
        // Exactly one terminal stop reason on the error path.
        const auto stops = static_cast<std::uint64_t>(stats.reader_error_stops) +
                           static_cast<std::uint64_t>(stats.writer_error_stops);
        FUZZ_ASSERT(stops == 1);
        return;
    }

    // --- C4: success consistency. ---
    FUZZ_ASSERT(result.has_value());
    FUZZ_ASSERT(result.value() == sink.size());
    FUZZ_ASSERT(sink.size() <= source.size());

    if (cfg.limit == LimitKind::Unlimited) {
        FUZZ_ASSERT(sink.size() == source.size());
    } else { // Bounded
        const std::uint64_t expected = std::min<std::uint64_t>(source.size(), cfg.limit_value);
        FUZZ_ASSERT(sink.size() == expected);
    }

    // --- C2: limit safety for bounded copies. ---
    if (cfg.limit == LimitKind::Bounded) {
        FUZZ_ASSERT(sink.size() <= cfg.limit_value);
        if (result.has_value()) {
            FUZZ_ASSERT(result.value() <= cfg.limit_value);
        }
        // copy_all must never ask for more than the remaining limit; every
        // recorded request is therefore <= limit.
        for (auto req : reader.requests()) {
            FUZZ_ASSERT(req <= cfg.limit_value);
        }
    }

    // --- C7: deferred fallback behaves like Auto and records the decision. ---
    if (cfg.strategy == StrategyKind::DeferredFallback) {
        FUZZ_ASSERT(decision.unsupported_requested);
        FUZZ_ASSERT(decision.selected == sluice::CopyStrategy::BufferedFirst);
    }

    // --- C8: stats sanity on the success path. ---
    FUZZ_ASSERT(stats.bytes_written == sink.size());
    FUZZ_ASSERT(stats.bytes_read >= stats.bytes_written);
    FUZZ_ASSERT(stats.copy_calls == 1);
    const auto stops = static_cast<std::uint64_t>(stats.eof_stops) +
                       static_cast<std::uint64_t>(stats.limit_stops);
    FUZZ_ASSERT(stops == 1);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::byte* bytes = reinterpret_cast<const std::byte*>(data);
    ByteCursor cur(bytes, size);

    CopyConfig cfg = fuzz::decode_config(cur);
    auto source = cur.take_rest();

    // Bound the source so a single fuzz case stays fast and deterministic.
    // 4 KiB is plenty to exercise multi-iteration short-read loops.
    constexpr std::size_t kMaxSource = 4096;
    if (source.size() > kMaxSource) {
        source.resize(kMaxSource);
    }

    auto options = make_options(cfg);
    options.limit = make_limit(cfg);

    // Scratch buffer per the config (may be zero-length).
    std::vector<std::byte> scratch(cfg.scratch_size);

    ModelReader reader(source, cfg);
    ModelWriter writer(cfg);
    sluice::CopyStats stats{};
    sluice::CopyDecision decision{};

    auto result = sluice::copy_all(reader, writer, std::span<std::byte>(scratch), options, &stats,
                                   &decision);

    check_oracle(cfg, source, reader, writer, result, decision, stats);
    return 0;
}
