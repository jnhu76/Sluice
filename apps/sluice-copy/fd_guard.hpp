#pragma once

#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace sluice_copy::detail {

struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f) : fd(f) {}
    ~ScopedFd() {
        if (fd >= 0)
            ::close(fd);
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

enum class SourceFailure : std::uint8_t {
    none,
    open,
    stat,
    not_regular,
};

struct SourceProbe {
    std::optional<sluice::File> file;
    struct stat stat{};
    SourceFailure failure = SourceFailure::none;
    sluice::IoError error{};
};

inline SourceProbe open_regular_source(const std::string& path) {
    SourceProbe out;
    auto opened = sluice::File::open(path);
    if (!opened.has_value()) {
        out.failure = SourceFailure::open;
        out.error = opened.error();
        return out;
    }
    out.file.emplace(std::move(opened).value());
    if (::fstat(out.file->native_handle(), &out.stat) != 0) {
        out.failure = SourceFailure::stat;
        out.error = sluice::from_errno_value(errno);
        return out;
    }
    if (!S_ISREG(out.stat.st_mode)) {
        out.failure = SourceFailure::not_regular;
        out.error = sluice::IoError{sluice::IoError::Code::invalid_state};
    }
    return out;
}

}
