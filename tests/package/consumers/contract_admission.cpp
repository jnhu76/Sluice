// Clean-room micro-contract probe: admission rejection precedence and
// boundedness. Built against the installed prefix.
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

std::string make_temp_path() {
    char path[] = "/tmp/sluice_f1_cadm_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    ::close(fd);
    return path;
}

sluice::FileOpen create_read_write() {
    sluice::FileOpen mode;
    mode.access = sluice::FileAccess::read_write;
    mode.existence = sluice::FileExistence::create_if_missing;
    return mode;
}

} // namespace

int main() {
    using namespace sluice;
    using namespace sluice::async;

    const std::string path = make_temp_path();
    auto opened = File::open(path, create_read_write());
    CHECK(opened.has_value());
    File file = std::move(opened).value();
    const std::string payload = "admission";
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    CHECK(sluice::blocking::write_all(file, bytes).has_value());

    AsyncIoContext ctx(
        std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{.request_capacity = 2,
                                                              .worker_count = 1}));

    // Invalid resource (negative fd): rejected at submission, no retained
    // borrow, no background effect.
    {
        const NativeFileRef bad{-1, FileAccess::read_only};
        std::vector<std::byte> buf(4, std::byte{0});
        auto r = ctx.submit_read(ReadOp{.file = bad, .dst = buf.data(), .len = buf.size()});
        CHECK(!r.has_value());
        CHECK(r.error().code == IoError::Code::invalid_state);
        CHECK(ctx.outstanding() == 0);
    }

    // Illegal access: write submitted against a read-only descriptor is
    // invalid_argument before acceptance.
    {
        auto ro = File::open(path, [] {
            sluice::FileOpen mode;
            mode.access = FileAccess::read_only;
            return mode;
        }());
        CHECK(ro.has_value());
        const NativeFileRef ro_ref{ro.value()};
        auto r = ctx.submit_write(WriteOp{.file = ro_ref,
                                          .src = bytes.data(),
                                          .len = bytes.size(),
                                          .offset = 0});
        CHECK(!r.has_value());
        CHECK(r.error().code == IoError::Code::invalid_argument);
        CHECK(ctx.outstanding() == 0);
        CHECK(ro.value().close().has_value());
    }

    // Zero buffer with an invalid resource / illegal access: the resource and
    // access rejections take precedence over the zero-length no-op.
    {
        const NativeFileRef bad{-1, FileAccess::read_only};
        auto r = ctx.submit_read(ReadOp{.file = bad, .dst = nullptr, .len = 0});
        CHECK(!r.has_value());
        CHECK(r.error().code == IoError::Code::invalid_state);

        auto ro = File::open(path, [] {
            sluice::FileOpen mode;
            mode.access = FileAccess::read_only;
            return mode;
        }());
        CHECK(ro.has_value());
        auto w = ctx.submit_write(
            WriteOp{.file = NativeFileRef{ro.value()}, .src = nullptr, .len = 0});
        CHECK(!w.has_value());
        CHECK(w.error().code == IoError::Code::invalid_argument);
        CHECK(ro.value().close().has_value());
    }

    // Zero-length request: legal, needs one slot, publishes immediately ready,
    // never dispatches a data syscall.
    {
        auto r = ctx.submit_read(
            ReadOp{.file = NativeFileRef{file}, .dst = nullptr, .len = 0, .offset = 0});
        CHECK(r.has_value());
        CHECK(r.value().ready());
        const auto observation = r.value().take_result();
        CHECK(observation.readiness == RequestReadiness::ready);
        CHECK(observation.result.has_value() && observation.result.value() == 0);
        // Released bindings reclaim their slots through a progress pass
        // Slot reclaim is driven by a progress pass, never assumed.
        (void)ctx.poll();
    }

    // Full table: a valid submission is rejected while retained results hold
    // the slots; the identical direct operation still succeeds.
    {
        auto a = ctx.submit_read(
            ReadOp{.file = NativeFileRef{file}, .dst = nullptr, .len = 0, .offset = 0});
        auto b = ctx.submit_read(
            ReadOp{.file = NativeFileRef{file}, .dst = nullptr, .len = 0, .offset = 0});
        CHECK(a.has_value() && b.has_value());
        auto full = ctx.submit_read(
            ReadOp{.file = NativeFileRef{file}, .dst = nullptr, .len = 0, .offset = 0});
        CHECK(!full.has_value());
        CHECK(full.error().code == IoError::Code::would_block);
        CHECK(ctx.admission_open());

        std::vector<std::byte> direct_buf(2, std::byte{0});
        auto direct = sluice::blocking::read_at(file, 0, direct_buf);
        CHECK(direct.has_value() && direct.value() == 2);

        if (a.has_value())
            (void)a.value().take_result();
        if (b.has_value())
            (void)b.value().take_result();
    }

    // Admission close: after close_admission, submissions are rejected with
    // invalid_state and the context reports admission closed.
    ctx.close_admission();
    CHECK(!ctx.admission_open());
    {
        auto r = ctx.submit_read(
            ReadOp{.file = NativeFileRef{file}, .dst = nullptr, .len = 0, .offset = 0});
        CHECK(!r.has_value());
        CHECK(r.error().code == IoError::Code::invalid_state);
    }

    CHECK(ctx.shutdown(ShutdownPolicy::drain).has_value());
    file.close();
    ::unlink(path.c_str());

    if (g_failures != 0) {
        std::fprintf(stderr, "contract_admission: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("contract_admission: PASS\n");
    return 0;
}
