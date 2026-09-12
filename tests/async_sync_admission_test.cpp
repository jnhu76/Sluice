#include <sluice/async/async_io_context.hpp>
#include <sluice/async/file.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstdio>
#include <memory>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::Result;

// Counts every backend sync entry. The closed-resource rejection belongs to
// the initiation boundary (ADR-0002 §5.5 precedence: closed -> invalid_state
// before any backend effect), so a rejected submission must leave these at
// zero: any backend entry means the canonical admission was bypassed.
class CountingBackend final : public AsyncBackend {
  public:
    int sync_data_entries = 0;
    int sync_all_entries = 0;

    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override {
        return sluice::make_unexpected<std::size_t>(IoError{IoError::Code::not_supported});
    }
    std::size_t outstanding() const noexcept override { return 0; }
    bool supports_request_identity() const noexcept override { return true; }

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        (void)op;
        (void)c;
        return sluice::make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        (void)op;
        (void)c;
        return sluice::make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override {
        (void)op;
        (void)c;
        ++sync_data_entries;
        return sluice::make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override {
        (void)op;
        (void)c;
        ++sync_all_entries;
        return sluice::make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
};

std::string make_temp_path() {
    char path[] = "/tmp/sluice_sync_admission_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    ::close(fd);
    return path;
}

// A closed canonical File: the reference it yields carries fd = -1, the
// shape the initiation boundary must reject before any backend effect.
File closed_writable_file() {
    const std::string path = make_temp_path();
    FileOpen mode;
    mode.access = FileAccess::read_write;
    File file = std::move(File::open(path, mode).value());
    ::unlink(path.c_str());
    (void)file.close();
    return file;
}

bool submit_sync_data_rejects_closed_reference_before_backend() {
    File file = closed_writable_file();

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<void> c;
    auto r = ctx.submit_sync_data(SyncDataOp{file}, c);
    if (r.has_value())
        return false;
    if (r.error().code != IoError::Code::invalid_state)
        return false;
    if (!c.idle())
        return false;
    return counts->sync_data_entries == 0 && counts->sync_all_entries == 0;
}

bool submit_sync_all_rejects_closed_reference_before_backend() {
    File file = closed_writable_file();

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<void> c;
    auto r = ctx.submit_sync_all(SyncAllOp{file}, c);
    if (r.has_value())
        return false;
    if (r.error().code != IoError::Code::invalid_state)
        return false;
    if (!c.idle())
        return false;
    return counts->sync_data_entries == 0 && counts->sync_all_entries == 0;
}

bool submit_sync_data_request_rejects_closed_reference_before_backend() {
    File file = closed_writable_file();

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<void> c;
    auto r = ctx.submit_sync_data_request(SyncDataOp{file}, c);
    if (r.has_value())
        return false;
    if (r.error().code != IoError::Code::invalid_state)
        return false;
    if (!c.idle())
        return false;
    return counts->sync_data_entries == 0 && counts->sync_all_entries == 0;
}

bool submit_sync_all_request_rejects_closed_reference_before_backend() {
    File file = closed_writable_file();

    auto backend = std::make_unique<CountingBackend>();
    CountingBackend* counts = backend.get();
    AsyncIoContext ctx(std::move(backend));

    Completion<void> c;
    auto r = ctx.submit_sync_all_request(SyncAllOp{file}, c);
    if (r.has_value())
        return false;
    if (r.error().code != IoError::Code::invalid_state)
        return false;
    if (!c.idle())
        return false;
    return counts->sync_data_entries == 0 && counts->sync_all_entries == 0;
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"submit_sync_data_rejects_closed_reference_before_backend",
         submit_sync_data_rejects_closed_reference_before_backend},
        {"submit_sync_all_rejects_closed_reference_before_backend",
         submit_sync_all_rejects_closed_reference_before_backend},
        {"submit_sync_data_request_rejects_closed_reference_before_backend",
         submit_sync_data_request_rejects_closed_reference_before_backend},
        {"submit_sync_all_request_rejects_closed_reference_before_backend",
         submit_sync_all_request_rejects_closed_reference_before_backend},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu async sync admission tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
