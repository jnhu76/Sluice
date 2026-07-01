// Tests for FileReader / FileWriter: POSIX-backed blocking I/O with RAII.
// Uses temp files under the system temp dir; cleaned up per test.
#include "harness.hpp"

#include <cppio/file.hpp>
#include <cppio/copy.hpp>
#include <cppio/wal.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace {

// Makes a unique temp path (not yet created) and cleans it up on scope exit.
struct TempPath {
    std::filesystem::path p;
    TempPath() {
        auto dir = std::filesystem::temp_directory_path();
        p = dir / ("cppio_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                   ".tmp");
    }
    ~TempPath() { std::filesystem::remove(p); }
    std::string str() const { return p.string(); }
};

std::span<const std::byte> sb(std::string_view s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}
bool file_contains(const std::string& path, std::string_view expected) {
    std::ifstream in(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    return content == std::string(expected);
}

}  // namespace

CPPIO_TEST_CASE(file_writer_creates_and_appends_bytes) {
    TempPath tp;
    {
        cppio::FileWriter w(tp.str());
        CPPIO_CHECK(w.opened());
        CPPIO_CHECK(w.write_all(sb("hello ")).has_value());
        CPPIO_CHECK(w.write_all(sb("world")).has_value());
        CPPIO_CHECK(w.flush().has_value());  // user-space no-op for now
    }  // RAII closes
    CPPIO_CHECK(file_contains(tp.str(), "hello world"));
}

CPPIO_TEST_CASE(file_reader_reads_back_written_bytes) {
    TempPath tp;
    {
        cppio::FileWriter w(tp.str());
        CPPIO_CHECK(w.write_all(sb("file payload")).has_value());
    }
    cppio::FileReader r(tp.str());
    CPPIO_CHECK(r.opened());
    std::vector<std::byte> out(12);
    auto res = r.read_exact(std::span<std::byte>(out));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(std::memcmp(out.data(), "file payload", 12) == 0);
}

CPPIO_TEST_CASE(file_reader_returns_zero_at_eof) {
    TempPath tp;
    {
        cppio::FileWriter w(tp.str());
        CPPIO_CHECK(w.write_all(sb("ab")).has_value());
    }
    cppio::FileReader r(tp.str());
    std::array<std::byte, 2> a{};
    (void)r.read_some(std::span<std::byte>(a));
    std::array<std::byte, 2> b{};
    auto res = r.read_some(std::span<std::byte>(b));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 0);
}

CPPIO_TEST_CASE(file_reader_open_missing_returns_error) {
    cppio::FileReader r("/no/such/file/should/exist/cppio");
    CPPIO_CHECK(!r.opened());
    // read_some on a failed-open file surfaces an error, not success.
    std::array<std::byte, 4> buf{};
    auto res = r.read_some(std::span<std::byte>(buf));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::permission_denied);
}

CPPIO_TEST_CASE(file_copy_all_round_trips_through_real_files) {
    TempPath src_tp, dst_tp;
    {
        cppio::FileWriter w(src_tp.str());
        std::string big(2048, 'z');
        CPPIO_CHECK(w.write_all(sb(big)).has_value());
    }
    {
        cppio::FileReader r(src_tp.str());
        cppio::FileWriter w(dst_tp.str());
        auto res = cppio::copy_all(r, w);
        CPPIO_CHECK(res.has_value());
        CPPIO_CHECK(res.value() == 2048);
    }
    CPPIO_CHECK(file_contains(dst_tp.str(), std::string(2048, 'z')));
}

CPPIO_TEST_CASE(file_wal_records_round_trip_on_disk) {
    TempPath tp;
    {
        cppio::FileWriter w(tp.str());
        CPPIO_CHECK(cppio::wal::write_record(w, sb("rec-one")).has_value());
        CPPIO_CHECK(cppio::wal::write_record(w, sb("rec-two")).has_value());
    }
    cppio::FileReader r(tp.str());
    auto r1 = cppio::wal::read_record(r);
    auto r2 = cppio::wal::read_record(r);
    CPPIO_CHECK(r1.has_value());
    CPPIO_CHECK(r2.has_value());
    CPPIO_CHECK(std::memcmp(r1.value().data(), "rec-one", 7) == 0);
    CPPIO_CHECK(std::memcmp(r2.value().data(), "rec-two", 7) == 0);
}

CPPIO_TEST_CASE(file_writer_is_move_only) {
    TempPath tp;
    cppio::FileWriter w(tp.str());
    CPPIO_CHECK(w.opened());
    cppio::FileWriter moved = std::move(w);
    CPPIO_CHECK(moved.opened());
    CPPIO_CHECK(!w.opened());  // moved-from is released
    CPPIO_CHECK(moved.write_all(sb("moved")).has_value());
}

CPPIO_MAIN()
