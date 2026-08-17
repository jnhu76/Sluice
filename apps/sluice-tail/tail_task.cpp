// sluice-tail engine implementation.
//
// Task shape: one Runtime task does the backward last-N scan and (with -f)
// the follow loop; the terminal outcome is published through an app-owned
// slot. The owner thread calls wait() — unlike the run-to-completion apps it
// does NOT request stop BEFORE the task finishes: for follow mode the task
// is long-lived and stop is exactly how it ends (from a signal waiter
// thread). The task itself never observes root cancellation as an error: a
// stopped follow is the normal exit.
#include "tail_task.hpp"

#include <sluice/async/threadpool_backend.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

namespace sluice_tail {

namespace {

using namespace sluice::async;
using sluice::IoError;

// Bounded line assembler with a match-all policy: reuses the grep shape (a
// single carry buffer, complete lines emitted in order, long lines dropped
// with a flag). Kept app-local: sharing grep's matcher across apps would be
// a premature abstraction (track rule: promote only after real duplication).
struct LineAssembler {
    std::size_t max_line_bytes;
    std::string carry;
    bool dropping = false;
    bool dropped_long = false;

    explicit LineAssembler(std::size_t cap) : max_line_bytes(cap) {
        carry.reserve(cap + 1);
    }

    // Feed a chunk; append complete lines (without '\n') to `out`.
    void feed(const std::uint8_t* data, std::size_t len,
              std::vector<std::string>& out) {
        std::size_t i = 0;
        while (i < len) {
            const void* nl = std::memchr(data + i, '\n', len - i);
            if (nl == nullptr) {
                std::size_t rest = len - i;
                if (dropping) return;
                if (carry.size() + rest <= max_line_bytes) {
                    carry.append(reinterpret_cast<const char*>(data + i), rest);
                } else {
                    dropped_long = true;
                    dropping = true;
                    carry.clear();
                }
                return;
            }
            std::size_t nl_off = static_cast<const std::uint8_t*>(nl) - data;
            std::size_t piece = nl_off - i;
            if (dropping) {
                dropping = false;  // newline ends the dropped line
                carry.clear();
            } else if (!carry.empty()) {
                if (carry.size() + piece <= max_line_bytes) {
                    carry.append(reinterpret_cast<const char*>(data + i), piece);
                    out.push_back(std::move(carry));
                    carry.clear();
                } else {
                    dropped_long = true;
                    carry.clear();
                }
            } else if (piece <= max_line_bytes) {
                out.emplace_back(reinterpret_cast<const char*>(data + i), piece);
            } else {
                dropped_long = true;
            }
            i = nl_off + 1;
        }
    }

    // EOF: a non-empty carry is the final unterminated line.
    void finish(std::vector<std::string>& out) {
        if (dropping) {
            dropping = false;
        } else if (!carry.empty()) {
            out.push_back(std::move(carry));
            carry.clear();
        }
    }

    void reset_partial() {
        carry.clear();
        dropping = false;
    }
};

struct TailTask {
    int fd;
    TailOptions options;
    LineSink sink;
    DiagSink diag;
    std::vector<std::uint8_t> buffer;

    std::mutex& mtx;
    std::optional<TailResult>& out;
    std::atomic<bool>& done;
    std::condition_variable& done_cv;

    void publish(TailResult r) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!done.load(std::memory_order::relaxed)) {
                out = std::move(r);
                done.store(true, std::memory_order::release);
            }
        }
        done_cv.notify_all();
    }

    void emit(std::vector<std::string>& lines, TailResult& r) {
        for (auto& l : lines) {
            if (sink) sink(l);
            ++r.lines_emitted;
        }
        lines.clear();
    }

    void diag_msg(const char* msg) {
        if (diag) diag(msg);
    }

    // Await one outstanding read Completion to terminal. Returns the byte
    // count (0 = EOF) or an error.
    sluice::Result<std::size_t> read_at(RuntimeTaskContext& ctx,
                                        Completion<std::size_t>& rc,
                                        std::uint64_t offset) {
        auto sr = ctx.submit_read(
            ReadOp{fd, reinterpret_cast<std::byte*>(buffer.data()),
                   buffer.size(), offset},
            rc);
        if (!sr.has_value())
            return make_unexpected<std::size_t>(sr.error());
        auto wr = ctx.await_completion(rc);
        if (!wr.has_value())
            return make_unexpected<std::size_t>(wr.error());
        auto rr = rc.result();
        rc.reset();
        if (!rr.has_value())
            return make_unexpected<std::size_t>(rr.error());
        return rr.value();
    }

    // Backward scan: find the offset where the last `n` lines start. Reads
    // descending windows; counts newlines; never buffers more than one
    // window. The file-final '\n' (if any) closes the last line and does not
    // count as a separator.
    sluice::Result<std::uint64_t> find_last_lines_offset(
        RuntimeTaskContext& ctx, std::uint64_t size, std::size_t n) {
        if (size == 0 || n == 0) return size;

        // Whether the file's last byte is '\n' (skip it as a separator).
        bool skip_final_nl = false;
        {
            Completion<std::size_t> rc;
            std::uint8_t last = 0;
            // One 1-byte positional read through the same async path keeps
            // every I/O on the backend (no direct pread in the task).
            auto sr = ctx.submit_read(
                ReadOp{fd, reinterpret_cast<std::byte*>(&last), 1, size - 1},
                rc);
            if (!sr.has_value())
                return make_unexpected<std::uint64_t>(sr.error());
            auto wr = ctx.await_completion(rc);
            if (!wr.has_value())
                return make_unexpected<std::uint64_t>(wr.error());
            auto rr = rc.result();
            rc.reset();
            if (!rr.has_value())
                return make_unexpected<std::uint64_t>(rr.error());
            skip_final_nl = (last == '\n');
        }

        std::size_t count = 0;
        std::uint64_t pos = size;
        Completion<std::size_t> rc;
        while (pos > 0) {
            if (ctx.cancel_token().is_requested())
                return size;  // stop quickly; caller treats cancel as clean
            std::uint64_t lo = (pos > buffer.size()) ? pos - buffer.size() : 0;
            // The LAST window is short: read exactly the window [lo, pos), not
            // buffer.size() bytes — a full-length read would cross `pos` into
            // already-scanned territory (double-counted newlines) and trip
            // the exact-length guard below.
            std::size_t want = static_cast<std::size_t>(pos - lo);
            auto sr = ctx.submit_read(
                ReadOp{fd, reinterpret_cast<std::byte*>(buffer.data()), want,
                       lo},
                rc);
            if (!sr.has_value())
                return make_unexpected<std::uint64_t>(sr.error());
            auto wr = ctx.await_completion(rc);
            if (!wr.has_value())
                return make_unexpected<std::uint64_t>(wr.error());
            auto rr = rc.result();
            rc.reset();
            if (!rr.has_value())
                return make_unexpected<std::uint64_t>(rr.error());
            std::size_t got = rr.value();
            if (got != want)
                return make_unexpected<std::uint64_t>(
                    IoError{IoError::Code::backend_error});
            for (std::size_t i = got; i-- > 0;) {
                if (buffer[i] == '\n') {
                    std::uint64_t abs_off = lo + i;
                    if (skip_final_nl && abs_off == size - 1) {
                        skip_final_nl = false;
                        continue;
                    }
                    ++count;
                    if (count >= n) return abs_off + 1;
                }
            }
            pos = lo;
        }
        return 0;  // fewer than n lines in the whole file
    }

    void operator()(RuntimeTaskContext& ctx) {
        TailResult r;
        try {
            run(ctx, r);
        } catch (...) {
            if (!r.error.has_value())
                r.error = IoError{IoError::Code::backend_error};
        }
        publish(std::move(r));
    }

    void run(RuntimeTaskContext& ctx, TailResult& r) {
        // Snapshot the size at scan start; follow begins at this EOF.
        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            r.error = sluice::from_errno_value(errno);
            return;
        }
        std::uint64_t size = static_cast<std::uint64_t>(st.st_size);

        // ---- Phase 1: last-N. ----
        if (options.lines > 0) {
            auto start_r = find_last_lines_offset(ctx, size, options.lines);
            if (!start_r.has_value()) {
                r.error = start_r.error();
                return;
            }
            if (ctx.cancel_token().is_requested()) {
                r.stopped_by_cancel = true;
                return;  // clean stop during scan
            }
            // Forward stream from the scan start through the assembler.
            LineAssembler asmbl(options.max_line_bytes);
            std::vector<std::string> lines;
            Completion<std::size_t> rc;
            std::uint64_t off = start_r.value();
            while (off < size) {
                auto rr = read_at(ctx, rc, off);
                if (!rr.has_value()) {
                    r.error = rr.error();
                    return;
                }
                std::size_t n = rr.value();
                if (n == 0) break;  // defensive: file shrank mid-scan
                asmbl.feed(buffer.data(), n, lines);
                emit(lines, r);
                off += n;
            }
            asmbl.finish(lines);
            emit(lines, r);
            r.dropped_long_lines = asmbl.dropped_long;
            if (asmbl.dropped_long)
                diag_msg("line longer than --max-line-bytes skipped\n");
        }

        if (!options.follow) return;  // finite tail: done

        // ---- Phase 2: follow. ----
        LineAssembler asmbl(options.max_line_bytes);
        std::vector<std::string> lines;
        Completion<std::size_t> rc;
        std::uint64_t off = size;
        const auto slice = std::chrono::milliseconds(50);
        while (true) {
            if (ctx.cancel_token().is_requested()) {
                r.stopped_by_cancel = true;
                return;  // the documented normal end of tail -f
            }
            auto rr = read_at(ctx, rc, off);
            if (!rr.has_value()) {
                r.error = rr.error();
                return;
            }
            std::size_t n = rr.value();
            if (n > 0) {
                asmbl.feed(buffer.data(), n, lines);
                emit(lines, r);
                off += n;
                continue;
            }

            // EOF: bounded wait, re-stat, detect growth/truncation. The
            // sleep is sliced so a stop request is noticed within ~50ms
            // without busy polling (one stat per poll interval).
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(options.poll_interval_ms);
            while (std::chrono::steady_clock::now() < deadline) {
                if (ctx.cancel_token().is_requested()) break;
                std::this_thread::sleep_for(slice);
            }

            struct stat st2{};
            if (::fstat(fd, &st2) != 0) {
                r.error = sluice::from_errno_value(errno);
                return;
            }
            std::uint64_t cur = static_cast<std::uint64_t>(st2.st_size);
            if (cur < off) {
                diag_msg("file truncated\n");
                r.truncation_detected = true;
                off = 0;
                asmbl.reset_partial();  // the partial line may never complete
            }
            // Growth (cur > off) or same size: loop reads at `off`; a same-
            // size EOF just parks for another interval (one stat per wake).
        }
    }
};

}  // namespace

// Impl is defined at sluice_tail scope (not inside the anonymous namespace):
// TailEngine::Impl is a member of a class in this namespace, and a nested-
// name definition must appear in an enclosing namespace.
struct TailEngine::Impl {
    int fd;
    TailOptions options;
    LineSink sink;
    DiagSink diag;
    std::unique_ptr<ApplicationRuntime> rt;
    std::mutex mtx;
    std::condition_variable done_cv;
    std::optional<TailResult> out;
    std::atomic<bool> done{false};
    std::atomic<bool> started{false};
    std::atomic<bool> waited{false};
    std::vector<std::uint8_t> buffer;
};

namespace {

bool options_valid(const TailOptions& o) {
    return o.lines <= kMaxLines &&
           o.buffer_size >= kMinBufferSize && o.buffer_size <= kMaxBufferSize &&
           o.max_line_bytes > 0 && o.max_line_bytes <= kMaxMaxLineBytes &&
           o.workers > 0 && o.workers <= kMaxWorkers &&
           o.poll_interval_ms >= kMinPollMs &&
           o.poll_interval_ms <= kMaxPollMs;
}

}  // namespace

TailEngine::TailEngine(int fd, TailOptions options, LineSink sink,
                       DiagSink diag)
    : impl_(std::make_unique<Impl>()) {
    impl_->fd = fd;
    impl_->options = options;
    impl_->sink = std::move(sink);
    impl_->diag = std::move(diag);
}

TailEngine::~TailEngine() = default;

sluice::Result<void> TailEngine::start() {
    if (!options_valid(impl_->options))
        return sluice::make_unexpected<void>(
            IoError{IoError::Code::invalid_state});
    try {
        impl_->buffer.assign(impl_->options.buffer_size, 0);
    } catch (const std::bad_alloc&) {
        return sluice::make_unexpected<void>(IoError{IoError::Code::no_space});
    }

    RuntimeBuilder builder;
    builder.backend(std::make_unique<ThreadPoolBackend>());
    builder.workers(impl_->options.workers);
    try {
        auto build_r = builder.build();
        if (!build_r.has_value())
            return sluice::make_unexpected<void>(build_r.error());
        impl_->rt = std::move(build_r.value());
        auto start_r = impl_->rt->start();
        if (!start_r.has_value())
            return sluice::make_unexpected<void>(start_r.error());
    } catch (...) {
        if (impl_->rt) (void)impl_->rt->shutdown();
        return sluice::make_unexpected<void>(IoError{IoError::Code::no_space});
    }

    TailTask task{impl_->fd,
                  impl_->options,
                  impl_->sink,
                  impl_->diag,
                  std::move(impl_->buffer),
                  impl_->mtx,
                  impl_->out,
                  impl_->done,
                  impl_->done_cv};
    // The task captures buffer by move; hand the Runtime a heap-kept copy of
    // the state it needs (the task object itself must outlive the run).
    auto sub_r = impl_->rt->submit(
        [t = std::move(task)](RuntimeTaskContext& ctx) mutable { t(ctx); });
    if (!sub_r.has_value()) {
        (void)impl_->rt->shutdown();
        return sluice::make_unexpected<void>(sub_r.error());
    }
    impl_->started.store(true, std::memory_order::release);
    return {};
}

void TailEngine::request_stop() noexcept {
    if (impl_->started.load(std::memory_order::acquire) && impl_->rt)
        impl_->rt->request_stop();  // noexcept, idempotent, worker-safe
}

sluice::Result<TailResult> TailEngine::wait() {
    if (!impl_->started.load(std::memory_order::acquire))
        return sluice::make_unexpected<TailResult>(
            IoError{IoError::Code::invalid_state});
    bool already = false;
    if (!impl_->waited.compare_exchange_strong(already, true)) {
        return sluice::make_unexpected<TailResult>(
            IoError{IoError::Code::invalid_state});
    }

    {
        std::unique_lock<std::mutex> lk(impl_->mtx);
        impl_->done_cv.wait(lk, [&] {
            return impl_->done.load(std::memory_order::acquire);
        });
    }
    // The task is terminal; now close the Runtime lifecycle.
    impl_->rt->request_stop();
    (void)impl_->rt->drain();
    (void)impl_->rt->join();

    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (!impl_->out.has_value())
        return sluice::make_unexpected<TailResult>(
            IoError{IoError::Code::invalid_state});
    return std::move(impl_->out.value());
}

}  // namespace sluice_tail
