#include "tail_task.hpp"

#include <sluice/file_resource.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice_tail::TailEngine;
using sluice_tail::TailOptions;
using sluice_tail::TailResult;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_app_tail_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    std::string out_path = path;
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(content.size())) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            ::close(fd);
            ::unlink(out_path.c_str());
            return {};
        }
        written += n;
    }
    ::close(fd);
    return out_path;
}

bool append_line(const std::string& path, const std::string& line) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
    if (fd < 0)
        return false;
    const ssize_t n = ::write(fd, line.data(), line.size());
    ::close(fd);
    return n == static_cast<ssize_t>(line.size());
}

bool truncate_file(const std::string& path, std::uint64_t size) {
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0)
        return false;
    const bool ok = ::ftruncate(fd, static_cast<off_t>(size)) == 0;
    ::close(fd);
    return ok;
}

bool tail_engine_emits_last_lines() {
    std::string content;
    for (int i = 1; i <= 100; ++i)
        content += "line " + std::to_string(i) + "\n";
    const std::string path = make_temp_file(content);
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;

    std::vector<std::string> lines;
    TailOptions options;
    options.lines = 3;

    TailEngine engine(std::move(opened).value(), options,
                      [&](std::string_view l) { lines.emplace_back(l); });
    if (!engine.start().has_value())
        return false;
    const auto result = engine.wait();
    if (!result.has_value())
        return false;
    if (result.value().error.has_value())
        return false;
    if (result.value().lines_emitted != 3)
        return false;
    if (result.value().stopped_by_cancel || result.value().truncation_detected)
        return false;
    if (lines.size() != 3)
        return false;
    return lines[0] == "line 98" && lines[1] == "line 99" && lines[2] == "line 100";
}

bool tail_engine_lines_zero_emits_nothing() {
    const std::string path = make_temp_file("a\nb\n");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;

    std::vector<std::string> lines;
    TailOptions options;
    options.lines = 0;

    TailEngine engine(std::move(opened).value(), options,
                      [&](std::string_view l) { lines.emplace_back(l); });
    if (!engine.start().has_value())
        return false;
    const auto result = engine.wait();
    if (!result.has_value())
        return false;
    return !result.value().error.has_value() && result.value().lines_emitted == 0 &&
           lines.empty();
}

bool tail_engine_start_rejects_invalid_options() {
    const std::string path = make_temp_file("a\n");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;

    TailOptions options;
    options.workers = 0;

    std::vector<std::string> lines;
    TailEngine engine(std::move(opened).value(), options,
                      [&](std::string_view l) { lines.emplace_back(l); });
    if (engine.start().has_value())
        return false;
    return engine.wait().error().code == sluice::IoError::Code::invalid_state;
}

bool tail_engine_follow_stops_on_request_stop() {
    std::string content;
    for (int i = 1; i <= 10; ++i)
        content += "seed " + std::to_string(i) + "\n";
    const std::string path = make_temp_file(content);
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;

    std::vector<std::string> lines;
    TailOptions options;
    options.lines = 2;
    options.follow = true;
    options.poll_interval_ms = 50;

    TailEngine engine(std::move(opened).value(), options,
                      [&](std::string_view l) { lines.emplace_back(l); });
    if (!engine.start().has_value())
        return false;
    ::usleep(150 * 1000);
    engine.request_stop();
    const auto result = engine.wait();
    if (!result.has_value())
        return false;
    if (!result.value().stopped_by_cancel)
        return false;
    if (result.value().error.has_value())
        return false;
    if (result.value().lines_emitted != 2)
        return false;
    return lines.size() == 2 && lines[0] == "seed 9" && lines[1] == "seed 10";
}

bool tail_engine_follow_detects_truncation() {
    std::string content;
    for (int i = 1; i <= 10; ++i)
        content += "seed " + std::to_string(i) + "\n";
    const std::string path = make_temp_file(content);
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value()) {
        ::unlink(path.c_str());
        return false;
    }

    std::vector<std::string> lines;
    std::string diag;
    TailOptions options;
    options.lines = 1;
    options.follow = true;
    options.poll_interval_ms = 50;

    TailEngine engine(std::move(opened).value(), options,
                      [&](std::string_view l) { lines.emplace_back(l); },
                      [&](std::string_view m) { diag.append(m); });
    if (!engine.start().has_value()) {
        ::unlink(path.c_str());
        return false;
    }
    ::usleep(150 * 1000);
    if (!truncate_file(path, 0)) {
        ::unlink(path.c_str());
        return false;
    }
    ::usleep(150 * 1000);
    if (!append_line(path, "reborn\n")) {
        ::unlink(path.c_str());
        return false;
    }
    ::usleep(150 * 1000);
    engine.request_stop();
    const auto result = engine.wait();
    ::unlink(path.c_str());
    if (!result.has_value())
        return false;
    if (!result.value().truncation_detected)
        return false;
    if (diag.find("file truncated") == std::string::npos)
        return false;
    bool saw_reborn = false;
    for (const auto& l : lines)
        if (l == "reborn")
            saw_reborn = true;
    return saw_reborn;
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"tail_engine_emits_last_lines", tail_engine_emits_last_lines},
        {"tail_engine_lines_zero_emits_nothing", tail_engine_lines_zero_emits_nothing},
        {"tail_engine_start_rejects_invalid_options",
         tail_engine_start_rejects_invalid_options},
        {"tail_engine_follow_stops_on_request_stop",
         tail_engine_follow_stops_on_request_stop},
        {"tail_engine_follow_detects_truncation", tail_engine_follow_detects_truncation},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu app tail consumption tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
