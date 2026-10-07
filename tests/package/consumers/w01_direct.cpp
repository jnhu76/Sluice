// Clean-room P0 consumer (W-01): canonical direct/core surface only, built
// against the installed prefix. Link requirement: libsluice_core only.
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace sluice::async {
class AsyncIoContext;
class ApplicationRuntime;
class Fiber;
class Scheduler;
namespace detail {
class RequestArena;
}
}

template <class T, class = void> struct is_complete : std::false_type {};
template <class T> struct is_complete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

static_assert(!is_complete<sluice::async::AsyncIoContext>::value);
static_assert(!is_complete<sluice::async::Scheduler>::value);
static_assert(!is_complete<sluice::async::Fiber>::value);
static_assert(!is_complete<sluice::async::ApplicationRuntime>::value);
static_assert(!is_complete<sluice::async::detail::RequestArena>::value);

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
    char path[] = "/tmp/sluice_f1_w01_XXXXXX";
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
    using sluice::blocking::EffectCertainty;
    using sluice::File;
    using sluice::FileKind;

    // W-01 trace: open, minimal metadata, positional write/read, explicit
    // durability, resize, explicit close, content check.
    {
        const std::string path = make_temp_path();
        CHECK(!path.empty());
        auto opened = File::open(path, create_read_write());
        CHECK(opened.has_value());
        File file = std::move(opened).value();

        auto info = sluice::blocking::file_info(file);
        CHECK(info.has_value());
        CHECK(info.value().kind == FileKind::regular);
        CHECK(info.value().size == 0);
        CHECK(info.value().identity.has_value());

        const std::string payload = "w01-clean-room";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        auto wrote = sluice::blocking::write_at(file, 0, bytes);
        CHECK(wrote.has_value() && wrote.value() == payload.size());
        CHECK(sluice::blocking::sync_data(file).has_value());

        std::vector<std::byte> out(payload.size(), std::byte{0});
        auto read = sluice::blocking::read_exact(file, out);
        CHECK(read.has_value() && read.value().complete());
        CHECK(std::string(reinterpret_cast<const char*>(out.data()), out.size()) == payload);

        // Short I/O: positional read spanning EOF returns a short count.
        std::vector<std::byte> past_eof(payload.size() + 16, std::byte{0});
        auto short_read = sluice::blocking::read_at(file, 4, past_eof);
        CHECK(short_read.has_value());
        CHECK(short_read.value() == payload.size() - 4);

        // Zero-length data operation is a logical no-op with success 0.
        std::span<std::byte> empty{};
        auto zero_read = sluice::blocking::read(file, empty);
        CHECK(zero_read.has_value() && zero_read.value() == 0);

        // Resize grow/shrink, then sync covers the size change.
        CHECK(sluice::blocking::resize(file, 64).has_value());
        CHECK(sluice::blocking::size(file).has_value());
        CHECK(sluice::blocking::sync_data(file).has_value());
        CHECK(sluice::blocking::resize(file, 4).has_value());
        CHECK(sluice::blocking::sync_all(file).has_value());
        CHECK(sluice::blocking::size(file).value() == 4);

        // Explicit close: first attempt consumes ownership; closing an already
        // closed file is a successful no-op.
        auto closed = file.close();
        CHECK(closed.has_value());
        CHECK(!file.is_open());
        CHECK(file.close().has_value());

        int fd = ::open(path.c_str(), O_RDONLY);
        CHECK(fd >= 0);
        std::vector<char> content(16, '\0');
        const ssize_t n = ::read(fd, content.data(), content.size());
        ::close(fd);
        CHECK(n == 4);
        CHECK(std::string(content.data(), 4) == "w01-");
        ::unlink(path.c_str());
    }

    // RAII path: destruction releases the descriptor without explicit close.
    {
        const std::string path = make_temp_path();
        int borrowed_fd = -1;
        {
            auto opened = File::open(path, create_read_write());
            CHECK(opened.has_value());
            File file = std::move(opened).value();
            borrowed_fd = file.native_handle();
            const std::string payload = "raii";
            const std::span<const std::byte> bytes(
                reinterpret_cast<const std::byte*>(payload.data()), payload.size());
            CHECK(sluice::blocking::write_all(file, bytes).has_value());
            CHECK(sluice::blocking::sync_data(file).has_value());
        }
        CHECK(::fcntl(borrowed_fd, F_GETFD) < 0);
        ::unlink(path.c_str());
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "w01_direct: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("w01_direct: PASS\n");
    return 0;
}
