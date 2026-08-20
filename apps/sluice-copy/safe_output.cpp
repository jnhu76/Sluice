// sluice-copy Version C — safe atomic output implementation.
#include "safe_output.hpp"

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

// App-local RAII file descriptor (brief §21: do NOT promote to core).
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

// Parent directory of `path` ("/a/b/c" -> "/a/b", "c" -> "."). A trailing
// slash ("dir/") keeps the last slash as the split point ("dir"), so the
// destination is the (empty) name after it — which the rename step rejects.
std::string parent_dir_of(const std::string& path) {
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// Directory durability syscall — indirection point for the deterministic
// EINTR regression seam (#142). In production this is exactly ::fsync; only
// the internal-testing build can script it, and that build carries no other
// behavior difference.
int directory_fsync(int fd) {
#ifdef SLUICE_COPY_INTERNAL_TESTING
    if (testing::DirFsyncScript* script = testing::DirFsyncScript::active()) {
        return script->next(fd);
    }
#endif
    return ::fsync(fd);
}

}  // namespace

SafeOpenOutcome open_atomic_copy(const std::string& src_path,
                                 const std::string& dst_path) {
    // 1. Source: open read-only, fstat, require a regular file (same input
    //    domain as file_domain.cpp — the Version B pipeline needs a seekable,
    //    finite-length source). Rejected BEFORE anything is created.
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

    // 2. Destination (only if it exists): must be a regular file and must not
    //    be the same inode as the source. stat() FOLLOWS a symlink: a symlink
    //    destination is validated against its target (and rejected as
    //    same_file if the target IS the source), while the later rename
    //    replaces the LINK itself — documented install-style semantics.
    //    The destination is never opened for writing and never truncated.
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
        // Exists-but-unstattable (EACCES loop etc.) is an error; ENOENT is
        // the normal fresh-destination case. ENOTDIR covers a non-directory
        // prefix ("file/x") — the temp create below will report the precise
        // errno if it matters.
        return fail(SafeOpenFailure::dst_stat,
                    sluice::from_errno_value(errno));
    }

    // 3. Temp file in the DESTINATION's directory: same filesystem, so the
    //    rename is atomic. mkstemp gives O_CREAT|O_EXCL semantics with a
    //    unique name (0600); the mode is set explicitly afterwards.
    std::string dir = parent_dir_of(dst_path);
    std::string tmpl = dir + "/.sluice-copy.tmp.XXXXXX";
    // mkstemp mutates the template in place; copy into a writable buffer.
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int temp_fd = ::mkstemp(buf.data());
    if (temp_fd < 0) {
        return fail(SafeOpenFailure::temp_create,
                    sluice::from_errno_value(errno));
    }
    ScopedFd temp_guard(temp_fd);
    std::string temp_path(buf.data());

    // 4. Permission policy: the destination receives the source's rwx bits
    //    (0777 mask). setuid/setgid/sticky are deliberately dropped (copying
    //    a setuid binary must not resurrect execute-as-owner); umask is not
    //    applied (explicit fchmod). Owner/group/timestamps are NOT preserved.
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
    // Close the temp fd on every path (its data was already synced by the
    // copy task's SyncPolicy; close itself may still report an error).
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

    // The atomic replacement. Failure leaves the destination untouched.
    if (::rename(o.temp_path.c_str(), dst_path.c_str()) != 0) {
        if (stage) *stage = SafeCommitStage::rename;
        IoError e = sluice::from_errno_value(errno);
        ::unlink(o.temp_path.c_str());
        o.temp_path.clear();
        return sluice::make_unexpected<void>(e);
    }
    o.temp_path.clear();

    // Directory durability (only when the policy requires it): fsync the
    // parent directory so the rename itself survives a crash. At this point
    // the rename already happened; a failure here is reported as missing
    // durability, not as a lost copy.
    if (sync != SyncPolicy::none) {
        int dir_fd = ::open(o.dst_dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd < 0) {
            if (stage) *stage = SafeCommitStage::dir_sync;
            return sluice::make_unexpected<void>(
                sluice::from_errno_value(errno));
        }
        ScopedFd dir_guard(dir_fd);
        if (directory_fsync(dir_fd) != 0) {
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

}  // namespace sluice_copy
