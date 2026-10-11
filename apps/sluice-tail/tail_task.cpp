#include "tail_task.hpp"

#include <sluice/blocking/file.hpp>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <system_error>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace sluice_tail {

namespace {

using sluice::IoError;

struct LineAssembler {
    std::size_t max_line_bytes;
    std::string carry;
    bool dropping = false;
    bool dropped_long = false;

    explicit LineAssembler(std::size_t cap) : max_line_bytes(cap) { carry.reserve(cap + 1); }

    void feed(const std::uint8_t* data, std::size_t len, std::vector<std::string>& out) {
        std::size_t i = 0;
        while (i < len) {
            const void* nl = std::memchr(data + i, '\n', len - i);
            if (nl == nullptr) {
                std::size_t rest = len - i;
                if (dropping)
                    return;
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
                dropping = false;
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

struct DriverState {
    std::mutex mutex;
    std::condition_variable wake;
    bool started = false;
    bool waited = false;
    bool stop = false;
    std::thread::id driver_id;

    bool stop_requested() {
        std::lock_guard lock(mutex);
        return stop;
    }

    void wait_poll(unsigned interval_ms) {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(interval_ms), [this] { return stop; });
    }
};

struct TailTask {
    const sluice::File* file;
    TailOptions options;
    LineSink sink;
    DiagSink diag;
    std::vector<std::uint8_t> buffer;

    DriverState& state;

    void emit(std::vector<std::string>& lines, TailResult& r) {
        for (auto& l : lines) {
            if (sink)
                sink(l);
            ++r.lines_emitted;
        }
        lines.clear();
    }

    void diag_msg(const char* msg) {
        if (diag)
            diag(msg);
    }

    sluice::Result<std::size_t> read_at(std::uint64_t offset) {
        return sluice::blocking::read_at(
            *file, offset,
            std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()), buffer.size()));
    }

    sluice::Result<std::uint64_t> find_last_lines_offset(std::uint64_t size, std::size_t n) {
        if (size == 0 || n == 0)
            return size;

        bool skip_final_nl = false;
        {
            std::uint8_t last = 0;

            auto rr = sluice::blocking::read_at(
                *file, size - 1, std::span<std::byte>(reinterpret_cast<std::byte*>(&last), 1));
            if (!rr.has_value())
                return make_unexpected<std::uint64_t>(rr.error());
            skip_final_nl = (last == '\n');
        }

        std::size_t count = 0;
        std::uint64_t pos = size;
        while (pos > 0) {
            if (state.stop_requested())
                return size;
            std::uint64_t lo = (pos > buffer.size()) ? pos - buffer.size() : 0;

            std::size_t want = static_cast<std::size_t>(pos - lo);
            auto rr = sluice::blocking::read_at(
                *file, lo, std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()), want));
            if (!rr.has_value())
                return make_unexpected<std::uint64_t>(rr.error());
            std::size_t got = rr.value();
            if (got != want)
                return make_unexpected<std::uint64_t>(IoError{IoError::Code::backend_error});
            for (std::size_t i = got; i-- > 0;) {
                if (buffer[i] == '\n') {
                    std::uint64_t abs_off = lo + i;
                    if (skip_final_nl && abs_off == size - 1) {
                        skip_final_nl = false;
                        continue;
                    }
                    ++count;
                    if (count >= n)
                        return abs_off + 1;
                }
            }
            pos = lo;
        }
        return 0;
    }

    TailResult operator()() {
        TailResult r;
        try {
            run(r);
        } catch (const std::bad_alloc&) {
            r.error = IoError{IoError::Code::no_space};
        } catch (...) {
            if (!r.error.has_value())
                r.error = IoError{IoError::Code::backend_error};
        }
        return r;
    }

    void run(TailResult& r) {
        auto size_r = sluice::blocking::size(*file);
        if (!size_r.has_value()) {
            r.error = size_r.error();
            return;
        }
        std::uint64_t size = size_r.value();

        std::uint64_t off = size;

        if (options.lines > 0) {
            auto start_r = find_last_lines_offset(size, options.lines);
            if (!start_r.has_value()) {
                r.error = start_r.error();
                return;
            }
            if (state.stop_requested()) {
                r.stopped_by_cancel = true;
                return;
            }

            LineAssembler asmbl(options.max_line_bytes);
            std::vector<std::string> lines;
            off = start_r.value();
            while (off < size) {
                if (state.stop_requested()) {
                    r.stopped_by_cancel = true;
                    return;
                }
                auto rr = read_at(off);
                if (!rr.has_value()) {
                    r.error = rr.error();
                    return;
                }
                std::size_t n = rr.value();
                if (n == 0)
                    break;
                asmbl.feed(buffer.data(), n, lines);
                emit(lines, r);
                off += n;
            }
            asmbl.finish(lines);
            emit(lines, r);
            if (asmbl.dropped_long) {
                r.dropped_long_lines = true;
                diag_msg("line longer than --max-line-bytes skipped\n");
            }
        }

        if (!options.follow)
            return;

        LineAssembler asmbl(options.max_line_bytes);
        std::vector<std::string> lines;
        while (true) {
            if (state.stop_requested()) {
                r.stopped_by_cancel = true;
                return;
            }
            auto rr = read_at(off);
            if (!rr.has_value()) {
                r.error = rr.error();
                return;
            }
            std::size_t n = rr.value();
            if (n > 0) {
                asmbl.feed(buffer.data(), n, lines);
                emit(lines, r);

                if (asmbl.dropped_long) {
                    r.dropped_long_lines = true;
                    diag_msg("line longer than --max-line-bytes skipped\n");
                    asmbl.dropped_long = false;
                }
                off += n;
                continue;
            }

            state.wait_poll(options.poll_interval_ms);
            if (state.stop_requested()) {
                r.stopped_by_cancel = true;
                return;
            }

            auto cur_r = sluice::blocking::size(*file);
            if (!cur_r.has_value()) {
                r.error = cur_r.error();
                return;
            }
            std::uint64_t cur = cur_r.value();
            if (cur < off) {
                diag_msg("file truncated\n");
                r.truncation_detected = true;
                off = 0;
                asmbl.reset_partial();
            }
        }
    }
};

} // namespace

struct TailEngine::Impl {
    explicit Impl(sluice::File f) : file(std::move(f)) {}

    sluice::File file;
    TailOptions options;
    LineSink sink;
    DiagSink diag;
    DriverState state;
    std::thread driver;
    TailResult result;
};

namespace {

bool options_valid(const TailOptions& o) {
    return o.lines <= kMaxLines && o.buffer_size >= kMinBufferSize &&
           o.buffer_size <= kMaxBufferSize && o.max_line_bytes > 0 &&
           o.max_line_bytes <= kMaxMaxLineBytes && o.workers > 0 && o.workers <= kMaxWorkers &&
           o.poll_interval_ms >= kMinPollMs && o.poll_interval_ms <= kMaxPollMs;
}

} // namespace

TailEngine::TailEngine(sluice::File file, TailOptions options, LineSink sink, DiagSink diag)
    : impl_(std::make_unique<Impl>(std::move(file))) {
    impl_->options = options;
    impl_->sink = std::move(sink);
    impl_->diag = std::move(diag);
}

TailEngine::~TailEngine() {
    request_stop();
    if (impl_->driver.joinable())
        impl_->driver.join();
}

sluice::Result<void> TailEngine::start() {
    std::lock_guard lock(impl_->state.mutex);
    if (impl_->state.started || !options_valid(impl_->options))
        return sluice::make_unexpected<void>(IoError{IoError::Code::invalid_state});
    try {
        TailTask task{&impl_->file,
                      impl_->options,
                      impl_->sink,
                      impl_->diag,
                      std::vector<std::uint8_t>(impl_->options.buffer_size),
                      impl_->state};
        impl_->driver =
            std::thread([this, task = std::move(task)]() mutable { impl_->result = task(); });
    } catch (const std::bad_alloc&) {
        return sluice::make_unexpected<void>(IoError{IoError::Code::no_space});
    } catch (const std::system_error& error) {
        return sluice::make_unexpected<void>(
            IoError{IoError::Code::backend_error, error.code().value()});
    } catch (...) {
        return sluice::make_unexpected<void>(IoError{IoError::Code::backend_error});
    }
    impl_->state.driver_id = impl_->driver.get_id();
    impl_->state.started = true;
    return {};
}

void TailEngine::request_stop() noexcept {
    {
        std::lock_guard lock(impl_->state.mutex);
        if (!impl_->state.started)
            return;
        impl_->state.stop = true;
    }
    impl_->state.wake.notify_all();
}

sluice::Result<TailResult> TailEngine::wait() {
    {
        std::lock_guard lock(impl_->state.mutex);
        if (!impl_->state.started || impl_->state.waited ||
            impl_->state.driver_id == std::this_thread::get_id())
            return sluice::make_unexpected<TailResult>(IoError{IoError::Code::invalid_state});
        impl_->state.waited = true;
    }
    try {
        impl_->driver.join();
    } catch (const std::system_error& error) {
        return sluice::make_unexpected<TailResult>(
            IoError{IoError::Code::backend_error, error.code().value()});
    }
    return std::move(impl_->result);
}

}
