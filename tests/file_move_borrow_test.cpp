// File-owner moves interleaved with outstanding accepted borrows. The legal
// interleavings come from the borrow contract: moving the owner is permitted
// while a request holds the native resource, the request must settle against
// the same descriptor, and only after acquired terminal may the new owner
// close. Move-assigning over or closing the borrowed object itself is a
// caller violation and is deliberately not driven here.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(SLUICE_HAS_LIBURING)
#include <sluice/async/uring_backend.hpp>
#endif

#if defined(SLUICE_LIFE_MUTANT_MOVE_CLOSES_NATIVE)
namespace sluice::detail {
extern bool life_mutant_armed;
}
#endif

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::unique_ptr<AsyncBackend> make_backend() {
#if defined(SLUICE_HAS_LIBURING)
    return std::make_unique<UringAsyncBackend>();
#else
    return std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 2});
#endif
}

bool uring_profile_unavailable() {
#if defined(SLUICE_HAS_LIBURING)
    try {
        return !UringAsyncBackend().available();
    } catch (...) {
        return true;
    }
#else
    return false;
#endif
}

std::string temp_path(const char* tag) {
    std::string p = std::string("/tmp/sluice_move_borrow_") + tag + "_XXXXXX";
    std::vector<char> buf(p.begin(), p.end());
    buf.push_back('\0');
    const int fd = ::mkstemp(buf.data());
    if (fd < 0)
        return {};
    ::close(fd);
    ::unlink(buf.data());
    return std::string(buf.data());
}

constexpr std::size_t kPayload = 256;

std::vector<std::byte> pattern(std::size_t n) {
    std::vector<std::byte> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<std::byte>((i * 31 + 7) & 0xff);
    return out;
}

template <class T> void drive_ready(AsyncIoContext& ctx, Request<T>& request) {
    while (!request.ready())
        (void)ctx.poll();
}

bool fill_file(const std::string& path, const std::vector<std::byte>& body) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    const std::size_t n = std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    return n == body.size();
}

// The owner moves after acceptance and before settlement. The accepted read
// must still return exactly the stored payload through the moved descriptor,
// and the new owner must be able to close only after consuming the terminal.
bool moved_owner_keeps_outstanding_borrow_settling() {
    const std::string path = temp_path("settle");
    const std::vector<std::byte> body = pattern(kPayload);
    if (path.empty() || !fill_file(path, body)) {
        check(false, "fixture created");
        return false;
    }

    auto opened = File::open(path, FileOpen{});
    check(opened.has_value(), "fixture open");
    if (!opened.has_value())
        return false;

    AsyncIoContext ctx(make_backend());
    std::vector<std::byte> buffer(body.size(), std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef{opened.value()}, buffer.data(),
                                            buffer.size(), 0});
    check(submitted.has_value(), "read accepted before the move");
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted.value());

#if defined(SLUICE_LIFE_MUTANT_MOVE_CLOSES_NATIVE)
    sluice::detail::life_mutant_armed = true;
#endif
    File target = std::move(opened.value());
#if defined(SLUICE_LIFE_MUTANT_MOVE_CLOSES_NATIVE)
    sluice::detail::life_mutant_armed = false;
#endif
    check(!opened.value().is_open(), "the moved-from owner releases the resource");

    drive_ready(ctx, request);
    check(request.ready(), "the outstanding read settles after the owner move");
    auto observed = request.take_result();
    check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
              observed.result.value() == body.size(),
          "the settled count matches the stored payload length");
    check(std::memcmp(buffer.data(), body.data(), body.size()) == 0,
          "the settled payload matches the stored bytes through the moved descriptor");

    const auto closed = target.close();
    check(closed.has_value(), "the new owner closes cleanly after acquired terminal");
    const auto after = sluice::blocking::read(target, std::span<std::byte>(buffer));
    check(!after.has_value() && after.error().code == IoError::Code::invalid_state,
          "the closed moved-to owner rejects further use");
    ::unlink(path.c_str());
    return true;
}

// Submission facts are captured at initiation, so a second operation may be
// initiated through the new owner at a different address while the first
// borrow is still outstanding; both must settle against their own regions.
bool second_submission_through_moved_owner_settles() {
    const std::string path = temp_path("second");
    const std::vector<std::byte> body = pattern(kPayload * 2);
    if (path.empty() || !fill_file(path, body)) {
        check(false, "fixture created");
        return false;
    }

    auto opened = File::open(path, FileOpen{});
    check(opened.has_value(), "fixture open");
    if (!opened.has_value())
        return false;

    AsyncIoContext ctx(make_backend());
    std::vector<std::byte> low(body.size() / 2, std::byte{0});
    std::vector<std::byte> high(body.size() / 2, std::byte{0});
    auto first = ctx.submit_read(ReadOp{NativeFileRef{opened.value()}, low.data(), low.size(), 0});
    check(first.has_value(), "first read accepted");
    if (!first.has_value())
        return false;

#if defined(SLUICE_LIFE_MUTANT_MOVE_CLOSES_NATIVE)
    sluice::detail::life_mutant_armed = true;
#endif
    File target = std::move(opened.value());
#if defined(SLUICE_LIFE_MUTANT_MOVE_CLOSES_NATIVE)
    sluice::detail::life_mutant_armed = false;
#endif
    auto second = ctx.submit_read(
        ReadOp{NativeFileRef{target}, high.data(), high.size(), low.size()});
    check(second.has_value(), "second read accepted through the moved owner");
    if (!second.has_value())
        return false;

    Request<std::size_t> first_request = std::move(first.value());
    Request<std::size_t> second_request = std::move(second.value());
    drive_ready(ctx, first_request);
    drive_ready(ctx, second_request);
    auto first_outcome = first_request.take_result();
    auto second_outcome = second_request.take_result();
    check(first_outcome.result.has_value() && first_outcome.result.value() == low.size() &&
              std::memcmp(low.data(), body.data(), low.size()) == 0,
          "the pre-move borrow settles its own region");
    check(second_outcome.result.has_value() &&
              second_outcome.result.value() == high.size() &&
              std::memcmp(high.data(), body.data() + low.size(), high.size()) == 0,
          "the post-move submission settles its own region");
    check(ctx.outstanding() == 0, "both borrows retire at publication");
    (void)target.close();
    ::unlink(path.c_str());
    return true;
}

}

int main() {
    if (uring_profile_unavailable()) {
        std::printf("NOT RUN: io_uring unavailable on this host (kernel/policy blocked)\n");
        return 0;
    }
    moved_owner_keeps_outstanding_borrow_settling();
    second_submission_through_moved_owner_settles();
    if (g_failures != 0) {
        std::fprintf(stderr, "file move borrow oracle: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("file move borrow oracle passed\n");
    return 0;
}
