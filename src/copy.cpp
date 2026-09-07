#include <sluice/copy.hpp>
#include <sluice/buffered_readable.hpp>

#include <algorithm>
#include <array>

namespace sluice {

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

    bool use_fast_path =
        (options.strategy == CopyStrategy::BufferedFirst || options.strategy == CopyStrategy::Auto);
    if (options.strategy == CopyStrategy::Auto) {
        dec.selected = CopyStrategy::BufferedFirst;
        dec.reason = "auto";
    }

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
        }
    }

    const CopyLimit& limit = options.limit;

    if (limit.is_limited() && limit.remaining() == 0) {
        if (stats) {
            ++stats->limit_stops;
        }
        return std::uint64_t{0};
    }

    if (scratch.empty()) {
        return make_unexpected<std::uint64_t>(IoError{.code = IoError::Code::invalid_state});
    }

    BufferedReadable* br = use_fast_path ? dynamic_cast<BufferedReadable*>(&reader) : nullptr;

    std::uint64_t total = 0;
    while (limit.is_unlimited() || total < limit.remaining()) {
        if (stats) {
            ++stats->copy_loop_iterations;
        }

        if (br != nullptr) {
            auto buffered = br->peek_buffered();
            if (!buffered.empty()) {
                std::size_t allowed = buffered.size();
                if (limit.is_limited()) {
                    std::uint64_t left = limit.remaining() - total;
                    allowed =
                        static_cast<std::size_t>(std::min<std::uint64_t>(buffered.size(), left));
                }
                if (allowed == 0) {
                    break;
                }

                auto wr = writer.write_all(buffered.first(allowed));
                if (!wr.has_value()) {
                    if (stats) {
                        ++stats->writer_error_stops;
                    }
                    return make_unexpected<std::uint64_t>(wr.error());
                }
                auto cr = br->consume_buffered(allowed);
                if (!cr.has_value()) {
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
                continue;
            }
        }

        std::size_t to_read = scratch.size();
        if (limit.is_limited()) {
            std::uint64_t left = limit.remaining() - total;
            to_read = static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), left));
        }

        auto rr = reader.read_some(scratch.first(to_read));
        if (stats) {
            ++stats->scratch_path_calls;
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
            return total;
        }
        if (got > to_read) {
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
