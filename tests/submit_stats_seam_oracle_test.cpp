#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/async_backend.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::AsyncStats;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::Result;

class ScriptedBackend final : public AsyncBackend {
  public:
    IoError::Code spelling = IoError::Code::not_supported;
    mutable int outstanding_calls = 0;
    mutable std::vector<std::size_t> outstanding_script;
    std::size_t outstanding_default = 0;

    std::size_t poll() override { return 0; }
    std::size_t outstanding() const noexcept override {
        ++outstanding_calls;
        if (outstanding_script.empty())
            return outstanding_default;
        const std::size_t value = outstanding_script.front();
        outstanding_script.erase(outstanding_script.begin());
        return value;
    }
    bool internal_work_retired() const noexcept override { return true; }
    std::size_t slot_capacity() const noexcept override { return 0; }

    Result<detail::RequestKey> submit_read(ReadOp op) override {
        (void)op;
        return reject();
    }
    Result<detail::RequestKey> submit_write(WriteOp op) override {
        (void)op;
        return reject();
    }
    Result<detail::RequestKey> submit_sync_data(SyncDataOp op) override {
        (void)op;
        return reject();
    }
    Result<detail::RequestKey> submit_sync_all(SyncAllOp op) override {
        (void)op;
        return reject();
    }
    Result<detail::RequestKey> submit_file_info(FileInfoOp op) override {
        (void)op;
        return reject();
    }
    Result<detail::RequestKey> submit_size(SizeOp op) override {
        (void)op;
        return reject();
    }

    detail::PublicCancel cancel_identity(detail::RequestKey key) override {
        (void)key;
        return detail::PublicCancel::not_found;
    }

  private:
    Result<detail::RequestKey> reject() {
        return sluice::make_unexpected<detail::RequestKey>(IoError{spelling});
    }
};

std::string make_temp_path() {
    char path[] = "/tmp/sluice_submit_stats_seam_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    ::close(fd);
    return path;
}

File open_writable_file() {
    const std::string path = make_temp_path();
    FileOpen mode;
    mode.access = FileAccess::read_write;
    File file = std::move(File::open(path, mode).value());
    ::unlink(path.c_str());
    return file;
}

bool submit_entry_reports_scripted_error(AsyncIoContext& ctx, int which,
                                         const NativeFileRef& ref,
                                         IoError::Code expected) {
    ReadOp read{ref};
    WriteOp write{ref};
    SyncDataOp sync_data{ref};
    SyncAllOp sync_all{ref};
    FileInfoOp file_info{ref};
    SizeOp size{ref};
    const auto scripted = [expected](const auto& r) {
        return !r.has_value() && r.error().code == expected;
    };
    switch (which) {
    case 0:
        return scripted(ctx.submit_read(read));
    case 1:
        return scripted(ctx.submit_write(write));
    case 2:
        return scripted(ctx.submit_sync_data(sync_data));
    case 3:
        return scripted(ctx.submit_sync_all(sync_all));
    case 4:
        return scripted(ctx.submit_file_info(file_info));
    case 5:
        return scripted(ctx.submit_size(size));
    }
    return false;
}

bool stats_null_submit_samples_outstanding_exactly_when_unguarded() {
    File file = open_writable_file();
    const NativeFileRef ref{file.native_handle(), FileAccess::read_write};
    auto backend = std::make_unique<ScriptedBackend>();
    ScriptedBackend* b = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend));
    const IoError::Code spellings[] = {IoError::Code::would_block,
                                       IoError::Code::invalid_state,
                                       IoError::Code::not_supported};
    for (const IoError::Code code : spellings) {
        b->spelling = code;
        for (int which = 0; which < 6; ++which) {
            if (!submit_entry_reports_scripted_error(ctx, which, ref, code))
                return false;
        }
    }
#if defined(SLUICE_RESEARCH_ELIDE_001)
    return b->outstanding_calls == 0;
#else
    return b->outstanding_calls == 18;
#endif
}

bool stats_on_submit_still_samples_outstanding_once_per_submit() {
    File file = open_writable_file();
    const NativeFileRef ref{file.native_handle(), FileAccess::read_write};
    auto backend = std::make_unique<ScriptedBackend>();
    ScriptedBackend* b = backend.get();
    AsyncStats stats;
    AsyncIoContext ctx = detail::make_context(std::move(backend), &stats);
    const IoError::Code spellings[] = {IoError::Code::would_block,
                                       IoError::Code::invalid_state,
                                       IoError::Code::not_supported};
    for (const IoError::Code code : spellings) {
        b->spelling = code;
        for (int which = 0; which < 6; ++which) {
            if (!submit_entry_reports_scripted_error(ctx, which, ref, code))
                return false;
        }
    }
    return b->outstanding_calls == 18 && stats.submit_calls == 18 &&
           stats.submitted_ops == 0 && stats.queue_full_retries == 6 &&
           stats.invalid_state_rejections == 6;
}

bool max_outstanding_samples_every_submit_including_failures() {
    File file = open_writable_file();
    const NativeFileRef ref{file.native_handle(), FileAccess::read_write};
    for (int which = 0; which < 6; ++which) {
        {
            auto backend = std::make_unique<ScriptedBackend>();
            ScriptedBackend* b = backend.get();
            b->spelling = IoError::Code::would_block;
            b->outstanding_script = {3, 7, 7, 2};
            AsyncStats stats;
            AsyncIoContext ctx = detail::make_context(std::move(backend), &stats);
            for (int i = 0; i < 4; ++i) {
                if (!submit_entry_reports_scripted_error(ctx, which, ref,
                                                         IoError::Code::would_block))
                    return false;
            }
            if (stats.max_outstanding != 7 || stats.submit_calls != 4 ||
                stats.queue_full_retries != 4 || stats.submitted_ops != 0)
                return false;
        }
        {
            auto backend = std::make_unique<ScriptedBackend>();
            ScriptedBackend* b = backend.get();
            b->spelling = IoError::Code::would_block;
            b->outstanding_script = {9, 1, 1};
            AsyncStats stats;
            AsyncIoContext ctx = detail::make_context(std::move(backend), &stats);
            for (int i = 0; i < 3; ++i) {
                if (!submit_entry_reports_scripted_error(ctx, which, ref,
                                                         IoError::Code::would_block))
                    return false;
            }
            if (stats.max_outstanding != 9 || stats.queue_full_retries != 3)
                return false;
        }
        {
            auto backend = std::make_unique<ScriptedBackend>();
            ScriptedBackend* b = backend.get();
            b->spelling = IoError::Code::invalid_state;
            b->outstanding_default = 4;
            AsyncStats stats;
            AsyncIoContext ctx = detail::make_context(std::move(backend), &stats);
            if (!submit_entry_reports_scripted_error(ctx, which, ref,
                                                     IoError::Code::invalid_state))
                return false;
            if (stats.max_outstanding != 4 || stats.invalid_state_rejections != 1 ||
                stats.submit_calls != 1 || stats.submitted_ops != 0 ||
                stats.queue_full_retries != 0)
                return false;
        }
    }
    return true;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"stats_null_submit_samples_outstanding_exactly_when_unguarded",
         stats_null_submit_samples_outstanding_exactly_when_unguarded},
        {"stats_on_submit_still_samples_outstanding_once_per_submit",
         stats_on_submit_still_samples_outstanding_once_per_submit},
        {"max_outstanding_samples_every_submit_including_failures",
         max_outstanding_samples_every_submit_including_failures},
    };
    int failed = 0;
    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            ++failed;
        }
    }
    const int total = static_cast<int>(sizeof(tests) / sizeof(tests[0]));
    if (failed != 0) {
        std::fprintf(stderr, "%d of %d submit stats seam tests failed\n", failed,
                     total);
        return 1;
    }
    std::printf("all %d submit stats seam tests passed\n", total);
    return 0;
}
