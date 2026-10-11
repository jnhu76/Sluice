#include "grep_task.hpp"

#include "matcher.hpp"

#include <sluice/blocking/file.hpp>
#include <sluice/async/await_op_helpers.hpp>
#include <sluice/async/file.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <limits>
#include <memory>
#include <span>
#include <utility>

namespace sluice_grep {

namespace {

using namespace sluice::async;
using sluice::IoError;

struct GrepTask {
    std::string pattern;
    std::vector<GrepInput> inputs;
    std::size_t max_line_bytes;
    MatchSink sink;
    std::vector<std::uint8_t> buffer;
    std::vector<GrepFileResult> results;

    void finish_canceled(std::size_t i) {
        GrepFileResult r;
        r.path = inputs[i].path;
        r.error = IoError{IoError::Code::canceled};
        results.push_back(std::move(r));
    }

    template <class Read> void scan_one(std::size_t idx, Read&& read) {
        const GrepInput& in = inputs[idx];
        GrepFileResult out;
        out.path = in.path;

        LineMatcher matcher(pattern, max_line_bytes);
        std::vector<MatchEvent> events;

        std::uint64_t offset = 0;
        bool terminal = false;
        while (!terminal) {
            auto rr = read(offset, std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()),
                                                        buffer.size()));
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

    void operator()(RuntimeTaskContext& ctx,
                    TaskResultSlot<sluice::Result<std::vector<GrepFileResult>>>& slot) {
        try {
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                if (ctx.cancel_token().is_requested()) {
                    finish_canceled(i);
                    continue;
                }
                Completion<std::size_t> completion;
                scan_one(i, [&](std::uint64_t offset, std::span<std::byte> dst) {
                    if (ctx.cancel_token().is_requested())
                        return sluice::make_unexpected<std::size_t>(
                            IoError{IoError::Code::canceled});
                    return await_read_at(inputs[i].file, ctx, offset, dst, completion);
                });
            }
        } catch (...) {
            auto translated = translate_task_exception<std::vector<GrepFileResult>>();
            IoError err = translated.error();
            for (std::size_t i = results.size(); i < inputs.size(); ++i) {
                GrepFileResult r;
                r.path = inputs[i].path;
                r.error = err;
                results.push_back(std::move(r));
            }
        }
        slot.publish(std::move(results));
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

std::vector<GrepFileResult> run_grep_engine(const std::string& pattern,
                                            std::vector<GrepInput> inputs, std::size_t buffer_size,
                                            std::size_t max_line_bytes, unsigned workers,
                                            MatchSink sink, std::unique_ptr<AsyncBackend> backend) {
    if (!valid_config(buffer_size, max_line_bytes, workers) || !backend) {
        return all_error(inputs, IoError{IoError::Code::invalid_state});
    }

    std::vector<std::uint8_t> buffer;
    try {
        buffer.assign(buffer_size, 0);
    } catch (const std::bad_alloc&) {
        return all_error(inputs, IoError{IoError::Code::no_space});
    }

    GrepTask task{pattern,         std::move(inputs), max_line_bytes,
                  std::move(sink), std::move(buffer), {}};

    auto result =
        run_task_to_result<std::vector<GrepFileResult>>(workers, std::move(backend), task);
    if (!result.has_value())
        return all_error(task.inputs, result.error());
    return std::move(result.value());
}

}

std::vector<GrepFileResult> grep_files(const std::string& pattern, std::vector<GrepInput> inputs,
                                       std::size_t buffer_size, std::size_t max_line_bytes,
                                       unsigned workers, MatchSink sink) {
    if (!valid_config(buffer_size, max_line_bytes, workers))
        return all_error(inputs, IoError{IoError::Code::invalid_state});
    if (workers != 1)
        return run_grep_engine(pattern, std::move(inputs), buffer_size, max_line_bytes, workers,
                               std::move(sink), std::make_unique<ThreadPoolBackend>());

    std::vector<std::uint8_t> buffer;
    try {
        buffer.assign(buffer_size, 0);
    } catch (const std::bad_alloc&) {
        return all_error(inputs, IoError{IoError::Code::no_space});
    }
    GrepTask task{pattern,         std::move(inputs), max_line_bytes,
                  std::move(sink), std::move(buffer), {}};
    task.results.reserve(task.inputs.size());
    try {
        for (std::size_t i = 0; i < task.inputs.size(); ++i)
            task.scan_one(i, [&](std::uint64_t offset, std::span<std::byte> dst) {
                return sluice::blocking::read_at(task.inputs[i].file, offset, dst);
            });
    } catch (...) {
        auto translated = translate_task_exception<std::vector<GrepFileResult>>();
        for (std::size_t i = task.results.size(); i < task.inputs.size(); ++i) {
            GrepFileResult out;
            out.path = task.inputs[i].path;
            out.error = translated.error();
            task.results.push_back(std::move(out));
        }
    }
    return std::move(task.results);
}

std::vector<GrepFileResult>
grep_files_with_backend(const std::string& pattern, std::vector<GrepInput> inputs,
                        std::size_t buffer_size, std::size_t max_line_bytes, unsigned workers,
                        MatchSink sink, std::unique_ptr<sluice::async::AsyncBackend> backend) {
    return run_grep_engine(pattern, std::move(inputs), buffer_size, max_line_bytes, workers,
                           std::move(sink), std::move(backend));
}

}
