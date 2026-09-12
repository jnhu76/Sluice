#include <sluice/blocking/file.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::Result;
using sluice::blocking::read;
using sluice::blocking::read_at;
using sluice::blocking::write;
using sluice::blocking::write_at;

FileOpen writable_mode() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    return mode;
}

FileOpen write_only_mode() {
    FileOpen mode;
    mode.access = FileAccess::write_only;
    return mode;
}

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_blocking_file_sequential_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    if (!content.empty()) {
        const ssize_t n = ::write(fd, content.data(), content.size());
        if (n != static_cast<ssize_t>(content.size())) {
            ::close(fd);
            return {};
        }
    }
    ::close(fd);
    return path;
}

bool file_content_is(const std::string& path, const std::string& expected) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    std::vector<char> buf(expected.size() + 1, '\0');
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    ::close(fd);
    if (n != static_cast<ssize_t>(expected.size()))
        return false;
    return std::memcmp(buf.data(), expected.data(), expected.size()) == 0;
}

std::span<const std::byte> as_bytes(const std::string& src) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(src.data()), src.size());
}

bool sequential_write_then_read_roundtrip() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    const std::string src_str = "hello";
    auto wr = write(file, as_bytes(src_str));
    if (!wr.has_value() || wr.value() != src_str.size()) {
        ::unlink(path.c_str());
        return false;
    }

    // The write advanced the shared offset to 5, so a sequential read is EOF.
    std::vector<std::byte> eof_dst(5);
    auto rd = read(file, eof_dst);
    if (!rd.has_value() || rd.value() != 0) {
        ::unlink(path.c_str());
        return false;
    }

    // The written bytes are recoverable positionally at offset 0.
    std::vector<std::byte> dst(5);
    auto back = read_at(file, 0, dst);

    const bool content_ok = file_content_is(path, "hello");
    ::unlink(path.c_str());

    if (!back.has_value())
        return false;
    if (back.value() != 5)
        return false;
    if (std::memcmp(dst.data(), "hello", 5) != 0)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool sequential_read_advances_position() {
    const std::string path = make_temp_file("abcdef");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    std::vector<std::byte> dst(2);
    for (const char* expected : {"ab", "cd", "ef"}) {
        auto result = read(file, dst);
        if (!result.has_value())
            return false;
        if (result.value() != 2)
            return false;
        if (std::memcmp(dst.data(), expected, 2) != 0)
            return false;
    }

    std::vector<std::byte> eof_dst(1);
    auto eof = read(file, eof_dst);
    if (!eof.has_value())
        return false;
    if (eof.value() != 0)
        return false;
    return file.close().has_value();
}

bool sequential_read_at_eof_returns_success_zero() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    std::vector<std::byte> dst(4);
    auto result = read(file, dst);

    if (!result.has_value())
        return false;
    if (result.value() != 0)
        return false;
    return file.close().has_value();
}

bool sequential_zero_length_read_succeeds_without_progress() {
    const std::string path = make_temp_file("xy");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    auto empty = read(file, std::span<std::byte>{});
    if (!empty.has_value())
        return false;
    if (empty.value() != 0)
        return false;

    std::vector<std::byte> dst(2);
    auto result = read(file, dst);
    if (!result.has_value())
        return false;
    if (result.value() != 2)
        return false;
    if (std::memcmp(dst.data(), "xy", 2) != 0)
        return false;
    return file.close().has_value();
}

bool sequential_zero_length_write_succeeds() {
    const std::string path = make_temp_file("ab");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto empty = write(file, std::span<const std::byte>{});
    if (!empty.has_value())
        return false;
    if (empty.value() != 0)
        return false;

    std::vector<std::byte> dst(2);
    auto result = read(file, dst);

    const bool content_ok = file_content_is(path, "ab");
    ::unlink(path.c_str());

    if (!result.has_value())
        return false;
    if (result.value() != 2)
        return false;
    if (std::memcmp(dst.data(), "ab", 2) != 0)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool sequential_write_advances_position() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    const std::string first = "abc";
    const std::string second = "def";
    auto w1 = write(file, as_bytes(first));
    if (!w1.has_value() || w1.value() != first.size()) {
        ::unlink(path.c_str());
        return false;
    }
    auto w2 = write(file, as_bytes(second));
    if (!w2.has_value() || w2.value() != second.size()) {
        ::unlink(path.c_str());
        return false;
    }

    std::vector<std::byte> six(6);
    auto positional = read_at(file, 0, six);
    if (!positional.has_value() || positional.value() != 6) {
        ::unlink(path.c_str());
        return false;
    }
    if (std::memcmp(six.data(), "abcdef", 6) != 0) {
        ::unlink(path.c_str());
        return false;
    }

    const std::string third = "g";
    auto w3 = write(file, as_bytes(third));
    if (!w3.has_value() || w3.value() != 1) {
        ::unlink(path.c_str());
        return false;
    }

    std::vector<std::byte> one(1);
    auto at_six = read_at(file, 6, one);
    const bool content_ok = file_content_is(path, "abcdefg");
    ::unlink(path.c_str());

    if (!at_six.has_value() || at_six.value() != 1)
        return false;
    if (std::memcmp(one.data(), "g", 1) != 0)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool positional_operations_do_not_move_shared_position() {
    // A positional write must not advance the shared offset: after write_at(100)
    // on an empty file, a sequential write has to land at offset 0. A positional
    // read must not move it either: the read_at probe below observes offset 0,
    // and the sequential write right after it still has to land at offset 3.
    {
        const std::string path = make_temp_file("");
        if (path.empty())
            return false;
        auto opened = File::open(path, writable_mode());
        if (!opened.has_value())
            return false;
        File file = std::move(opened).value();

        const std::string far_src = "XYZ";
        auto grown = write_at(file, 100, as_bytes(far_src));
        if (!grown.has_value() || grown.value() != 3) {
            ::unlink(path.c_str());
            return false;
        }

        const std::string near_src = "abc";
        auto near_wr = write(file, as_bytes(near_src));
        if (!near_wr.has_value() || near_wr.value() != 3) {
            ::unlink(path.c_str());
            return false;
        }

        std::vector<std::byte> probe(1);
        auto probe_rd = read_at(file, 0, probe);
        if (!probe_rd.has_value() || probe_rd.value() != 1) {
            ::unlink(path.c_str());
            return false;
        }
        if (std::memcmp(probe.data(), "a", 1) != 0) {
            ::unlink(path.c_str());
            return false;
        }

        const std::string after_probe = "Q";
        auto q_wr = write(file, as_bytes(after_probe));
        if (!q_wr.has_value() || q_wr.value() != 1) {
            ::unlink(path.c_str());
            return false;
        }

        std::vector<std::byte> head(6);
        std::vector<std::byte> q_dst(1);
        std::vector<std::byte> beyond_dst(1);
        std::vector<std::byte> far_dst(3);
        auto at_q = read_at(file, 3, q_dst);
        auto at_beyond = read_at(file, 4, beyond_dst);
        auto at_head = read_at(file, 0, head);
        auto at_far = read_at(file, 100, far_dst);
        const bool closed_ok = file.close().has_value();
        ::unlink(path.c_str());

        if (!at_head.has_value() || at_head.value() != 6)
            return false;
        if (std::memcmp(head.data(), "abcQ\0\0", 6) != 0)
            return false;
        if (!at_q.has_value() || at_q.value() != 1)
            return false;
        if (std::memcmp(q_dst.data(), "Q", 1) != 0)
            return false;
        if (!at_beyond.has_value() || at_beyond.value() != 1)
            return false;
        if (beyond_dst[0] != std::byte{0})
            return false;
        if (!at_far.has_value() || at_far.value() != 3)
            return false;
        if (std::memcmp(far_dst.data(), "XYZ", 3) != 0)
            return false;
        if (!closed_ok)
            return false;
    }

    // A positional write must not move the shared offset either: after writing
    // "abcdef" sequentially (position 6), write_at(0) leaves the position at 6,
    // so the next sequential read is EOF at the 6-byte end of file.
    {
        const std::string path = make_temp_file("");
        if (path.empty())
            return false;
        auto opened = File::open(path, writable_mode());
        if (!opened.has_value())
            return false;
        File file = std::move(opened).value();

        const std::string body = "abcdef";
        auto w = write(file, as_bytes(body));
        if (!w.has_value() || w.value() != body.size()) {
            ::unlink(path.c_str());
            return false;
        }

        const std::string head_src = "ZZZ";
        auto overwritten = write_at(file, 0, as_bytes(head_src));
        if (!overwritten.has_value() || overwritten.value() != 3) {
            ::unlink(path.c_str());
            return false;
        }

        std::vector<std::byte> dst(3);
        auto eof = read(file, dst);
        if (!eof.has_value())
            return false;
        if (eof.value() != 0)
            return false;

        std::vector<std::byte> six(6);
        auto at_head = read_at(file, 0, six);
        const bool closed_ok = file.close().has_value();
        ::unlink(path.c_str());

        if (!at_head.has_value() || at_head.value() != 6)
            return false;
        if (std::memcmp(six.data(), "ZZZdef", 6) != 0)
            return false;
        if (!closed_ok)
            return false;
    }
    return true;
}

bool sequential_read_on_write_only_file_rejected_upfront() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path, write_only_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    std::vector<std::byte> dst(3);
    auto result = read(file, dst);

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_argument)
        return false;

    auto empty_result = read(file, std::span<std::byte>{});
    if (empty_result.has_value())
        return false;
    if (empty_result.error().code != IoError::Code::invalid_argument)
        return false;
    return file.close().has_value();
}

bool sequential_zero_length_read_after_close_reports_invalid_state() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    if (!file.close().has_value())
        return false;

    auto result = read(file, std::span<std::byte>{});

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_state)
        return false;
    return true;
}

bool sequential_zero_length_write_on_read_only_reports_invalid_argument() {
    const std::string path = make_temp_file("keep");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    auto result = write(file, std::span<const std::byte>{});

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_argument)
        return false;
    return true;
}

bool sequential_write_on_read_only_reports_invalid_argument() {
    const std::string path = make_temp_file("keep");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    const std::string src_str = "X";
    auto result = write(file, as_bytes(src_str));

    const bool content_ok = file_content_is(path, "keep");
    ::unlink(path.c_str());

    if (result.has_value())
        return false;
    if (result.error().code != IoError::Code::invalid_argument)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

bool sequential_after_close_reports_invalid_state() {
    const std::string path = make_temp_file("abcd");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    ::unlink(path.c_str());

    if (!file.close().has_value())
        return false;

    std::vector<std::byte> dst(4);
    auto rd = read(file, dst);
    if (rd.has_value())
        return false;
    if (rd.error().code != IoError::Code::invalid_state)
        return false;

    const std::string src_str = "X";
    auto wr = write(file, as_bytes(src_str));
    if (wr.has_value())
        return false;
    if (wr.error().code != IoError::Code::invalid_state)
        return false;
    return true;
}

bool concurrent_sequential_reads_partition_content() {
    std::string content(100, '\0');
    for (std::size_t i = 0; i < content.size(); ++i)
        content[i] = static_cast<char>(i);

    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    auto opened = File::open(path, writable_mode());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();

    auto seeded = write_at(file, 0, as_bytes(content));
    if (!seeded.has_value() || seeded.value() != content.size()) {
        ::unlink(path.c_str());
        return false;
    }

    auto consume = [&file](bool* ok, std::vector<std::byte>* collected) {
        *ok = true;
        std::vector<std::byte> chunk(7);
        for (;;) {
            auto r = read(file, chunk);
            if (!r.has_value()) {
                *ok = false;
                return;
            }
            if (r.value() == 0)
                return;
            collected->insert(collected->end(), chunk.begin(),
                              chunk.begin() + static_cast<std::ptrdiff_t>(r.value()));
        }
    };

    bool ok1 = false;
    bool ok2 = false;
    std::vector<std::byte> got1;
    std::vector<std::byte> got2;
    std::thread t1(consume, &ok1, &got1);
    std::thread t2(consume, &ok2, &got2);
    t1.join();
    t2.join();

    std::vector<std::byte> merged = got1;
    merged.insert(merged.end(), got2.begin(), got2.end());

    std::size_t seen[256] = {};
    for (const std::byte b : merged)
        ++seen[std::to_integer<unsigned int>(b)];

    bool multiset_ok = ok1 && ok2;
    for (unsigned int v = 0; v < 100; ++v)
        if (seen[v] != 1)
            multiset_ok = false;
    for (unsigned int v = 100; v < 256; ++v)
        if (seen[v] != 0)
            multiset_ok = false;

    const bool content_ok = file_content_is(path, content);
    ::unlink(path.c_str());

    if (got1.size() + got2.size() != content.size())
        return false;
    if (!multiset_ok)
        return false;
    if (!content_ok)
        return false;
    return file.close().has_value();
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"sequential_write_then_read_roundtrip", sequential_write_then_read_roundtrip},
        {"sequential_read_advances_position", sequential_read_advances_position},
        {"sequential_read_at_eof_returns_success_zero", sequential_read_at_eof_returns_success_zero},
        {"sequential_zero_length_read_succeeds_without_progress",
         sequential_zero_length_read_succeeds_without_progress},
        {"sequential_zero_length_write_succeeds", sequential_zero_length_write_succeeds},
        {"sequential_write_advances_position", sequential_write_advances_position},
        {"positional_operations_do_not_move_shared_position",
         positional_operations_do_not_move_shared_position},
        {"sequential_read_on_write_only_file_rejected_upfront",
         sequential_read_on_write_only_file_rejected_upfront},
        {"sequential_zero_length_read_after_close_reports_invalid_state",
         sequential_zero_length_read_after_close_reports_invalid_state},
        {"sequential_zero_length_write_on_read_only_reports_invalid_argument",
         sequential_zero_length_write_on_read_only_reports_invalid_argument},
        {"sequential_write_on_read_only_reports_invalid_argument",
         sequential_write_on_read_only_reports_invalid_argument},
        {"sequential_after_close_reports_invalid_state",
         sequential_after_close_reports_invalid_state},
        {"concurrent_sequential_reads_partition_content",
         concurrent_sequential_reads_partition_content},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu blocking file sequential tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
