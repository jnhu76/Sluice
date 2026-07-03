// Tests for FileReader / FileWriter: POSIX-backed blocking I/O with RAII.
// Uses temp files under the system temp dir; cleaned up per test.
#include "harness.hpp"

#include <cppio/file.hpp>
#include <cppio/copy.hpp>
#include <cppio/measurement.hpp>
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
    ~TempPath() {
        try { std::filesystem::remove(p); } catch (...) {}
    }
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

CPPIO_TEST_CASE(file_reader_open_missing_preserves_enoent) {
    cppio::FileReader r("/no/such/file/should/exist/cppio");
    CPPIO_CHECK(!r.opened());
    // read_some on a failed-open file surfaces the real errno (ENOENT), not a
    // synthetic code.
    std::array<std::byte, 4> buf{};
    auto res = r.read_some(std::span<std::byte>(buf));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().code == cppio::IoError::Code::permission_denied);
    CPPIO_CHECK(res.error().os_errno == ENOENT);
}

CPPIO_TEST_CASE(file_reader_open_under_a_file_preserves_enotdir) {
    // ENOTDIR: opening "<existing regular file>/child" must fail with ENOTDIR,
    // and the real errno must survive to read_some's error.
    TempPath tp;
    { cppio::FileWriter w(tp.str()); (void)w.write_all(sb("x")); }
    cppio::FileReader r(tp.str() + "/child");
    CPPIO_CHECK(!r.opened());
    std::array<std::byte, 4> buf{};
    auto res = r.read_some(std::span<std::byte>(buf));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().os_errno == ENOTDIR);
}

CPPIO_TEST_CASE(file_writer_open_failure_preserves_real_errno) {
    // Try to write-create under a path whose parent is a regular file -> ENOTDIR.
    TempPath tp;
    { cppio::FileWriter w(tp.str()); (void)w.write_all(sb("x")); }
    cppio::FileWriter w(tp.str() + "/child");
    CPPIO_CHECK(!w.opened());
    auto res = w.write_some(sb("data"));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(res.error().os_errno == ENOTDIR);
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

CPPIO_TEST_CASE(file_reader_is_move_only) {
    // Parity with the FileWriter move test: verifies the moved-from reader
    // releases its fd (no double-close) and the moved-to reader reads correctly.
    TempPath tp;
    {
        cppio::FileWriter w(tp.str());
        CPPIO_CHECK(w.write_all(sb("moved")).has_value());
    }
    cppio::FileReader r(tp.str());
    CPPIO_CHECK(r.opened());
    cppio::FileReader moved = std::move(r);
    CPPIO_CHECK(moved.opened());
    CPPIO_CHECK(!r.opened());  // moved-from is released
    std::array<std::byte, 5> buf{};
    auto res = moved.read_exact(std::span<std::byte>(buf));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(std::memcmp(buf.data(), "moved", 5) == 0);
}

// ---------- SyscallStats attachment (CPPIO-CORE-004) ----------

CPPIO_TEST_CASE(file_writer_records_syscall_stats) {
    TempPath tp;
    cppio::SyscallStats stats{};
    {
        cppio::FileWriter w(tp.str(), &stats);
        CPPIO_CHECK(w.write_all(sb("hello")).has_value());
    }
    CPPIO_CHECK(stats.write_syscalls >= 1);
    CPPIO_CHECK(stats.write_syscall_bytes == 5);
    CPPIO_CHECK(stats.write_syscall_errors == 0);
}

CPPIO_TEST_CASE(file_reader_records_syscall_stats) {
    TempPath tp;
    {
        cppio::FileWriter w(tp.str());
        CPPIO_CHECK(w.write_all(sb("hello")).has_value());
    }
    cppio::SyscallStats stats{};
    cppio::FileReader r(tp.str(), &stats);
    std::array<std::byte, 5> buf{};
    CPPIO_CHECK(r.read_exact(std::span<std::byte>(buf)).has_value());

    CPPIO_CHECK(stats.read_syscalls >= 1);
    CPPIO_CHECK(stats.read_syscall_bytes == 5);
    CPPIO_CHECK(stats.read_syscall_errors == 0);
}

CPPIO_TEST_CASE(file_stats_null_is_zero_overhead_and_semantics_unchanged) {
    // Null stats must not crash and must not change behavior.
    TempPath tp;
    {
        cppio::FileWriter w(tp.str(), nullptr);
        CPPIO_CHECK(w.write_all(sb("x")).has_value());
    }
    cppio::FileReader r(tp.str());  // default: no stats
    std::array<std::byte, 1> buf{};
    CPPIO_CHECK(r.read_exact(std::span<std::byte>(buf)).has_value());
    CPPIO_CHECK(std::to_integer<char>(buf[0]) == 'x');
}

CPPIO_TEST_CASE(file_reader_records_syscall_errors_on_failed_open) {
    // A failed open surfaces on first read; the error path should count it.
    cppio::SyscallStats stats{};
    cppio::FileReader r("/no/such/cppio/path", &stats);
    std::array<std::byte, 4> buf{};
    auto res = r.read_some(std::span<std::byte>(buf));
    CPPIO_CHECK(!res.has_value());
    CPPIO_CHECK(stats.read_syscall_errors >= 1);
    CPPIO_CHECK(stats.read_syscalls == 0);  // no actual ::read happened
}

CPPIO_MAIN()
