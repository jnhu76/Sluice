#include "copy_task.hpp"
#include "safe_output.hpp"
#include "safe_output_test_seams.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice_copy::SafeCommitStage;
using sluice_copy::SafeOpenFailure;
using sluice_copy::SyncPolicy;
using sluice_copy::testing::DirFsyncScript;

constexpr std::size_t kBufferSize = 4096;
constexpr std::size_t kPipelineDepth = 4;
constexpr unsigned kWorkers = 2;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_app_copy_dirsync_src_XXXXXX";
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
    char path[] = "/tmp/sluice_app_copy_dirsync_XXXXXX";
    if (::mkdtemp(path) == nullptr)
        return {};
    return path;
}

std::string read_file(const std::string& path) {
    FILE* f = ::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return {};
    std::string out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    ::fclose(f);
    return out;
}

struct SyncScriptCase {
    sluice::Result<void> commit;
    SafeCommitStage stage = SafeCommitStage::none;
    std::size_t dir_fsync_calls = 0;
    std::string copied;
    bool dst_exists = false;
};

SyncScriptCase commit_with_dir_fsync_script(std::vector<DirFsyncScript::Step> steps) {
    SyncScriptCase out;
    const std::string content = "directory fsync durability payload";
    const std::string src = make_temp_file(content);
    const std::string dir = make_temp_dir();
    if (src.empty() || dir.empty())
        return out;
    const std::string dst = dir + "/dst.bin";

    sluice_copy::SafeOpenOutcome oc = sluice_copy::open_atomic_copy(src, dst);
    ::unlink(src.c_str());
    if (oc.failure != SafeOpenFailure::none) {
        ::rmdir(dir.c_str());
        return out;
    }
    const auto written = sluice_copy::run_pipelined_copy(
        *oc.src_file, sluice::async::NativeFileRef{oc.temp_fd, sluice::FileAccess::read_write},
        kBufferSize, kPipelineDepth, kWorkers, SyncPolicy::none);
    if (!written.has_value()) {
        sluice_copy::discard_atomic_copy(oc);
        ::rmdir(dir.c_str());
        return out;
    }

    {
        DirFsyncScript script(std::move(steps));
        out.commit = sluice_copy::commit_atomic_copy(oc, dst, SyncPolicy::data, &out.stage);
        out.dir_fsync_calls = script.calls();
    }
    out.copied = read_file(dst);
    out.dst_exists = ::access(dst.c_str(), F_OK) == 0;
    ::unlink(dst.c_str());
    ::rmdir(dir.c_str());
    return out;
}

// The injected interruption is only survived when the directory fsync runs
// through the shared retry authority; a single unretried attempt must surface
// EINTR as a durability failure instead of durable success.
bool directory_fsync_eintr_is_retried_to_success() {
    const auto c = commit_with_dir_fsync_script({{-1, EINTR}, {0, 0}});
    return c.commit.has_value() && c.stage == SafeCommitStage::none &&
           c.dir_fsync_calls == 2 && c.dst_exists && !c.copied.empty();
}

bool every_interruption_is_retried() {
    const auto c = commit_with_dir_fsync_script({{-1, EINTR}, {-1, EINTR}, {0, 0}});
    return c.commit.has_value() && c.stage == SafeCommitStage::none && c.dir_fsync_calls == 3 &&
           c.dst_exists;
}

// The retry covers interruption only: the terminal error after an interrupted
// attempt is reported, not swallowed.
bool real_error_after_interruption_is_reported() {
    const auto c = commit_with_dir_fsync_script({{-1, EINTR}, {-1, EIO}});
    return !c.commit.has_value() && c.stage == SafeCommitStage::dir_sync &&
           c.dir_fsync_calls == 2 && c.commit.error().os_errno == EIO && c.dst_exists;
}

bool terminal_error_is_not_retried() {
    const auto c = commit_with_dir_fsync_script({{-1, EIO}});
    return !c.commit.has_value() && c.stage == SafeCommitStage::dir_sync &&
           c.dir_fsync_calls == 1 && c.commit.error().os_errno == EIO;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"directory_fsync_eintr_is_retried_to_success", directory_fsync_eintr_is_retried_to_success},
        {"every_interruption_is_retried", every_interruption_is_retried},
        {"real_error_after_interruption_is_reported", real_error_after_interruption_is_reported},
        {"terminal_error_is_not_retried", terminal_error_is_not_retried},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu app copy directory-fsync tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
