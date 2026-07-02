// copy_all implementation. The primary overload drives the bounded loop; the
// back-compat / convenience overloads delegate to it. The loop tries the
// buffered fast path first (CPPIO-CORE-006D): when the reader also implements
// BufferedReadable, already-buffered unread bytes are drained via
// peek_buffered/consume_buffered before any scratch read, mirroring Zig
// std.Io's Reader.stream (Reader.zig:168).
#include <cppio/copy.hpp>
#include <cppio/buffered_readable.hpp>

#include <algorithm>
#include <array>

namespace cppio {

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch,
                               CopyLimit limit, CopyStats* stats) {
    if (stats) ++stats->copy_calls;

    // nothing() (and bytes(0)): succeed immediately without touching endpoints.
    if (limit.is_limited() && limit.remaining() == 0) {
        if (stats) ++stats->limit_stops;
        return std::uint64_t{0};
    }

    // A non-zero / unlimited copy needs somewhere to stage bytes. An empty
    // scratch can never make progress (the fast path may fully satisfy a limited
    // copy, but a non-empty scratch is still required for the fallback), so
    // reject rather than spin or no-op.
    if (scratch.empty()) {
        return make_unexpected<std::uint64_t>(IoError{IoError::Code::invalid_state});
    }

    // Detect the buffered-readability capability once. A reader that does not
    // implement BufferedReadable (FileReader, MemoryReader, FaultReader, ...)
    // yields nullptr and the classic scratch loop runs unchanged.
    auto* br = dynamic_cast<BufferedReadable*>(&reader);

    std::uint64_t total = 0;
    while (limit.is_unlimited() || total < limit.remaining()) {
        if (stats) ++stats->copy_loop_iterations;

        // --- Buffered fast path: drain already-buffered bytes first. ---
        if (br != nullptr) {
            auto buffered = br->peek_buffered();
            if (!buffered.empty()) {
                // Respect the limit: copy at most (remaining - total) bytes.
                std::size_t allowed = buffered.size();
                if (limit.is_limited()) {
                    std::uint64_t left = limit.remaining() - total;
                    allowed = static_cast<std::size_t>(
                        std::min<std::uint64_t>(buffered.size(), left));
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
                    if (stats) ++stats->writer_error_stops;
                    return make_unexpected<std::uint64_t>(wr.error());
                }
                auto cr = br->consume_buffered(allowed);
                if (!cr.has_value()) {
                    // Should be impossible: allowed <= buffered.size().
                    if (stats) ++stats->reader_error_stops;
                    return make_unexpected<std::uint64_t>(cr.error());
                }
                if (stats) {
                    stats->bytes_read += allowed;
                    stats->bytes_written += allowed;
                    ++stats->buffered_fast_path_calls;
                    stats->buffered_fast_path_bytes += allowed;
                }
                total += allowed;
                continue;  // loop: maybe more buffered bytes, maybe limit done
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
        if (stats) ++stats->scratch_path_calls;  // counts the attempt (incl. EOF probe)
        if (!rr.has_value()) {
            if (stats) ++stats->reader_error_stops;
            return make_unexpected<std::uint64_t>(rr.error());
        }
        std::size_t got = rr.value();
        if (got == 0) {
            if (stats) ++stats->eof_stops;
            return total;  // clean EOF
        }
        if (got > to_read) {
            // Defensive: a reader returning more than asked is broken.
            if (stats) ++stats->reader_error_stops;
            return make_unexpected<std::uint64_t>(IoError{IoError::Code::invalid_state});
        }
        if (stats) {
            stats->bytes_read += got;
            stats->scratch_path_bytes += got;
        }
        auto wr = writer.write_all(std::span<const std::byte>(scratch.data(), got));
        if (!wr.has_value()) {
            if (stats) ++stats->writer_error_stops;
            return make_unexpected<std::uint64_t>(wr.error());
        }
        if (stats) stats->bytes_written += got;
        total += got;
    }
    // Loop exited because the limit was reached (not EOF/error).
    if (stats) ++stats->limit_stops;
    return total;
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

}  // namespace cppio
