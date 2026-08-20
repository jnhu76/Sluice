// sluice-copy Version C safe-output tests.
//
// Drives the SAME safe_output.cpp module the CLI uses (compiled into this
// target) plus the real copy_task.cpp, over real files and directories:
//   - open_atomic_copy input domain (regular source, destination validation,
//     same-inode rejection, temp file in the destination directory, source
//     permission preservation);
//   - commit_atomic_copy (atomic replacement, temp cleanup, rename-failure
//     path, directory fsync under sync policies);
//   - the full atomic flow with a fault-injecting backend: a copy error or
//     cancellation leaves an existing destination untouched and no temp file;
//   - the full atomic flow with the real ThreadPoolBackend (multi-buffer,
//     pipeline, sync=all);
//   - the directory-fsync EINTR contract (#142): scripted through the
//     app-private DirFsyncScript seam (armed only in this internal-testing
//     target) — EINTR is retried, a real error still fails.
//
// No sleeps. safe_output.cpp + copy_task.cpp are compiled into this target.
#include "harness.hpp"

#include "copy_task.hpp"
#include "safe_output.hpp"
#include "safe_output_test_seams.hpp"

#include <sluice/async/fake_backend.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace sluice_copy;
using sluice::IoError;
using sluice::async::FakeAsyncBackend;

namespace {

// RAII temp DIRECTORY (mkdtemp). Destruction unlinks every entry then rmdirs,
// so a leaked .sluice-copy.tmp.* file fails the count_temp_files checks first
// and is still cleaned up afterwards.
struct TempDir {
    std::string path;
    TempDir() {
        char p[] = "/tmp/sluice_copy_safe_XXXXXX";
        char* r = ::mkdtemp(p);
        SLUICE_CHECK(r != nullptr);
        path = p;
    }
    ~TempDir() {
        DIR* d = ::opendir(path.c_str());
        if (d) {
            while (dirent* e = ::readdir(d)) {
                if (std::strcmp(e->d_name, ".") == 0 ||
                    std::strcmp(e->d_name, "..") == 0)
                    continue;
                std::string full = path + "/" + e->d_name;
                ::unlink(full.c_str());
            }
            ::closedir(d);
        }
        ::rmdir(path.c_str());
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string file(const char* name) const { return path + "/" + name; }
};

// SLUICE_CHECK (harness.hpp) bare-returns on failure, which is invalid in a
// value-returning helper; this variant returns a default-constructed value.
#define SAFE_CHECK(cond)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ::sluice_test::record_failure(__FILE__, __LINE__, #cond);          \
            return {};                                                         \
        }                                                                      \
    } while (0)

// Create a file with `content` (mode 0644 unless overridden) inside `dir`.
std::string make_file(const TempDir& dir, const char* name,
                      const std::string& content, mode_t mode = 0644) {
    std::string p = dir.file(name);
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    SAFE_CHECK(fd >= 0);
    SAFE_CHECK(::write(fd, content.data(), content.size()) ==
               static_cast<ssize_t>(content.size()));
    ::fchmod(fd, mode);  // open() applied umask; make the mode exact
    ::close(fd);
    return p;
}

std::string read_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return {};
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, n);
    ::close(fd);
    return out;
}

// Count leftover ".sluice-copy.tmp.*" entries in dir (leak detector).
int count_temp_files(const TempDir& dir) {
    DIR* d = ::opendir(dir.path.c_str());
    if (!d) return -1;
    int count = 0;
    while (dirent* e = ::readdir(d)) {
        if (std::strncmp(e->d_name, ".sluice-copy.tmp.",
                         sizeof ".sluice-copy.tmp." - 1) == 0)
            ++count;
    }
    ::closedir(d);
    return count;
}

std::string temp_prefix_in(const TempDir& dir) {
    return dir.path + "/.sluice-copy.tmp.";
}

}  // namespace

// ---------------------------------------------------------------------------
// open_atomic_copy — input domain
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(safe_output_open_creates_temp_in_dst_dir) {
    TempDir dir;
    std::string src = make_file(dir, "src", "hello");
    std::string dst = dir.file("dst");

    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);
    SLUICE_CHECK(o.src_fd >= 0);
    SLUICE_CHECK(o.temp_fd >= 0);
    SLUICE_CHECK(o.temp_path.rfind(temp_prefix_in(dir), 0) == 0);
    SLUICE_CHECK(o.dst_dir == dir.path);

    discard_atomic_copy(o);
    SLUICE_CHECK(o.temp_fd == -1);
    SLUICE_CHECK(count_temp_files(dir) == 0);
    ::close(o.src_fd);
}

SLUICE_TEST_CASE(safe_output_open_preserves_source_mode_bits) {
    TempDir dir;
    std::string src = make_file(dir, "src", "x", 0741);
    std::string dst = dir.file("dst");

    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);

    struct stat st{};
    SLUICE_CHECK(::fstat(o.temp_fd, &st) == 0);
    SLUICE_CHECK((st.st_mode & 07777) == 0741);

    discard_atomic_copy(o);
    ::close(o.src_fd);
}

SLUICE_TEST_CASE(safe_output_open_rejects_directory_source) {
    TempDir dir;
    std::string dst = dir.file("dst");
    SafeOpenOutcome o = open_atomic_copy(dir.path, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::src_not_regular);
    SLUICE_CHECK(o.src_fd < 0 && o.temp_fd < 0);
    SLUICE_CHECK(count_temp_files(dir) == 0);
}

SLUICE_TEST_CASE(safe_output_open_rejects_directory_destination) {
    TempDir dir;
    TempDir dst_dir;  // destination that already exists as a directory
    std::string src = make_file(dir, "src", "x");
    SafeOpenOutcome o = open_atomic_copy(src, dst_dir.path);
    SLUICE_CHECK(o.failure == SafeOpenFailure::dst_not_regular);
    ::close(o.src_fd);
}

SLUICE_TEST_CASE(safe_output_open_rejects_same_file_and_hardlink) {
    TempDir dir;
    std::string src = make_file(dir, "src", "data");

    // Identical path.
    SafeOpenOutcome a = open_atomic_copy(src, src);
    SLUICE_CHECK(a.failure == SafeOpenFailure::same_file);
    ::close(a.src_fd);

    // Hard link alias (different path, same inode).
    std::string link = dir.file("link");
    SLUICE_CHECK(::link(src.c_str(), link.c_str()) == 0);
    SafeOpenOutcome b = open_atomic_copy(src, link);
    SLUICE_CHECK(b.failure == SafeOpenFailure::same_file);
    ::close(b.src_fd);
    ::unlink(link.c_str());
    SLUICE_CHECK(read_file(src) == "data");
}

SLUICE_TEST_CASE(safe_output_open_missing_dst_dir_reports_temp_create) {
    TempDir dir;
    std::string src = make_file(dir, "src", "x");
    std::string dst = dir.file("no-such-dir") + "/dst";
    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::temp_create);
    SLUICE_CHECK(o.error.os_errno == ENOENT);
    ::close(o.src_fd);
}

// ---------------------------------------------------------------------------
// commit_atomic_copy / discard_atomic_copy
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(safe_output_commit_replaces_dst_and_cleans_temp) {
    TempDir dir;
    std::string src = make_file(dir, "src", "ignored");
    std::string dst = make_file(dir, "dst", "OLD CONTENT");

    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);
    const char* n = "NEW CONTENT";
    SLUICE_CHECK(::write(o.temp_fd, n, std::strlen(n)) ==
                 static_cast<ssize_t>(std::strlen(n)));
    ::close(o.src_fd);

    auto r = commit_atomic_copy(o, dst, SyncPolicy::none);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(o.temp_fd == -1);
    SLUICE_CHECK(read_file(dst) == "NEW CONTENT");
    SLUICE_CHECK(count_temp_files(dir) == 0);
}

SLUICE_TEST_CASE(safe_output_commit_sync_all_dir_fsync_ok) {
    TempDir dir;
    std::string src = make_file(dir, "src", "x");
    std::string dst = dir.file("fresh");

    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);
    ::close(o.src_fd);
    auto r = commit_atomic_copy(o, dst, SyncPolicy::all);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(count_temp_files(dir) == 0);
    SLUICE_CHECK(read_file(dst).empty());  // nothing written to temp
}

SLUICE_TEST_CASE(safe_output_commit_rename_failure_unlinks_temp) {
    TempDir dir;
    std::string src = make_file(dir, "src", "x");
    std::string dst = dir.file("late-dir");

    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);
    ::close(o.src_fd);

    // Turn the destination path into a directory AFTER the open (rename over
    // a non-empty directory fails with EISDIR/ENOTEMPTY). Deterministic
    // rename-failure injection without touching production seams.
    SLUICE_CHECK(::mkdir(dst.c_str(), 0755) == 0);
    SLUICE_CHECK(::mkdir((dst + "/keep").c_str(), 0755) == 0);

    SafeCommitStage stage = SafeCommitStage::none;
    auto r = commit_atomic_copy(o, dst, SyncPolicy::none, &stage);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(stage == SafeCommitStage::rename);
    SLUICE_CHECK(r.error().os_errno == EISDIR || r.error().os_errno == ENOTEMPTY);
    SLUICE_CHECK(count_temp_files(dir) == 0);
    // The directory destination is untouched.
    struct stat st{};
    SLUICE_CHECK(::stat((dst + "/keep").c_str(), &st) == 0);

    ::rmdir((dst + "/keep").c_str());
    ::rmdir(dst.c_str());
}

SLUICE_TEST_CASE(safe_output_discard_keeps_existing_dst) {
    TempDir dir;
    std::string src = make_file(dir, "src", "x");
    std::string dst = make_file(dir, "dst", "KEEP ME");

    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);
    ::close(o.src_fd);
    discard_atomic_copy(o);
    // Idempotent.
    discard_atomic_copy(o);

    SLUICE_CHECK(read_file(dst) == "KEEP ME");
    SLUICE_CHECK(count_temp_files(dir) == 0);
}

// ---------------------------------------------------------------------------
// Full atomic flow with deterministic fault injection
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(atomic_flow_copy_error_preserves_existing_dst) {
    TempDir dir;
    std::string src = make_file(dir, "src", std::string(64, 'a'));
    std::string dst = make_file(dir, "dst", "PRECIOUS");

    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);

    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_error(IoError{IoError::Code::backend_error});
    auto r = run_pipelined_copy_with_backend(o.src_fd, o.temp_fd, 16, 1, 1,
                                             SyncPolicy::none,
                                             std::move(backend));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);

    discard_atomic_copy(o);
    ::close(o.src_fd);
    SLUICE_CHECK(read_file(dst) == "PRECIOUS");
    SLUICE_CHECK(count_temp_files(dir) == 0);
}

SLUICE_TEST_CASE(atomic_flow_canceled_copy_preserves_existing_dst) {
    TempDir dir;
    std::string src = make_file(dir, "src", std::string(64, 'a'));
    std::string dst = make_file(dir, "dst", "PRECIOUS");

    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);

    auto backend = std::make_unique<FakeAsyncBackend>();
    backend->auto_error(IoError{IoError::Code::canceled});
    auto r = run_pipelined_copy_with_backend(o.src_fd, o.temp_fd, 16, 1, 1,
                                             SyncPolicy::none,
                                             std::move(backend));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::canceled);

    discard_atomic_copy(o);
    ::close(o.src_fd);
    SLUICE_CHECK(read_file(dst) == "PRECIOUS");
    SLUICE_CHECK(count_temp_files(dir) == 0);
}

// ---------------------------------------------------------------------------
// Full atomic flow with the REAL backend
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(atomic_flow_real_backend_multibuffer_sync_all) {
    TempDir dir;
    // ~100 KiB deterministic pattern, mode 0640.
    std::string content;
    for (int i = 0; i < 100 * 1024; ++i)
        content.push_back(static_cast<char>((i * 7) & 0xff));
    std::string src = make_file(dir, "src", content, 0640);
    std::string dst = make_file(dir, "dst", "OLD");

    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);

    auto r = run_pipelined_copy(o.src_fd, o.temp_fd,
                                /*buffer_size=*/16 * 1024,
                                /*pipeline_depth=*/4, /*workers=*/2,
                                SyncPolicy::all);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value().bytes_copied == content.size());

    auto c = commit_atomic_copy(o, dst, SyncPolicy::all);
    SLUICE_CHECK(c.has_value());

    SLUICE_CHECK(read_file(dst) == content);
    struct stat st{};
    SLUICE_CHECK(::stat(dst.c_str(), &st) == 0);
    SLUICE_CHECK((st.st_mode & 07777) == 0640);  // source mode preserved
    SLUICE_CHECK(count_temp_files(dir) == 0);
    ::close(o.src_fd);
}

SLUICE_TEST_CASE(atomic_flow_empty_source_makes_empty_dst) {
    TempDir dir;
    std::string src = make_file(dir, "src", "");
    std::string dst = make_file(dir, "dst", "WILL BE EMPTIED");

    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);

    auto r = run_pipelined_copy(o.src_fd, o.temp_fd, 4096, 1, 1,
                                SyncPolicy::none);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value().bytes_copied == 0);

    auto c = commit_atomic_copy(o, dst, SyncPolicy::none);
    SLUICE_CHECK(c.has_value());
    SLUICE_CHECK(read_file(dst).empty());
    SLUICE_CHECK(count_temp_files(dir) == 0);
    ::close(o.src_fd);
}

// ---------------------------------------------------------------------------
// Directory fsync EINTR contract (#142 EINTR-001)
// ---------------------------------------------------------------------------
// The post-rename directory fsync is scripted via DirFsyncScript (armed only
// in this internal-testing build; production calls the real ::fsync). Each
// script's destructor disarms the seam, so these cases cannot affect the
// real-filesystem cases above.

// fsync #1 -> -1/EINTR, fsync #2 -> 0: the interruption must be retried, not
// reported as a durability failure.
SLUICE_TEST_CASE(safe_output_commit_dir_fsync_eintr_once_still_succeeds) {
    TempDir dir;
    std::string src = make_file(dir, "src", "x");
    std::string dst = make_file(dir, "dst", "OLD");
    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);
    const char* n = "NEW";
    SLUICE_CHECK(::write(o.temp_fd, n, std::strlen(n)) ==
                 static_cast<ssize_t>(std::strlen(n)));
    ::close(o.src_fd);

    testing::DirFsyncScript script({{-1, EINTR}, {0, 0}});
    SafeCommitStage stage = SafeCommitStage::none;
    auto r = commit_atomic_copy(o, dst, SyncPolicy::all, &stage);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(stage == SafeCommitStage::none);
    SLUICE_CHECK(script.calls() == 2);  // interrupted once, retried once
    SLUICE_CHECK(read_file(dst) == "NEW");
    SLUICE_CHECK(count_temp_files(dir) == 0);
}

// fsync #1 -> -1/EIO: a real fsync error is a genuine durability failure —
// it must surface with the rename already applied (missing durability, not a
// lost copy), and it must not be retried.
SLUICE_TEST_CASE(safe_output_commit_dir_fsync_real_eio_still_fails) {
    TempDir dir;
    std::string src = make_file(dir, "src", "x");
    std::string dst = make_file(dir, "dst", "OLD");
    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);
    const char* n = "NEW";
    SLUICE_CHECK(::write(o.temp_fd, n, std::strlen(n)) ==
                 static_cast<ssize_t>(std::strlen(n)));
    ::close(o.src_fd);

    testing::DirFsyncScript script({{-1, EIO}});
    SafeCommitStage stage = SafeCommitStage::none;
    auto r = commit_atomic_copy(o, dst, SyncPolicy::all, &stage);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(stage == SafeCommitStage::dir_sync);
    SLUICE_CHECK(r.error().os_errno == EIO);
    SLUICE_CHECK(script.calls() == 1);  // real errors are not retried
    SLUICE_CHECK(read_file(dst) == "NEW");
    SLUICE_CHECK(count_temp_files(dir) == 0);
}

// fsync #1 -> -1/EINTR, fsync #2 -> -1/EIO: EINTR triggers exactly one retry;
// the following real error terminates it and is reported verbatim — no
// infinite loop, no EINTR leaking to the caller.
SLUICE_TEST_CASE(safe_output_commit_dir_fsync_eintr_then_eio_reports_eio) {
    TempDir dir;
    std::string src = make_file(dir, "src", "x");
    std::string dst = make_file(dir, "dst", "OLD");
    SafeOpenOutcome o = open_atomic_copy(src, dst);
    SLUICE_CHECK(o.failure == SafeOpenFailure::none);
    const char* n = "NEW";
    SLUICE_CHECK(::write(o.temp_fd, n, std::strlen(n)) ==
                 static_cast<ssize_t>(std::strlen(n)));
    ::close(o.src_fd);

    testing::DirFsyncScript script({{-1, EINTR}, {-1, EIO}});
    SafeCommitStage stage = SafeCommitStage::none;
    auto r = commit_atomic_copy(o, dst, SyncPolicy::all, &stage);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(stage == SafeCommitStage::dir_sync);
    SLUICE_CHECK(r.error().os_errno == EIO);
    SLUICE_CHECK(script.calls() == 2);  // one retry, then stop at the error
    SLUICE_CHECK(read_file(dst) == "NEW");
    SLUICE_CHECK(count_temp_files(dir) == 0);
}

SLUICE_MAIN()
