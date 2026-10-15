#include "grep_task.hpp"

#include "matcher.hpp"

#include <sluice/blocking/file.hpp>

#include <limits>
#include <new>
#include <system_error>
#include <span>
#include <utility>

namespace sluice_grep {

namespace {

using sluice::IoError;

IoError current_exception_error() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return IoError{IoError::Code::no_space};
    } catch (const std::system_error& error) {
        const int native = error.code().value();
        return IoError{sluice::detail::canonical_error_code(native), native};
    } catch (...) {
        return IoError{IoError::Code::backend_error};
    }
}

struct GrepScan {
    std::string pattern;
    std::vector<GrepInput> inputs;
    std::size_t max_line_bytes;
    MatchSink sink;
    std::vector<std::uint8_t> buffer;
    std::vector<GrepFileResult> results;

    void scan_one(std::size_t idx) {
        const GrepInput& in = inputs[idx];
        GrepFileResult out;
        out.path = in.path;

        LineMatcher matcher(pattern, max_line_bytes);
        std::vector<MatchEvent> events;

        std::uint64_t offset = 0;
        bool terminal = false;
        while (!terminal) {
            auto rr = sluice::blocking::read_at(
                in.file, offset,
                std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()), buffer.size()));
            if (!rr.has_value()) {
                out.error = rr.error();
                break;
            }
            std::size_t n = rr.value();
            if (n == 0) {
                events.clear();
                matcher.finish(events);
                for (auto& e : events) {
                    if (sink)
                        sink(in.path, e.line_no, e.line);
                    ++out.match_count;
                }
                terminal = true;
                break;
            }
            if (offset > std::numeric_limits<std::uint64_t>::max() - n) {
                out.error = IoError{IoError::Code::invalid_state};
                break;
            }
            events.clear();
            matcher.feed(buffer.data(), n, events);
            for (auto& e : events) {
                if (sink)
                    sink(in.path, e.line_no, e.line);
                ++out.match_count;
            }
            offset += n;
        }
        out.lines_scanned = matcher.complete_lines();
        out.dropped_long_lines = matcher.dropped_long_lines();
        results.push_back(std::move(out));
    }
};

std::vector<GrepFileResult> all_error(const std::vector<GrepInput>& inputs, IoError e) {
    std::vector<GrepFileResult> out;
    out.reserve(inputs.size());
    for (auto& in : inputs) {
        GrepFileResult r;
        r.path = in.path;
        r.error = e;
        out.push_back(std::move(r));
    }
    return out;
}

}

std::vector<GrepFileResult> grep_files(const std::string& pattern, std::vector<GrepInput> inputs,
                                       std::size_t buffer_size, std::size_t max_line_bytes,
                                       MatchSink sink) {
    if (!valid_config(buffer_size, max_line_bytes))
        return all_error(inputs, IoError{IoError::Code::invalid_state});
    std::vector<std::uint8_t> buffer;
    try {
        buffer.assign(buffer_size, 0);
    } catch (const std::bad_alloc&) {
        return all_error(inputs, IoError{IoError::Code::no_space});
    }
    GrepScan scan{pattern,         std::move(inputs), max_line_bytes,
                  std::move(sink), std::move(buffer), {}};
    try {
        scan.results.reserve(scan.inputs.size());
        for (std::size_t i = 0; i < scan.inputs.size(); ++i)
            scan.scan_one(i);
    } catch (...) {
        const IoError error = current_exception_error();
        for (std::size_t i = scan.results.size(); i < scan.inputs.size(); ++i) {
            GrepFileResult out;
            out.path = scan.inputs[i].path;
            out.error = error;
            scan.results.push_back(std::move(out));
        }
    }
    return std::move(scan.results);
}

}
