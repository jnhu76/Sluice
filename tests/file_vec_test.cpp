// Tests for FileReader::read_vec / FileWriter::write_vec — the POSIX readv/writev
// overrides. Uses temp files. Covers round-trip correctness, IOV_MAX chunking,
// empty-only vectors (no syscall), and open-error errno preservation.
#include "harness.hpp"

#include <cppio/file.hpp>
#include <cppio/iovec.hpp>
#include <cppio/measurement.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath() {
        auto dir = std::filesystem::temp_directory_path();
        p = dir / ("cppio_vec_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                   ".tmp");
    }
    ~TempPath() { std::filesystem::remove(p); }
    std::string str() const { return p.string(); }
};

cppio::ConstIoSlice cslice_of(std::string_view s) {
    return cppio::ConstIoSlice{std::as_bytes(std::span(s.data(), s.size()))};
}
cppio::IoSlice mslice_of(std::span<std::byte> b) { return cppio::IoSlice{b}; }

bool file_equals(const std::string& path, std::string_view expected) {
    std::ifstream in(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    return content == std::string(expected);
}

}  // namespace

CPPIO_TEST_CASE(file_write_vec_writes_correct_bytes_to_disk) {
    TempPath tp;
    {
        cppio::FileWriter w(tp.str());
        CPPIO_CHECK(w.opened());
        std::array<cppio::ConstIoSlice, 2> srcs = {cslice_of("hello "), cslice_of("world")};
        auto r = w.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
        CPPIO_CHECK(r.has_value());
        CPPIO_CHECK(r.value() == 11);
    }
    CPPIO_CHECK(file_equals(tp.str(), "hello world"));
}

CPPIO_TEST_CASE(file_read_vec_reads_correct_bytes_from_disk) {
    TempPath tp;
    {
        cppio::FileWriter w(tp.str());
        CPPIO_CHECK(w.write_all(std::as_bytes(std::span("file payload", 12))).has_value());
    }
    cppio::FileReader r(tp.str());
    CPPIO_CHECK(r.opened());
    std::vector<std::byte> a(5), b(7);
    std::array<cppio::IoSlice, 2> dsts = {mslice_of(a), mslice_of(b)};
    auto res = r.read_vec(std::span<cppio::IoSlice>(dsts));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 12);
    CPPIO_CHECK(std::memcmp(a.data(), "file ", 5) == 0);
    CPPIO_CHECK(std::memcmp(b.data(), "payload", 7) == 0);
}

CPPIO_TEST_CASE(file_write_vec_then_read_vec_round_trips_many_slices) {
    TempPath tp;
    {
        cppio::FileWriter w(tp.str());
        std::array<cppio::ConstIoSlice, 4> srcs = {
            cslice_of("aaaa"), cslice_of("bbbb"), cslice_of("cccc"), cslice_of("dddd")};
        auto r = w.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
        CPPIO_CHECK(r.has_value());
        CPPIO_CHECK(r.value() == 16);
    }
    cppio::FileReader r(tp.str());
    std::vector<std::byte> out(16);
    std::array<cppio::IoSlice, 4> dsts = {
        mslice_of(std::span<std::byte>(out).subspan(0, 4)),
        mslice_of(std::span<std::byte>(out).subspan(4, 4)),
        mslice_of(std::span<std::byte>(out).subspan(8, 4)),
        mslice_of(std::span<std::byte>(out).subspan(12, 4)),
    };
    auto res = r.read_vec(std::span<cppio::IoSlice>(dsts));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 16);
    CPPIO_CHECK(std::memcmp(out.data(), "aaaabbbbccccdddd", 16) == 0);
}

CPPIO_TEST_CASE(file_vec_handles_more_slices_than_iov_max) {
    // A slice count exceeding IOV_MAX must still round-trip: the override chunks
    // the readv/writev calls. Many tiny slices force multiple chunks.
    TempPath tp;
    constexpr std::size_t kN = 4096;  // well above IOV_MAX (1024 on Linux)
    std::vector<std::string> pieces;
    std::string expected;
    expected.reserve(kN * 3);
    for (std::size_t i = 0; i < kN; ++i) {
        std::string s = "x" + std::to_string(i % 10) + "y";  // 3 bytes each
        pieces.push_back(s);
        expected += s;
    }
    std::vector<cppio::ConstIoSlice> srcs;
    srcs.reserve(kN);
    for (auto& s : pieces) srcs.push_back(cslice_of(s));
    {
        cppio::FileWriter w(tp.str());
        auto r = w.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
        CPPIO_CHECK(r.has_value());
        CPPIO_CHECK(r.value() == expected.size());
    }
    cppio::FileReader r(tp.str());
    std::vector<std::byte> out(expected.size());
    std::vector<cppio::IoSlice> dsts;
    // Read back in 256-byte chunks -> far fewer slices than written, still > 1.
    for (std::size_t off = 0; off < out.size(); off += 256) {
        std::size_t n = std::min<std::size_t>(256, out.size() - off);
        dsts.push_back(mslice_of(std::span<std::byte>(out).subspan(off, n)));
    }
    auto res = r.read_vec(std::span<cppio::IoSlice>(dsts));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == expected.size());
    CPPIO_CHECK(std::memcmp(out.data(), expected.data(), expected.size()) == 0);
}

CPPIO_TEST_CASE(file_vec_empty_only_vector_returns_zero_no_syscall) {
    TempPath tp;
    {
        cppio::FileWriter w(tp.str());
        cppio::ConstIoSlice empty{std::span<const std::byte>{}};
        std::array<cppio::ConstIoSlice, 2> srcs = {empty, empty};
        auto r = w.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
        CPPIO_CHECK(r.has_value());
        CPPIO_CHECK(r.value() == 0);
    }
    // File should be empty (no bytes written).
    CPPIO_CHECK(file_equals(tp.str(), ""));

    cppio::FileReader r(tp.str());
    cppio::IoSlice empty{std::span<std::byte>{}};
    std::array<cppio::IoSlice, 2> dsts = {empty, empty};
    auto res = r.read_vec(std::span<cppio::IoSlice>(dsts));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 0);
}

CPPIO_TEST_CASE(file_vec_open_error_preserves_errno) {
    // read_vec / write_vec on a failed-open file must surface the real errno,
    // matching read_some / write_some behavior.
    cppio::FileReader r("/no/such/cppio/vec/path");
    CPPIO_CHECK(!r.opened());
    std::array<std::byte, 4> buf{};
    cppio::IoSlice dst{buf};
    auto res = r.read_vec(std::span<cppio::IoSlice>(&dst, 1));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::permission_denied);
    CPPIO_CHECK(res.error().os_errno == ENOENT);
}

// ---------- VectorStats on the POSIX file backends ----------

CPPIO_TEST_CASE(file_vec_stats_count_non_fallback_path) {
    TempPath tp;
    cppio::VectorStats vs{};
    {
        cppio::FileWriter w(tp.str(), nullptr, &vs);
        std::array<cppio::ConstIoSlice, 3> srcs = {
            cslice_of("ab"), cslice_of(""), cslice_of("cde")};
        auto r = w.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
        CPPIO_CHECK(r.has_value());
        CPPIO_CHECK(r.value() == 5);
    }
    CPPIO_CHECK(vs.write_vec_calls == 1);
    CPPIO_CHECK(vs.write_vec_bytes == 5);
    CPPIO_CHECK(vs.write_vec_iovecs == 2);       // empty slice not counted
    CPPIO_CHECK(vs.write_vec_fallback_calls == 0);  // real writev, not fallback

    cppio::VectorStats vrs{};
    cppio::FileReader r(tp.str(), nullptr, &vrs);
    std::vector<std::byte> a(3), b(2);
    std::array<cppio::IoSlice, 2> dsts = {mslice_of(a), mslice_of(b)};
    auto res = r.read_vec(std::span<cppio::IoSlice>(dsts));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 5);
    CPPIO_CHECK(vrs.read_vec_calls == 1);
    CPPIO_CHECK(vrs.read_vec_bytes == 5);
    CPPIO_CHECK(vrs.read_vec_iovecs == 2);
    CPPIO_CHECK(vrs.read_vec_fallback_calls == 0);  // real readv, not fallback
}

CPPIO_TEST_CASE(file_vec_short_read_returns_partial_byte_count) {
    // File has 3 bytes but the dst vector requests 8 (4+4): readv short-reads,
    // returning only the 3 bytes available. The POSIX override must report the
    // partial count and leave the rest of the dst buffers untouched.
    TempPath tp;
    {
        cppio::FileWriter w(tp.str());
        CPPIO_CHECK(w.write_all(std::as_bytes(std::span("abc", 3))).has_value());
    }
    cppio::FileReader r(tp.str());
    std::vector<std::byte> a(4, std::byte{0xFF}), b(4, std::byte{0xFF});
    std::array<cppio::IoSlice, 2> dsts = {mslice_of(a), mslice_of(b)};
    auto res = r.read_vec(std::span<cppio::IoSlice>(dsts));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 3);  // partial: only what the file held
    CPPIO_CHECK(std::memcmp(a.data(), "abc", 3) == 0);
    // slice a bytes [3] and all of slice b must be untouched (0xFF).
    CPPIO_CHECK(std::to_integer<int>(a[3]) == 0xFF);
    for (auto x : b) CPPIO_CHECK(std::to_integer<int>(x) == 0xFF);
}

CPPIO_MAIN()
