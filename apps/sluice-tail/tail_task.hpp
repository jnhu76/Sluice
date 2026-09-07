




























#pragma once

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/async_io_context.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace sluice_tail {


constexpr std::size_t kMinBufferSize = 4 * 1024;
constexpr std::size_t kMaxBufferSize = 64 * 1024 * 1024;
constexpr std::size_t kDefaultMaxLineBytes = 1 << 20;
constexpr std::size_t kMaxMaxLineBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxLines = 1000 * 1000 * 1000;
constexpr unsigned kMaxWorkers = 64;
constexpr unsigned kMinPollMs = 50;
constexpr unsigned kMaxPollMs = 5000;

struct TailOptions {
    std::size_t lines = 10;
    bool follow = false;
    unsigned poll_interval_ms = 200;
    std::size_t buffer_size = 64 * 1024;
    std::size_t max_line_bytes = kDefaultMaxLineBytes;
    unsigned workers = 1;
};



using LineSink = std::function<void(std::string_view line)>;



using DiagSink = std::function<void(std::string_view msg)>;

struct TailResult {
    std::optional<sluice::IoError> error;
    bool stopped_by_cancel = false;
    std::uint64_t lines_emitted = 0;
    bool truncation_detected = false;
    bool dropped_long_lines = false;
};

class TailEngine {
public:


    TailEngine(int fd, TailOptions options, LineSink sink,
               DiagSink diag = nullptr);
    ~TailEngine();

    TailEngine(const TailEngine&) = delete;
    TailEngine& operator=(const TailEngine&) = delete;




    sluice::Result<void> start();




    void request_stop() noexcept;



    sluice::Result<TailResult> wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
