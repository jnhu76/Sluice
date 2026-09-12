#include "copy_task.hpp"
#include "file_domain.hpp"
#include "safe_output.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using sluice_copy::OpenCopyFailure;
using sluice_copy::SafeOpenFailure;
using sluice_copy::SyncPolicy;

constexpr std::size_t kBufferSize = 4096;
constexpr std::size_t kPipelineDepth = 4;
constexpr unsigned kWorkers = 2;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_app_copy_src_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    std::string out_path = path;
    std::size_t written = 0;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            ::close(fd);
            ::unlink(out_path.c_str());
            return {};
        }
        written += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return out_path;
}

std::string make_temp_dir() {
    char path[] = "/tmp/sluice_app_copy_dst_XXXXXX";
    if (::mkdtemp(path) == nullptr)
        return {};
    return path;
}

std::string read_file(const std::string& path) {
    std::string content;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return content;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        content.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return content;
}

std::size_t count_temp_files(const std::string& dir) {
    std::size_t count = 0;
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return 0;
    while (dirent* e = ::readdir(d)) {
        if (std::strncmp(e->d_name, ".sluice-copy.tmp.", 17) == 0)
            ++count;
    }
    ::closedir(d);
    return count;
}

bool copy_non_atomic_roundtrip() {
    std::string content(3 * kBufferSize + 1234, '\0');
    for (std::size_t i = 0; i < content.size(); ++i)
        content[i] = static_cast<char>(i % 251);
    const std::string src = make_temp_file(content);
    const std::string dst_dir = make_temp_dir();
    if (src.empty() || dst_dir.empty())
        return false;
    const std::string dst = dst_dir + "/dst.bin";

    sluice_copy::OpenCopyOutcome oc = sluice_copy::open_copy_files(src, dst);
    if (oc.failure != OpenCopyFailure::none) {
        ::unlink(src.c_str());
        ::rmdir(dst_dir.c_str());
        return false;
    }
    if (!oc.src_file.has_value() || oc.dst_fd < 0) {
        ::unlink(src.c_str());
        ::rmdir(dst_dir.c_str());
        return false;
    }

    const auto result =
        sluice_copy::run_pipelined_copy(*oc.src_file, sluice::async::NativeFileRef{oc.dst_fd, sluice::FileAccess::write_only},
                                        kBufferSize, kPipelineDepth, kWorkers, SyncPolicy::none);
    ::close(oc.dst_fd);
    const std::string copied = read_file(dst);
    const bool ok =
        result.has_value() && result.value().bytes_copied == content.size() && copied == content;
    ::unlink(src.c_str());
    ::unlink(dst.c_str());
    ::rmdir(dst_dir.c_str());
    return ok;
}

bool outcome_move_transfers_source_ownership() {
    const std::string content = "move-ownership payload";
    const std::string src = make_temp_file(content);
    const std::string dst_dir = make_temp_dir();
    if (src.empty() || dst_dir.empty())
        return false;
    const std::string dst = dst_dir + "/dst.bin";

    sluice_copy::OpenCopyOutcome oc = sluice_copy::open_copy_files(src, dst);
    ::unlink(src.c_str());
    if (oc.failure != OpenCopyFailure::none) {
        ::rmdir(dst_dir.c_str());
        return false;
    }
    sluice_copy::OpenCopyOutcome moved = std::move(oc);
    if (!oc.src_file.has_value() || oc.src_file->is_open()) {
        ::close(moved.dst_fd);
        ::unlink(dst.c_str());
        ::rmdir(dst_dir.c_str());
        return false;
    }

    const auto result =
        sluice_copy::run_pipelined_copy(*moved.src_file, sluice::async::NativeFileRef{moved.dst_fd, sluice::FileAccess::write_only},
                                        kBufferSize, kPipelineDepth, kWorkers, SyncPolicy::none);
    ::close(moved.dst_fd);
    const std::string copied = read_file(dst);
    const bool ok = result.has_value() && copied == content;
    ::unlink(dst.c_str());
    ::rmdir(dst_dir.c_str());
    return ok;
}

bool open_copy_files_missing_source_fails() {
    const std::string dst_dir = make_temp_dir();
    if (dst_dir.empty())
        return false;
    const std::string dst = dst_dir + "/dst.bin";

    sluice_copy::OpenCopyOutcome oc =
        sluice_copy::open_copy_files("/tmp/sluice_app_copy_no_such_file", dst);
    ::unlink(dst.c_str());
    ::rmdir(dst_dir.c_str());
    return oc.failure == OpenCopyFailure::src_open && oc.error.os_errno == ENOENT &&
           !oc.src_file.has_value() && oc.dst_fd < 0;
}

bool open_copy_files_directory_source_fails() {
    const std::string src_dir = make_temp_dir();
    const std::string dst_dir = make_temp_dir();
    if (src_dir.empty() || dst_dir.empty())
        return false;
    const std::string dst = dst_dir + "/dst.bin";

    sluice_copy::OpenCopyOutcome oc = sluice_copy::open_copy_files(src_dir, dst);
    ::unlink(dst.c_str());
    ::rmdir(src_dir.c_str());
    ::rmdir(dst_dir.c_str());
    return oc.failure == OpenCopyFailure::src_not_regular && !oc.src_file.has_value() &&
           oc.dst_fd < 0;
}

bool open_copy_files_same_file_fails() {
    const std::string content = "same file";
    const std::string path = make_temp_file(content);
    if (path.empty())
        return false;

    sluice_copy::OpenCopyOutcome oc = sluice_copy::open_copy_files(path, path);
    ::unlink(path.c_str());
    return oc.failure == OpenCopyFailure::same_file && !oc.src_file.has_value() && oc.dst_fd < 0;
}

bool open_copy_files_destination_open_failure_closes_source() {
    const std::string content = "destination failure";
    const std::string src = make_temp_file(content);
    if (src.empty())
        return false;

    sluice_copy::OpenCopyOutcome oc = sluice_copy::open_copy_files(src, "/tmp/no_such_dir/dst.bin");
    ::unlink(src.c_str());
    return oc.failure == OpenCopyFailure::dst_open && oc.error.os_errno == ENOENT &&
           !oc.src_file.has_value() && oc.dst_fd < 0;
}

bool atomic_copy_roundtrip_with_commit() {
    std::string content(2 * kBufferSize + 77, '\0');
    for (std::size_t i = 0; i < content.size(); ++i)
        content[i] = static_cast<char>(i % 249);
    const std::string src = make_temp_file(content);
    const std::string dst_dir = make_temp_dir();
    if (src.empty() || dst_dir.empty())
        return false;
    const std::string dst = dst_dir + "/dst.bin";

    sluice_copy::SafeOpenOutcome oc = sluice_copy::open_atomic_copy(src, dst);
    ::unlink(src.c_str());
    if (oc.failure != SafeOpenFailure::none || !oc.src_file.has_value() || oc.temp_fd < 0) {
        ::rmdir(dst_dir.c_str());
        return false;
    }

    const auto result =
        sluice_copy::run_pipelined_copy(*oc.src_file, sluice::async::NativeFileRef{oc.temp_fd, sluice::FileAccess::read_write},
                                        kBufferSize, kPipelineDepth, kWorkers, SyncPolicy::none);
    if (!result.has_value()) {
        sluice_copy::discard_atomic_copy(oc);
        ::rmdir(dst_dir.c_str());
        return false;
    }

    const auto commit = sluice_copy::commit_atomic_copy(oc, dst, SyncPolicy::none);
    const std::string copied = read_file(dst);
    const bool ok = commit.has_value() && oc.temp_path.empty() &&
                    ::access(dst.c_str(), F_OK) == 0 && copied == content &&
                    count_temp_files(dst_dir) == 0;
    ::unlink(dst.c_str());
    ::rmdir(dst_dir.c_str());
    return ok;
}

bool atomic_discard_removes_temp_file() {
    const std::string content = "discard me";
    const std::string src = make_temp_file(content);
    const std::string dst_dir = make_temp_dir();
    if (src.empty() || dst_dir.empty())
        return false;
    const std::string dst = dst_dir + "/dst.bin";

    sluice_copy::SafeOpenOutcome oc = sluice_copy::open_atomic_copy(src, dst);
    ::unlink(src.c_str());
    if (oc.failure != SafeOpenFailure::none) {
        ::rmdir(dst_dir.c_str());
        return false;
    }
    if (count_temp_files(dst_dir) != 1) {
        sluice_copy::discard_atomic_copy(oc);
        ::rmdir(dst_dir.c_str());
        return false;
    }

    sluice_copy::discard_atomic_copy(oc);
    const bool ok = count_temp_files(dst_dir) == 0 && oc.temp_fd < 0 && oc.temp_path.empty();
    ::rmdir(dst_dir.c_str());
    return ok;
}

bool atomic_missing_source_fails() {
    const std::string dst_dir = make_temp_dir();
    if (dst_dir.empty())
        return false;
    const std::string dst = dst_dir + "/dst.bin";

    sluice_copy::SafeOpenOutcome oc =
        sluice_copy::open_atomic_copy("/tmp/sluice_app_copy_no_such_file", dst);
    ::rmdir(dst_dir.c_str());
    return oc.failure == SafeOpenFailure::src_open && oc.error.os_errno == ENOENT &&
           !oc.src_file.has_value() && oc.temp_fd < 0;
}

bool atomic_same_file_fails_without_temp() {
    const std::string content = "atomic same file";
    const std::string path = make_temp_file(content);
    if (path.empty())
        return false;

    sluice_copy::SafeOpenOutcome oc = sluice_copy::open_atomic_copy(path, path);
    ::unlink(path.c_str());
    return oc.failure == SafeOpenFailure::same_file && !oc.src_file.has_value() && oc.temp_fd < 0 &&
           oc.temp_path.empty();
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"copy_non_atomic_roundtrip", copy_non_atomic_roundtrip},
        {"outcome_move_transfers_source_ownership", outcome_move_transfers_source_ownership},
        {"open_copy_files_missing_source_fails", open_copy_files_missing_source_fails},
        {"open_copy_files_directory_source_fails", open_copy_files_directory_source_fails},
        {"open_copy_files_same_file_fails", open_copy_files_same_file_fails},
        {"open_copy_files_destination_open_failure_closes_source",
         open_copy_files_destination_open_failure_closes_source},
        {"atomic_copy_roundtrip_with_commit", atomic_copy_roundtrip_with_commit},
        {"atomic_discard_removes_temp_file", atomic_discard_removes_temp_file},
        {"atomic_missing_source_fails", atomic_missing_source_fails},
        {"atomic_same_file_fails_without_temp", atomic_same_file_fails_without_temp},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu app copy consumption tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
