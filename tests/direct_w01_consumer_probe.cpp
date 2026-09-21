// W-01 ordinary utility, run as an external-style consumer of the direct
// surface only.
//
// This TU includes nothing but the two canonical direct headers and links only
// `sluice_core` (never `sluice_async`), so it is the build-boundary evidence:
// open, metadata, read/write, positional I/O, composition, resize, durability
// and close must work without a Scheduler, Fiber, ApplicationRuntime,
// AsyncBackend, Completion, RequestArena or liburing. The static assertions fail
// the build if a direct header ever starts pulling an async definition into this
// translation unit.
//
// Two traces run below: an explicit-close trace that reports the close error
// channel, and an RAII trace whose scope exit is the deterministic cleanup.

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
// Declared here, not included: if any direct header had transitively included
// include/sluice/async/**, these types would be complete in this TU.
class AsyncBackend;
class AsyncIoContext;
class ApplicationRuntime;
class Fiber;
class Scheduler;
namespace detail {
class RequestArena;
}
}

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileKind;
using sluice::FileOpen;
using sluice::IdentityMatch;

template <class T, class = void> struct is_complete : std::false_type {};
template <class T> struct is_complete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

static_assert(!is_complete<sluice::async::AsyncIoContext>::value,
              "the direct surface must not pull the async context into a consumer");
static_assert(!is_complete<sluice::async::AsyncBackend>::value,
              "the direct surface must not pull a backend into a consumer");
static_assert(!is_complete<sluice::async::Scheduler>::value,
              "the direct surface must not pull a Scheduler into a consumer");
static_assert(!is_complete<sluice::async::Fiber>::value,
              "the direct surface must not pull a Fiber into a consumer");
static_assert(!is_complete<sluice::async::ApplicationRuntime>::value,
              "the direct surface must not pull the runtime into a consumer");
static_assert(!is_complete<sluice::async::detail::RequestArena>::value,
              "the direct surface must not pull the request arena into a consumer");

std::string make_temp_path() {
    char path[] = "/tmp/sluice_w01_consumer_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    ::close(fd);
    return path;
}

FileOpen create_read_write() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::create_if_missing;
    return mode;
}

bool file_content_is(const std::string& path, const std::string& expected) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    std::vector<char> buffer(expected.size() + 1, '\0');
    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
    ::close(fd);
    return n == static_cast<ssize_t>(expected.size()) &&
           std::string(buffer.data(), expected.size()) == expected;
}

// W-01: open, inspect metadata, write and read, positionally and through the
// shared cursor, resize, synchronize, and report close errors explicitly.
bool w01_explicit_close_trace() {
    const std::string path = make_temp_path();
    if (path.empty())
        return false;

    auto opened = File::open(path, create_read_write());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    bool ok = true;

    // metadata
    auto info = sluice::blocking::file_info(file);
    ok = ok && info.has_value() && info.value().kind == FileKind::regular;
    ok = ok && info.value().size == 0 && info.value().identity.has_value();

    // positional write, then a positional composition that appends to it
    const std::string header = "header:";
    const std::string body = "body";
    const std::span<const std::byte> header_bytes(
        reinterpret_cast<const std::byte*>(header.data()), header.size());
    const std::span<const std::byte> body_bytes(
        reinterpret_cast<const std::byte*>(body.data()), body.size());
    auto wrote_at = sluice::blocking::write_at(file, 0, header_bytes);
    auto wrote_all = sluice::blocking::write_all_at(file, header.size(), body_bytes);
    ok = ok && wrote_at.has_value() && wrote_at.value() == header.size();
    ok = ok && wrote_all.has_value() && wrote_all.value().complete();

    // durability of the confirmed bytes
    ok = ok && sluice::blocking::sync_data(file).has_value();

    // shared-cursor exact read of the whole payload, then a shared-cursor write
    std::vector<std::byte> whole(header.size() + body.size(), std::byte{0});
    auto read_all = sluice::blocking::read_exact(file, whole);
    ok = ok && read_all.has_value() && read_all.value().complete();
    ok = ok && std::string(reinterpret_cast<const char*>(whole.data()), whole.size()) ==
                   header + body;
    const std::string bang = "!";
    const std::span<const std::byte> bang_bytes(
        reinterpret_cast<const std::byte*>(bang.data()), bang.size());
    auto appended = sluice::blocking::write(file, bang_bytes);
    ok = ok && appended.has_value() && appended.value() == bang.size();

    // a positional composition reads the same bytes back without the cursor
    std::vector<std::byte> all(header.size() + body.size() + bang.size(), std::byte{0});
    auto read_again = sluice::blocking::read_exact_at(file, 0, all);
    ok = ok && read_again.has_value() && read_again.value().complete();
    ok = ok && std::string(reinterpret_cast<const char*>(all.data()), all.size()) ==
                   header + body + bang;

    // resize grow, synchronize the size change, resize shrink, synchronize again
    auto grown = sluice::blocking::resize(file, 64);
    ok = ok && grown.has_value() && sluice::blocking::sync_data(file).has_value();
    auto grown_size = sluice::blocking::size(file);
    ok = ok && grown_size.has_value() && grown_size.value() == 64;
    auto shrunk = sluice::blocking::resize(file, 4);
    ok = ok && shrunk.has_value() && sluice::blocking::sync_all(file).has_value();
    auto shrunk_size = sluice::blocking::size(file);
    ok = ok && shrunk_size.has_value() && shrunk_size.value() == 4;

    // the explicit close is the observable close-error channel
    auto closed = file.close();
    ok = ok && closed.has_value() && !file.is_open();
    ok = ok && file_content_is(path, "head");
    ::unlink(path.c_str());
    return ok;
}

// W-01's RAII half: scope exit releases the resource deterministically, with no
// runtime, context or registry involved.
bool w01_raii_trace() {
    const std::string path = make_temp_path();
    if (path.empty())
        return false;

    int fd = -1;
    {
        auto opened = File::open(path, create_read_write());
        if (!opened.has_value())
            return false;
        File file = std::move(opened).value();
        fd = file.native_handle();

        const std::string payload = "raii";
        const std::span<const std::byte> bytes(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        if (!sluice::blocking::write_all(file, bytes).has_value())
            return false;
        if (!sluice::blocking::sync_data(file).has_value())
            return false;
    } // scope exit: one best-effort native close

    const bool released = ::fcntl(fd, F_GETFD) < 0;
    const bool content_ok = file_content_is(path, "raii");
    ::unlink(path.c_str());
    return released && content_ok;
}

// The same-file comparison a consumer would use to notice two paths are one
// file, with the unknown outcome when identity is unavailable.
bool same_file_comparison_is_usable() {
    const std::string path = make_temp_path();
    if (path.empty())
        return false;
    auto first_opened = File::open(path, create_read_write());
    auto second_opened = File::open(path, create_read_write());
    if (!first_opened.has_value() || !second_opened.has_value())
        return false;
    File first = std::move(first_opened).value();
    File second = std::move(second_opened).value();

    auto first_info = sluice::blocking::file_info(first);
    auto second_info = sluice::blocking::file_info(second);
    bool ok = first_info.has_value() && second_info.has_value();
    ok = ok && sluice::identity_match(first_info.value(), second_info.value()) ==
                   IdentityMatch::same;
    ok = ok && sluice::identity_match(first_info.value(), sluice::FileInfo{}) ==
                   IdentityMatch::unknown;
    ok = ok && first.close().has_value() && second.close().has_value();
    ::unlink(path.c_str());
    return ok;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"w01_explicit_close_trace", w01_explicit_close_trace},
        {"w01_raii_trace", w01_raii_trace},
        {"same_file_comparison_is_usable", same_file_comparison_is_usable},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu direct W-01 consumer-probe tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
