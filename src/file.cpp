// FileReader / FileWriter POSIX implementation.
#include <cppio/file.hpp>
#include <cppio/detail/posix_retry.hpp>

#include <cerrno>
#include <climits>
#include <cstddef>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/uio.h>  // readv / writev / struct iovec
#include <algorithm>
#include <vector>

#ifdef IOV_MAX
inline constexpr long kIovMaxConst = IOV_MAX;
#else
inline constexpr long kIovMaxConst = 16;  // POSIX minimum; overridden by sysconf at runtime
#endif

namespace cppio {

namespace {

// Wrap a raw ssize_t syscall result into Result<size_t>, mapping errno via the
// portable from_errno_value helper. The EINTR retry is handled by the caller
// via detail::retry_on_eintr, so by the time we get here errno is a real error.
Result<std::size_t> syscall_result(ssize_t n) {
    if (n < 0) return make_unexpected<std::size_t>(from_errno_value(errno));
    return static_cast<std::size_t>(n);
}

// The maximum number of iovec entries per readv/writev call. POSIX guarantees
// at least _XOPEN_IOV_MAX (>=16); Linux also exposes IOV_MAX (1024). We prefer
// the compile-time IOV_MAX, then query sysconf(_SC_IOV_MAX), then fall back to
// a conservative 16. Cached once.
//
// Thread-safety: this is a C++11 magic static, so the initializer runs exactly
// once even under concurrent first-call races.
long iov_max() {
    static const long cached = []() -> long {
#ifdef IOV_MAX
        long v = static_cast<long>(kIovMaxConst);
        return v > 0 ? v : 16L;
#else
        long v = ::sysconf(_SC_IOV_MAX);
        return v > 0 ? v : 16L;
#endif
    }();
    return cached;
}

// Clamp an iovec chunk count for the int-typed iovcnt parameter of readv/writev.
// iov_max() is realistically small (Linux IOV_MAX == 1024), but a pathological
// sysconf value > INT_MAX would turn a naive static_cast<int> negative and cause
// undefined behavior in the syscall. Clamp defensively.
int iovcnt_clamped(std::size_t chunk) {
    return static_cast<int>(std::min<std::size_t>(chunk, static_cast<std::size_t>(INT_MAX)));
}

}  // namespace

// ---------------- FileReader ----------------

FileReader::FileReader(const std::string& path, SyscallStats* stats, VectorStats* vec_stats)
    : stats_(stats), vec_stats_(vec_stats) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
        // Preserve the real errno so read_some() can report the actual cause
        // (ENOENT, ENOTDIR, EACCES, EMFILE, ...) rather than a synthetic code.
        open_error_ = from_errno_value(errno);
    }
}

void FileReader::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

FileReader::~FileReader() { close(); }

Result<std::size_t> FileReader::read_some(std::span<std::byte> dst) {
    if (fd_ < 0) {
        // Surface the real open() failure if we have it; otherwise this is a
        // default-constructed / moved-from reader.
        if (stats_) ++stats_->read_syscall_errors;
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{IoError::Code::permission_denied}));
    }
    if (dst.empty()) return std::size_t{0};
    ssize_t n = detail::retry_on_eintr([&] {
        return ::read(fd_, dst.data(), dst.size());
    });
    auto result = syscall_result(n);
    if (stats_) {
        if (result.has_value()) {
            ++stats_->read_syscalls;
            stats_->read_syscall_bytes += result.value();
        } else {
            ++stats_->read_syscall_errors;
        }
    }
    return result;
}

Result<std::size_t> FileReader::read_vec(std::span<IoSlice> dsts) {
    // Build the iovec list once, skipping empty slices (they never cause a
    // syscall). readv scatters directly into each IoSlice's buffer in place, so
    // no separate base/length bookkeeping is needed.
    std::vector<iovec> iovs;
    iovs.reserve(dsts.size());
    for (auto& d : dsts) {
        if (d.bytes.empty()) continue;
        iovs.push_back(iovec{d.bytes.data(), d.bytes.size()});
    }

    if (vec_stats_) {
        ++vec_stats_->read_vec_calls;
        vec_stats_->read_vec_iovecs += iovs.size();
        // NOTE: not a fallback — this is the real readv path.
    }

    // No non-empty slices: nothing to do, no syscall.
    if (iovs.empty()) return std::size_t{0};

    if (fd_ < 0) {
        // Preserve the exact open_error_ behavior of read_some.
        if (stats_) ++stats_->read_syscall_errors;
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{IoError::Code::permission_denied}));
    }

    std::size_t total = 0;
    std::size_t offset = 0;  // current iovec index into iovs
    while (offset < iovs.size()) {
        std::size_t chunk = std::min<std::size_t>(iovs.size() - offset,
                                                  static_cast<std::size_t>(iov_max()));
        ssize_t n = detail::retry_on_eintr([&] {
            return ::readv(fd_, &iovs[offset], iovcnt_clamped(chunk));
        });
        auto r = syscall_result(n);
        if (stats_) {
            if (r.has_value()) {
                ++stats_->read_syscalls;
                stats_->read_syscall_bytes += r.value();
            } else {
                ++stats_->read_syscall_errors;
            }
        }
        if (!r.has_value()) {
            // Error propagation mirrors read_vec's contract: errors are returned
            // immediately, even after partial progress.
            return make_unexpected<std::size_t>(r.error());
        }
        std::size_t got = r.value();
        total += got;
        if (got == 0) break;  // clean EOF mid-chunk: stop
        // Count how many full iovecs were completely consumed by this readv.
        // readv scatters directly into each IoSlice's buffer in place, so the
        // caller's slices already hold the bytes — no per-slice advancement here.
        std::size_t remaining = got;
        while (offset < iovs.size() && remaining >= iovs[offset].iov_len) {
            remaining -= iovs[offset].iov_len;
            ++offset;
        }
        if (remaining > 0) {
            // Partial fill of iovs[offset]: stop here to honor the
            // stop-on-short semantics shared with the default fallback.
            break;
        }
    }

    if (vec_stats_) vec_stats_->read_vec_bytes += total;
    return total;
}

// ---------------- FileWriter ----------------

FileWriter::FileWriter(const std::string& path, SyscallStats* stats, VectorStats* vec_stats)
    : stats_(stats), vec_stats_(vec_stats) {
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        open_error_ = from_errno_value(errno);
    }
}

void FileWriter::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

FileWriter::~FileWriter() { close(); }

Result<std::size_t> FileWriter::write_some(std::span<const std::byte> src) {
    if (fd_ < 0) {
        if (stats_) ++stats_->write_syscall_errors;
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{IoError::Code::permission_denied}));
    }
    if (src.empty()) return std::size_t{0};
    ssize_t n = detail::retry_on_eintr([&] {
        return ::write(fd_, src.data(), src.size());
    });
    auto result = syscall_result(n);
    if (stats_) {
        if (result.has_value()) {
            ++stats_->write_syscalls;
            stats_->write_syscall_bytes += result.value();
        } else {
            ++stats_->write_syscall_errors;
        }
    }
    return result;
}

Result<std::size_t> FileWriter::write_vec(std::span<const ConstIoSlice> srcs) {
    // Build the iovec list once, skipping empty slices (they never cause a
    // syscall).
    std::vector<iovec> iovs;
    iovs.reserve(srcs.size());
    for (const auto& s : srcs) {
        if (s.bytes.empty()) continue;
        // iovec::iov_base is void*; the source is const bytes. writev does not
        // mutate, so dropping const is safe and standard for writev. The
        // static_cast<const void*> first widens const std::byte* to const void*
        // (a different type), which the const_cast then strips.
        iovs.push_back(iovec{const_cast<void*>(static_cast<const void*>(s.bytes.data())),
                             s.bytes.size()});
    }

    if (vec_stats_) {
        ++vec_stats_->write_vec_calls;
        vec_stats_->write_vec_iovecs += iovs.size();
        // NOTE: not a fallback — this is the real writev path.
    }

    // No non-empty slices: nothing to do, no syscall.
    if (iovs.empty()) return std::size_t{0};

    if (fd_ < 0) {
        if (stats_) ++stats_->write_syscall_errors;
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{IoError::Code::permission_denied}));
    }

    std::size_t total = 0;
    std::size_t offset = 0;  // current iovec index into iovs
    while (offset < iovs.size()) {
        std::size_t chunk = std::min<std::size_t>(iovs.size() - offset,
                                                  static_cast<std::size_t>(iov_max()));
        ssize_t n = detail::retry_on_eintr([&] {
            return ::writev(fd_, &iovs[offset], iovcnt_clamped(chunk));
        });
        auto r = syscall_result(n);
        if (stats_) {
            if (r.has_value()) {
                ++stats_->write_syscalls;
                stats_->write_syscall_bytes += r.value();
            } else {
                ++stats_->write_syscall_errors;
            }
        }
        if (!r.has_value()) {
            // Error propagation mirrors write_vec's contract: errors returned
            // immediately, even after partial progress.
            return make_unexpected<std::size_t>(r.error());
        }
        std::size_t wrote = r.value();
        total += wrote;
        if (wrote == 0) break;  // zero-progress: stop and report (write_all_vec surfaces invalid_state)
        // Advance offset by the fully-written iovecs. A partial write of the
        // current iovec stops the loop (write_vec reports the partial total).
        std::size_t remaining = wrote;
        while (offset < iovs.size() && remaining >= iovs[offset].iov_len) {
            remaining -= iovs[offset].iov_len;
            ++offset;
        }
        if (remaining > 0) {
            break;  // short write into the middle of an iovec: stop
        }
    }

    if (vec_stats_) vec_stats_->write_vec_bytes += total;
    return total;
}

}  // namespace cppio
