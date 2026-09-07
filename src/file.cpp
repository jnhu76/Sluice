
#include <sluice/file.hpp>
#include <sluice/detail/io_validation.hpp>
#include <sluice/detail/posix_retry.hpp>

#include <cerrno>
#include <climits>
#include <cstddef>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <sys/uio.h>
#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#ifdef SLUICE_FILE_INTERNAL_TESTING
#include "file_test_seams.hpp"
#endif

#ifdef IOV_MAX
inline constexpr long kIovMaxConst = IOV_MAX;
#else
inline constexpr long kIovMaxConst = 16;
#endif

namespace sluice {

namespace {




Result<std::size_t> syscall_result(ssize_t n) {
    if (n < 0) {
        return make_unexpected<std::size_t>(from_errno_value(errno));
    }
    return static_cast<std::size_t>(n);
}








long iov_max() {
    static const long cached = []() -> long {
#ifdef IOV_MAX


        return static_cast<long>(kIovMaxConst);
#else


        long v = ::sysconf(_SC_IOV_MAX);
        return v > 0 ? v : 16L;
#endif
    }();
    return cached;
}





int iovcnt_clamped(std::size_t chunk) {
    return static_cast<int>(std::min<std::size_t>(chunk, static_cast<std::size_t>(INT_MAX)));
}







int close_fd(int fd) {
#ifdef SLUICE_FILE_INTERNAL_TESTING
    if (file_testing::CloseScript* script = file_testing::CloseScript::active()) {
        return script->next(fd);
    }
#endif
    return ::close(fd);
}

}



FileReader::FileReader(const std::string& path, SyscallStats* stats, VectorStats* vec_stats)
    : stats_(stats), vec_stats_(vec_stats) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {


        open_error_ = from_errno_value(errno);
    }
}

Result<void> FileReader::close() noexcept {
    if (fd_ < 0) {



        return {};
    }



    const int fd = std::exchange(fd_, -1);
    if (close_fd(fd) != 0) {
        return make_unexpected<void>(from_errno_value(errno));
    }
    return {};
}

FileReader::~FileReader() {



    (void)close();
}

Result<std::size_t> FileReader::read_some(std::span<std::byte> dst) {
    if (fd_ < 0) {


        if (stats_) {
            ++stats_->read_syscall_errors;
        }
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{.code = IoError::Code::permission_denied}));
    }
    if (dst.empty()) {
        return std::size_t{0};
    }
    ssize_t n = detail::retry_on_eintr([&] { return ::read(fd_, dst.data(), dst.size()); });
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



    std::vector<iovec> iovs;
    iovs.reserve(dsts.size());
    for (auto& d : dsts) {
        if (d.bytes.empty()) {
            continue;
        }
        iovs.push_back(iovec{.iov_base = d.bytes.data(), .iov_len = d.bytes.size()});
    }

    if (vec_stats_) {
        ++vec_stats_->read_vec_calls;
        vec_stats_->read_vec_iovecs += iovs.size();

    }


    if (iovs.empty()) {
        return std::size_t{0};
    }

    if (fd_ < 0) {

        if (stats_) {
            ++stats_->read_syscall_errors;
        }
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{.code = IoError::Code::permission_denied}));
    }

    std::size_t total = 0;
    std::size_t offset = 0;
    while (offset < iovs.size()) {
        std::size_t chunk =
            std::min<std::size_t>(iovs.size() - offset, static_cast<std::size_t>(iov_max()));
        ssize_t n = detail::retry_on_eintr(
            [&] { return ::readv(fd_, &iovs[offset], iovcnt_clamped(chunk)); });
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


            return make_unexpected<std::size_t>(r.error());
        }
        std::size_t got = r.value();
        total += got;
        if (got == 0) {
            break;
        }



        std::size_t remaining = got;
        while (offset < iovs.size() && remaining >= iovs[offset].iov_len) {
            remaining -= iovs[offset].iov_len;
            ++offset;
        }
        if (remaining > 0) {


            break;
        }
    }

    if (vec_stats_) {
        vec_stats_->read_vec_bytes += total;
    }
    return total;
}

Result<std::size_t> FileReader::read_at(std::uint64_t offset, std::span<std::byte> dst) {
    if (fd_ < 0) {
        if (stats_) {
            ++stats_->read_syscall_errors;
        }
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{.code = IoError::Code::permission_denied}));
    }
    if (dst.empty()) {
        return std::size_t{0};
    }
    auto native_offset = detail::checked_posix_offset(offset);
    if (!native_offset.has_value()) {
        return make_unexpected<std::size_t>(native_offset.error());
    }

    ssize_t n = detail::retry_on_eintr(
        [&] { return ::pread(fd_, dst.data(), dst.size(), native_offset.value()); });
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

Result<std::size_t> FileReader::read_vec_at(std::uint64_t offset, std::span<IoSlice> dsts) {

    std::vector<iovec> iovs;
    iovs.reserve(dsts.size());
    for (auto& d : dsts) {
        if (d.bytes.empty()) {
            continue;
        }
        iovs.push_back(iovec{.iov_base = d.bytes.data(), .iov_len = d.bytes.size()});
    }

    if (vec_stats_) {
        ++vec_stats_->read_vec_calls;
        vec_stats_->read_vec_iovecs += iovs.size();
    }

    if (iovs.empty()) {
        return std::size_t{0};
    }

    if (fd_ < 0) {
        if (stats_) {
            ++stats_->read_syscall_errors;
        }
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{.code = IoError::Code::permission_denied}));
    }




    std::size_t total = 0;
    std::size_t idx = 0;
    std::uint64_t off = offset;
    while (idx < iovs.size()) {
        auto native_offset = detail::checked_posix_offset(off);
        if (!native_offset.has_value()) {
            return make_unexpected<std::size_t>(native_offset.error());
        }
        std::size_t chunk =
            std::min<std::size_t>(iovs.size() - idx, static_cast<std::size_t>(iov_max()));
        ssize_t n = detail::retry_on_eintr([&] {
            return ::preadv(fd_, &iovs[idx], iovcnt_clamped(chunk), native_offset.value());
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
            return make_unexpected<std::size_t>(r.error());
        }
        std::size_t got = r.value();
        total += got;
        off += got;
        if (got == 0) {
            break;
        }

        std::size_t remaining = got;
        while (idx < iovs.size() && remaining >= iovs[idx].iov_len) {
            remaining -= iovs[idx].iov_len;
            ++idx;
        }
        if (remaining > 0) {
            break;
        }
    }

    if (vec_stats_) {
        vec_stats_->read_vec_bytes += total;
    }
    return total;
}

Result<void> FileReader::read_at_exact(std::uint64_t offset, std::span<std::byte> dst) {
    if (dst.empty()) {
        return {};
    }
    std::uint64_t off = offset;
    std::size_t filled = 0;
    while (filled < dst.size()) {
        auto r = read_at(off, dst.subspan(filled));
        if (!r.has_value()) {
            return make_unexpected(r.error());
        }
        std::size_t got = r.value();
        if (got == 0) {


            return make_unexpected(IoError{.code = IoError::Code::eof});
        }
        filled += got;
        off += got;
    }
    return {};
}



FileWriter::FileWriter(const std::string& path, SyscallStats* stats, VectorStats* vec_stats,
                       SyncStats* sync_stats)
    : stats_(stats), vec_stats_(vec_stats), sync_stats_(sync_stats) {
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        open_error_ = from_errno_value(errno);
    }
}

Result<void> FileWriter::close() noexcept {
    if (fd_ < 0) {



        return {};
    }


    const int fd = std::exchange(fd_, -1);
    if (close_fd(fd) != 0) {


        return make_unexpected<void>(from_errno_value(errno));
    }
    return {};
}

FileWriter::~FileWriter() {



    (void)close();
}

Result<std::size_t> FileWriter::write_some(std::span<const std::byte> src) {
    if (fd_ < 0) {
        if (stats_) {
            ++stats_->write_syscall_errors;
        }
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{.code = IoError::Code::permission_denied}));
    }
    if (src.empty()) {
        return std::size_t{0};
    }
    ssize_t n = detail::retry_on_eintr([&] { return ::write(fd_, src.data(), src.size()); });
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


    std::vector<iovec> iovs;
    iovs.reserve(srcs.size());
    for (const auto& s : srcs) {
        if (s.bytes.empty()) {
            continue;
        }




        iovs.push_back(
            iovec{.iov_base = const_cast<void*>(static_cast<const void*>(s.bytes.data())),
                  .iov_len = s.bytes.size()});
    }

    if (vec_stats_) {
        ++vec_stats_->write_vec_calls;
        vec_stats_->write_vec_iovecs += iovs.size();

    }


    if (iovs.empty()) {
        return std::size_t{0};
    }

    if (fd_ < 0) {
        if (stats_) {
            ++stats_->write_syscall_errors;
        }
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{.code = IoError::Code::permission_denied}));
    }

    std::size_t total = 0;
    std::size_t offset = 0;
    while (offset < iovs.size()) {
        std::size_t chunk =
            std::min<std::size_t>(iovs.size() - offset, static_cast<std::size_t>(iov_max()));
        ssize_t n = detail::retry_on_eintr(
            [&] { return ::writev(fd_, &iovs[offset], iovcnt_clamped(chunk)); });
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


            return make_unexpected<std::size_t>(r.error());
        }
        std::size_t wrote = r.value();
        total += wrote;
        if (wrote == 0) {
            break;
        }


        std::size_t remaining = wrote;
        while (offset < iovs.size() && remaining >= iovs[offset].iov_len) {
            remaining -= iovs[offset].iov_len;
            ++offset;
        }
        if (remaining > 0) {
            break;
        }
    }

    if (vec_stats_) {
        vec_stats_->write_vec_bytes += total;
    }
    return total;
}

Result<std::size_t> FileWriter::write_at(std::uint64_t offset, std::span<const std::byte> src) {
    if (fd_ < 0) {
        if (stats_) {
            ++stats_->write_syscall_errors;
        }
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{.code = IoError::Code::permission_denied}));
    }
    if (src.empty()) {
        return std::size_t{0};
    }
    auto native_offset = detail::checked_posix_offset(offset);
    if (!native_offset.has_value()) {
        return make_unexpected<std::size_t>(native_offset.error());
    }

    ssize_t n = detail::retry_on_eintr(
        [&] { return ::pwrite(fd_, src.data(), src.size(), native_offset.value()); });
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

Result<std::size_t> FileWriter::write_vec_at(std::uint64_t offset,
                                             std::span<const ConstIoSlice> srcs) {

    std::vector<iovec> iovs;
    iovs.reserve(srcs.size());
    for (const auto& s : srcs) {
        if (s.bytes.empty()) {
            continue;
        }
        iovs.push_back(
            iovec{.iov_base = const_cast<void*>(static_cast<const void*>(s.bytes.data())),
                  .iov_len = s.bytes.size()});
    }

    if (vec_stats_) {
        ++vec_stats_->write_vec_calls;
        vec_stats_->write_vec_iovecs += iovs.size();
    }

    if (iovs.empty()) {
        return std::size_t{0};
    }

    if (fd_ < 0) {
        if (stats_) {
            ++stats_->write_syscall_errors;
        }
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{.code = IoError::Code::permission_denied}));
    }



    std::size_t total = 0;
    std::size_t idx = 0;
    std::uint64_t off = offset;
    while (idx < iovs.size()) {
        auto native_offset = detail::checked_posix_offset(off);
        if (!native_offset.has_value()) {
            return make_unexpected<std::size_t>(native_offset.error());
        }
        std::size_t chunk =
            std::min<std::size_t>(iovs.size() - idx, static_cast<std::size_t>(iov_max()));
        ssize_t n = detail::retry_on_eintr([&] {
            return ::pwritev(fd_, &iovs[idx], iovcnt_clamped(chunk), native_offset.value());
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
            return make_unexpected<std::size_t>(r.error());
        }
        std::size_t wrote = r.value();
        total += wrote;
        off += wrote;
        if (wrote == 0) {
            break;
        }
        std::size_t remaining = wrote;
        while (idx < iovs.size() && remaining >= iovs[idx].iov_len) {
            remaining -= iovs[idx].iov_len;
            ++idx;
        }
        if (remaining > 0) {
            break;
        }
    }

    if (vec_stats_) {
        vec_stats_->write_vec_bytes += total;
    }
    return total;
}

Result<void> FileWriter::write_at_all(std::uint64_t offset, std::span<const std::byte> src) {
    if (src.empty()) {
        return {};
    }
    std::uint64_t off = offset;
    std::size_t written = 0;
    while (written < src.size()) {
        auto r = write_at(off, src.subspan(written));
        if (!r.has_value()) {
            return make_unexpected(r.error());
        }
        std::size_t put = r.value();
        if (put == 0) {

            return make_unexpected(IoError{.code = IoError::Code::invalid_state});
        }
        written += put;
        off += put;
    }
    return {};
}

namespace {




template <class Fn>
Result<void> do_sync(int fd, const std::optional<IoError>& open_error, const Fn& fn,
                     SyncStats* stats, std::uint64_t SyncStats::* calls,
                     std::uint64_t SyncStats::* errors) {
    if (fd < 0) {


        if (stats) {
            ++(stats->*errors);
        }
        if (open_error.has_value()) {
            return make_unexpected<void>(*open_error);
        }
        return make_unexpected<void>(IoError{.code = IoError::Code::invalid_state});
    }
    int rc = detail::retry_on_eintr([&] { return fn(fd); });
    if (rc < 0) {
        if (stats) {
            ++(stats->*errors);
        }
        return make_unexpected<void>(from_errno_value(errno));
    }
    if (stats) {
        ++(stats->*calls);
    }
    return {};
}

}

Result<void> FileWriter::sync_data() {
    return do_sync(
        fd_, open_error_, [](int fd) { return ::fdatasync(fd); }, sync_stats_,
        &SyncStats::sync_data_calls, &SyncStats::sync_data_errors);
}

Result<void> FileWriter::sync_all() {
    return do_sync(
        fd_, open_error_, [](int fd) { return ::fsync(fd); }, sync_stats_,
        &SyncStats::sync_all_calls, &SyncStats::sync_all_errors);
}

}
