// sluice-grep scanning engine implementation (same Runtime shape as
// sluice-copy/sluice-hash: one task, app-owned terminal slot, stop only
// after the task publishes).
#include "grep_task.hpp"

#include "matcher.hpp"

#include <sluice/async/threadpool_backend.hpp>

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <utility>

namespace sluice_grep {

namespace {

using namespace sluice::async;
using sluice::IoError;

struct GrepTask {
    std::string pattern;
    std::vector<GrepInput> inputs;
    std::size_t buffer_size;
    std::size_t max_line_bytes;
    MatchSink sink;
    std::vector<std::uint8_t> buffer;
    std::vector<GrepFileResult> results;

    std::mutex& mtx;
    bool& done;
    std::condition_variable& done_cv;

    void publish() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            done = true;
        }
        done_cv.notify_all();
    }

    void finish_canceled(std::size_t i) {
        GrepFileResult r;
        r.path = inputs[i].path;
        r.error = IoError{IoError::Code::canceled};
        results.push_back(std::move(r));
    }

    // Scan one file: positional async reads feed the LineMatcher; complete
    // matching lines go to the sink immediately (streaming output).
    void scan_one(RuntimeTaskContext& ctx, std::size_t idx) {
        const GrepInput& in = inputs[idx];
        GrepFileResult out;
        out.path = in.path;

        LineMatcher matcher(pattern, max_line_bytes);
        std::vector<MatchEvent> events;  // reused per-chunk scratch

        Completion<std::size_t> rc;
        std::uint64_t offset = 0;
        bool terminal = false;
        while (!terminal) {
            if (ctx.cancel_token().is_requested()) {
                out.error = IoError{IoError::Code::canceled};
                break;
            }
            auto sr = ctx.submit_read(
                ReadOp{in.fd,
                       reinterpret_cast<std::byte*>(buffer.data()),
                       buffer.size(), offset},
                rc);
            if (!sr.has_value()) {
                out.error = sr.error();
                break;
            }
            auto wr = ctx.await_completion(rc);
            if (!wr.has_value()) {
                out.error = wr.error();
                break;
            }
            auto rr = rc.result();
            rc.reset();
            if (!rr.has_value()) {
                out.error = rr.error();
                break;
            }
            std::size_t n = rr.value();
            if (n == 0) {
                // EOF: flush the final (unterminated) line.
                events.clear();
                matcher.finish(events);
                for (auto& e : events) {
                    if (sink) sink(in.path, e.line_no, e.line);
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
                if (sink) sink(in.path, e.line_no, e.line);
                ++out.match_count;
            }
            offset += n;
        }
        out.lines_scanned = matcher.complete_lines();
        out.dropped_long_lines = matcher.dropped_long_lines();
        results.push_back(std::move(out));
    }

    void operator()(RuntimeTaskContext& ctx) {
        // Exception boundary (same rationale as copy_task/hash_task): every
        // input gets exactly one result entry on every path.
        try {
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                if (ctx.cancel_token().is_requested()) {
                    finish_canceled(i);
                    continue;
                }
                scan_one(ctx, i);
            }
        } catch (...) {
            for (std::size_t i = results.size(); i < inputs.size(); ++i) {
                GrepFileResult r;
                r.path = inputs[i].path;
                r.error = IoError{IoError::Code::backend_error};
                results.push_back(std::move(r));
            }
        }
        publish();
    }
};

bool config_invalid(std::size_t buffer_size, std::size_t max_line_bytes,
                    unsigned workers) {
    return buffer_size < kMinBufferSize || buffer_size > kMaxBufferSize ||
           max_line_bytes == 0 || max_line_bytes > kMaxMaxLineBytes ||
           workers == 0 || workers > kMaxWorkers;
}

std::vector<GrepFileResult> all_error(const std::vector<GrepInput>& inputs,
                                      IoError e) {
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

std::vector<GrepFileResult> run_grep_engine(
    const std::string& pattern, std::vector<GrepInput> inputs,
    std::size_t buffer_size, std::size_t max_line_bytes, unsigned workers,
    MatchSink sink, std::unique_ptr<AsyncBackend> backend) {
    // Argument validation BEFORE any allocation or Runtime build.
    if (config_invalid(buffer_size, max_line_bytes, workers) || !backend) {
        return all_error(inputs, IoError{IoError::Code::invalid_state});
    }

    std::vector<std::uint8_t> buffer;
    try {
        buffer.assign(buffer_size, 0);
    } catch (const std::bad_alloc&) {
        return all_error(inputs, IoError{IoError::Code::no_space});
    }

    RuntimeBuilder builder;
    builder.backend(std::move(backend));
    builder.workers(workers);

    std::unique_ptr<ApplicationRuntime> rt;
    try {
        auto build_r = builder.build();
        if (!build_r.has_value())
            return all_error(inputs, build_r.error());
        rt = std::move(build_r.value());
        auto start_r = rt->start();
        if (!start_r.has_value())
            return all_error(inputs, start_r.error());
    } catch (...) {
        if (rt) (void)rt->shutdown();
        return all_error(inputs, IoError{IoError::Code::no_space});
    }

    std::mutex mtx;
    std::condition_variable done_cv;
    bool done = false;

    GrepTask task{pattern,          std::move(inputs), buffer_size,
                  max_line_bytes,   std::move(sink),   std::move(buffer),
                  {},               mtx,               done,
                  done_cv};

    auto sub_r = rt->submit(std::ref(task));
    if (!sub_r.has_value()) {
        (void)rt->shutdown();
        return all_error(task.inputs, sub_r.error());
    }

    {
        std::unique_lock<std::mutex> wlk(mtx);
        done_cv.wait(wlk, [&] { return done; });
    }
    rt->request_stop();
    (void)rt->drain();
    (void)rt->join();

    return std::move(task.results);
}

}  // namespace

std::vector<GrepFileResult> grep_files(
    const std::string& pattern, std::vector<GrepInput> inputs,
    std::size_t buffer_size, std::size_t max_line_bytes, unsigned workers,
    MatchSink sink) {
    return run_grep_engine(pattern, std::move(inputs), buffer_size,
                           max_line_bytes, workers, std::move(sink),
                           std::make_unique<ThreadPoolBackend>());
}

std::vector<GrepFileResult> grep_files_with_backend(
    const std::string& pattern, std::vector<GrepInput> inputs,
    std::size_t buffer_size, std::size_t max_line_bytes, unsigned workers,
    MatchSink sink, std::unique_ptr<sluice::async::AsyncBackend> backend) {
    return run_grep_engine(pattern, std::move(inputs), buffer_size,
                           max_line_bytes, workers, std::move(sink),
                           std::move(backend));
}

}  // namespace sluice_grep
