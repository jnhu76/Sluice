// copy_all implementation. The strategy-aware overload is the source of truth;
// the CopyLimit / back-compat / convenience overloads delegate to it.
//
// The loop tries the buffered fast path first (CPPIO-CORE-006D) when the
// strategy allows it: when the reader also implements BufferedReadable,
// already-buffered unread bytes are drained via peek_buffered/consume_buffered
// before any scratch read, mirroring Zig std.Io's Reader.stream (Reader.zig:168).
// CPPIO-CORE-007C made that an explicit CopyStrategy choice.
#include <sluice/copy.hpp>
#include <sluice/buffered_readable.hpp>

#include <algorithm>
#include <array>

namespace sluice {

namespace {

// Whether a strategy is one of the deferred (not-yet-implemented) reserved
// slots. They never execute their named path this stage.
bool is_deferred(CopyStrategy s) {
    return s == CopyStrategy::VectorDeferred || s == CopyStrategy::FileRangeDeferred ||
           s == CopyStrategy::SendfileDeferred || s == CopyStrategy::SpliceDeferred;
}

} // namespace

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch,
                               CopyOptions options, CopyStats* stats, CopyDecision* decision) {
    if (stats) {
        ++stats->copy_calls;
    }

    CopyDecision local_dec;
    CopyDecision& dec = decision ? *decision : local_dec;
    dec.requested = options.strategy;
    dec.selected = options.strategy;
    dec.reason = to_string(options.strategy);
    dec.used_buffered_fast_path = false;
    dec.used_scratch_path = false;
    dec.unsupported_requested = false;

    // --- Deferred strategies: explicitly unsupported this stage (007E). ---
    // Kept here (not in 007E only) so the intermediate is never silently broken.
    if (is_deferred(options.strategy)) {
        if (options.unsupported_policy == UnsupportedStrategyPolicy::FallbackToAuto) {
            dec.unsupported_requested = true;
            dec.selected = CopyStrategy::Auto;
            dec.reason = "deferred_fallback_to_auto";
            if (stats) {
                ++stats->strategy_deferred_fallback_calls;
            }
            // Fall through to Auto behavior below by normalizing the strategy.
            options.strategy = CopyStrategy::Auto;
        } else {
            // Default policy: return invalid_state, touch nothing.
            dec.unsupported_requested = true;
            dec.reason = "deferred_not_implemented";
            if (stats) {
                ++stats->strategy_deferred_rejected_calls;
                ++stats->reader_error_stops;
            }
            return make_unexpected<std::uint64_t>(IoError{.code = IoError::Code::invalid_state});
        }
    }

    // Resolve Auto: currently Auto == BufferedFirst (006 made buffered-first the
    // default). Documented and tested; may change after measurement (010). Auto
    // keeps its own requested value but reports what it ran as.
    bool use_fast_path =
        (options.strategy == CopyStrategy::BufferedFirst || options.strategy == CopyStrategy::Auto);
    if (options.strategy == CopyStrategy::Auto) {
        dec.selected = CopyStrategy::BufferedFirst;
        dec.reason = "auto";
    }

    // One strategy-selection counter per top-level call. For a deferred-fallback
    // this runs in addition to strategy_deferred_fallback_calls above (the call
    // was both a deferred fallback AND resolved to Auto/BufferedFirst). counts
    // the SELECTED strategy, so Auto is recorded as Auto even though it executes
    // as BufferedFirst. (007F semantics.)
    if (stats) {
        switch (options.strategy) {
        case CopyStrategy::Auto:
            ++stats->strategy_auto_calls;
            break;
        case CopyStrategy::Scratch:
            ++stats->strategy_scratch_calls;
            break;
        case CopyStrategy::BufferedFirst:
            ++stats->strategy_buffered_first_calls;
            break;
        // Deferred strategies never reach here: rejected returned early, and
        // fallback normalized options.strategy to Auto above (counted as Auto).
        case CopyStrategy::VectorDeferred:
        case CopyStrategy::FileRangeDeferred:
        case CopyStrategy::SendfileDeferred:
        case CopyStrategy::SpliceDeferred:
            break;
        default:
            break; // future enum values: do not double-count
        }
    }

    const CopyLimit& limit = options.limit;

    // nothing() (and bytes(0)): succeed immediately without touching endpoints.
    if (limit.is_limited() && limit.remaining() == 0) {
        if (stats) {
            ++stats->limit_stops;
        }
        return std::uint64_t{0};
    }

    // A non-zero / unlimited copy needs somewhere to stage bytes. An empty
    // scratch can never make progress (the fast path may fully satisfy a limited
    // copy, but a non-empty scratch is still required for the fallback), so
    // reject rather than spin or no-op.
    if (scratch.empty()) {
        return make_unexpected<std::uint64_t>(IoError{.code = IoError::Code::invalid_state});
    }

    // Detect the buffered-readability capability once, but only honor it when
    // the selected strategy wants the fast path (Scratch forces it off).
    BufferedReadable* br = use_fast_path ? dynamic_cast<BufferedReadable*>(&reader) : nullptr;

    std::uint64_t total = 0;
    while (limit.is_unlimited() || total < limit.remaining()) {
        if (stats) {
            ++stats->copy_loop_iterations;
        }

        // --- Buffered fast path: drain already-buffered bytes first. ---
        if (br != nullptr) {
            auto buffered = br->peek_buffered();
            if (!buffered.empty()) {
                // Respect the limit: copy at most (remaining - total) bytes.
                std::size_t allowed = buffered.size();
                if (limit.is_limited()) {
                    std::uint64_t left = limit.remaining() - total;
                    allowed =
                        static_cast<std::size_t>(std::min<std::uint64_t>(buffered.size(), left));
                }
                if (allowed == 0) {
                    // Limit reached on this iteration; fall through to limit stop.
                    break;
                }
                // write_all is all-or-error: on failure it does not expose how
                // many bytes (if any) it wrote. Per the writer-error rule we
                // therefore consume NOTHING on failure and return the error.
                auto wr = writer.write_all(buffered.first(allowed));
                if (!wr.has_value()) {
                    if (stats) {
                        ++stats->writer_error_stops;
                    }
                    return make_unexpected<std::uint64_t>(wr.error());
                }
                auto cr = br->consume_buffered(allowed);
                if (!cr.has_value()) {
                    // Should be impossible: allowed <= buffered.size().
                    if (stats) {
                        ++stats->reader_error_stops;
                    }
                    return make_unexpected<std::uint64_t>(cr.error());
                }
                if (stats) {
                    stats->bytes_read += allowed;
                    stats->bytes_written += allowed;
                    ++stats->buffered_fast_path_calls;
                    stats->buffered_fast_path_bytes += allowed;
                }
                dec.used_buffered_fast_path = true;
                total += allowed;
                continue; // loop: maybe more buffered bytes, maybe limit done
            }
        }

        // --- Scratch fallback: read into scratch, then write_all. ---
        // Never ask the reader for more than the remaining limit allows, and
        // never more than the scratch can hold.
        std::size_t to_read = scratch.size();
        if (limit.is_limited()) {
            std::uint64_t left = limit.remaining() - total;
            to_read = static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), left));
        }

        auto rr = reader.read_some(scratch.first(to_read));
        if (stats) {
            ++stats->scratch_path_calls; // counts the attempt (incl. EOF probe)
        }
        if (!rr.has_value()) {
            if (stats) {
                ++stats->reader_error_stops;
            }
            return make_unexpected<std::uint64_t>(rr.error());
        }
        std::size_t got = rr.value();
        if (got == 0) {
            if (stats) {
                ++stats->eof_stops;
            }
            return total; // clean EOF
        }
        if (got > to_read) {
            // Defensive: a reader returning more than asked is broken.
            if (stats) {
                ++stats->reader_error_stops;
            }
            return make_unexpected<std::uint64_t>(IoError{.code = IoError::Code::invalid_state});
        }
        if (stats) {
            stats->bytes_read += got;
            stats->scratch_path_bytes += got;
        }
        auto wr = writer.write_all(std::span<const std::byte>(scratch.data(), got));
        if (!wr.has_value()) {
            if (stats) {
                ++stats->writer_error_stops;
            }
            return make_unexpected<std::uint64_t>(wr.error());
        }
        if (stats) {
            stats->bytes_written += got;
        }
        dec.used_scratch_path = true;
        total += got;
    }
    // Loop exited because the limit was reached (not EOF/error).
    if (stats) {
        ++stats->limit_stops;
    }
    return total;
}

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch,
                               CopyLimit limit, CopyStats* stats) {
    return copy_all(reader, writer, scratch,
                    CopyOptions{.limit = limit, .strategy = CopyStrategy::Auto}, stats);
}

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch) {
    return copy_all(reader, writer, scratch, CopyLimit::unlimited(), nullptr);
}

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, CopyLimit limit) {
    std::array<std::byte, 8192> scratch{};
    return copy_all(reader, writer, std::span<std::byte>(scratch), limit, nullptr);
}

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer) {
    return copy_all(reader, writer, CopyLimit::unlimited());
}

} // namespace sluice
