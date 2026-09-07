
#include "safe_output.hpp"

#include <sluice/detail/posix_retry.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

#ifdef SLUICE_COPY_INTERNAL_TESTING
#include "safe_output_test_seams.hpp"
#endif

namespace sluice_copy {

namespace {

using sluice::IoError;


struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f) : fd(f) {}
    ~ScopedFd() { if (fd >= 0) ::close(fd); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

SafeOpenOutcome fail(SafeOpenFailure f, IoError e) {
    SafeOpenOutcome o;
    o.failure = f;
    o.error = e;
    return o;
}




std::string parent_dir_of(const std::string& path) {
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}





int directory_fsync(int fd) {
#ifdef SLUICE_COPY_INTERNAL_TESTING
    if (testing::DirFsyncScript* script = testing::DirFsyncScript::active()) {
        return script->next(fd);
    }
#endif
    return ::fsync(fd);
}

}

SafeOpenOutcome open_atomic_copy(const std::string& src_path,
                                 const std::string& dst_path) {



    int src_fd = ::open(src_path.c_str(), O_RDONLY);
    if (src_fd < 0) {
        return fail(SafeOpenFailure::src_open,
                    sluice::from_errno_value(errno));
    }
    ScopedFd src_guard(src_fd);

    struct stat src_stat{};
    if (::fstat(src_fd, &src_stat) != 0) {
        return fail(SafeOpenFailure::src_stat,
                    sluice::from_errno_value(errno));
    }
    if (!S_ISREG(src_stat.st_mode)) {
        return fail(SafeOpenFailure::src_not_regular,
                    IoError{IoError::Code::invalid_state});
    }







    struct stat dst_stat{};
    if (::stat(dst_path.c_str(), &dst_stat) == 0) {
        if (!S_ISREG(dst_stat.st_mode)) {
            return fail(SafeOpenFailure::dst_not_regular,
                        IoError{IoError::Code::invalid_state});
        }
        if (src_stat.st_dev == dst_stat.st_dev &&
            src_stat.st_ino == dst_stat.st_ino) {
            return fail(SafeOpenFailure::same_file,
                        IoError{IoError::Code::invalid_state});
        }
    } else if (errno != ENOENT && errno != ENOTDIR) {




        return fail(SafeOpenFailure::dst_stat,
                    sluice::from_errno_value(errno));
    }




    std::string dir = parent_dir_of(dst_path);
    std::string tmpl = dir + "/.sluice-copy.tmp.XXXXXX";

    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int temp_fd = ::mkstemp(buf.data());
    if (temp_fd < 0) {
        return fail(SafeOpenFailure::temp_create,
                    sluice::from_errno_value(errno));
    }
    ScopedFd temp_guard(temp_fd);
    std::string temp_path(buf.data());





    mode_t mode = static_cast<mode_t>(src_stat.st_mode & 0777);
    if (::fchmod(temp_fd, mode) != 0) {
        ::unlink(temp_path.c_str());
        return fail(SafeOpenFailure::temp_chmod,
                    sluice::from_errno_value(errno));
    }

    SafeOpenOutcome o;
    o.failure = SafeOpenFailure::none;
    o.src_fd = src_guard.fd;
    o.temp_fd = temp_guard.fd;
    o.temp_path = std::move(temp_path);
    o.dst_dir = std::move(dir);
    src_guard.fd = -1;
    temp_guard.fd = -1;
    return o;
}

const char* safe_open_failure_message(SafeOpenFailure f) {
    switch (f) {
    case SafeOpenFailure::none: return "ok";
    case SafeOpenFailure::src_open: return "cannot open source";
    case SafeOpenFailure::src_stat: return "cannot stat source";
    case SafeOpenFailure::src_not_regular:
        return "source is not a regular file";
    case SafeOpenFailure::dst_stat:
        return "cannot stat destination";
    case SafeOpenFailure::dst_not_regular:
        return "destination is not a regular file";
    case SafeOpenFailure::same_file:
        return "source and destination refer to the same file";
    case SafeOpenFailure::temp_dir:
        return "destination directory is not usable";
    case SafeOpenFailure::temp_create:
        return "cannot create temporary file in destination directory";
    case SafeOpenFailure::temp_chmod:
        return "cannot set permissions on temporary file";
    }
    return "unknown error";
}

sluice::Result<void> commit_atomic_copy(SafeOpenOutcome& o,
                                        const std::string& dst_path,
                                        SyncPolicy sync,
                                        SafeCommitStage* stage) {
    if (stage) *stage = SafeCommitStage::none;


    int fd = o.temp_fd;
    o.temp_fd = -1;
    bool closed_ok = (fd < 0) || (::close(fd) == 0);

    if (!closed_ok) {
        if (stage) *stage = SafeCommitStage::close;
        IoError e = sluice::from_errno_value(errno);
        ::unlink(o.temp_path.c_str());
        o.temp_path.clear();
        return sluice::make_unexpected<void>(e);
    }


    if (::rename(o.temp_path.c_str(), dst_path.c_str()) != 0) {
        if (stage) *stage = SafeCommitStage::rename;
        IoError e = sluice::from_errno_value(errno);
        ::unlink(o.temp_path.c_str());
        o.temp_path.clear();
        return sluice::make_unexpected<void>(e);
    }
    o.temp_path.clear();





    if (sync != SyncPolicy::none) {
        int dir_fd = ::open(o.dst_dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd < 0) {
            if (stage) *stage = SafeCommitStage::dir_sync;
            return sluice::make_unexpected<void>(
                sluice::from_errno_value(errno));
        }
        ScopedFd dir_guard(dir_fd);





        int rc = sluice::detail::retry_on_eintr(
            [&] { return directory_fsync(dir_fd); });
        if (rc != 0) {
            if (stage) *stage = SafeCommitStage::dir_sync;
            return sluice::make_unexpected<void>(
                sluice::from_errno_value(errno));
        }
    }
    return {};
}

void discard_atomic_copy(SafeOpenOutcome& o) {
    if (o.temp_fd >= 0) {
        ::close(o.temp_fd);
        o.temp_fd = -1;
    }
    if (!o.temp_path.empty()) {
        ::unlink(o.temp_path.c_str());
        o.temp_path.clear();
    }
}

}
