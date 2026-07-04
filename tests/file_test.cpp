// Tests for FileReader / FileWriter: POSIX-backed blocking I/O with RAII.
// Uses temp files under the system temp dir; cleaned up per test.
#include "harness.hpp"

#include <sluice/file.hpp>
#include <sluice/copy.hpp>
#include <sluice/measurement.hpp>
#include <sluice/wal.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>

namespace {

// Makes a unique temp path (not yet created) and cleans it up on scope exit.
struct TempPath {
    std::filesystem::path p;
    TempPath() {
        auto dir = std::filesystem::temp_directory_path();
        std::ostringstream oss;
        oss << "sluice_test_" << std::hex << reinterpret_cast<std::uintptr_t>(this) << ".tmp";
        p = dir / oss.str();
    }
    ~TempPath() {
        try {
            std::filesystem::remove(p);
        } catch (...) {}
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

} // namespace

SLUICE_TEST_CASE(file_writer_creates_and_appends_bytes) {
    TempPath tp;
    {
        sluice::FileWriter w(tp.str());
        SLUICE_CHECK(w.opened());
        SLUICE_CHECK(w.write_all(sb("hello ")).has_value());
        SLUICE_CHECK(w.write_all(sb("world")).has_value());
        SLUICE_CHECK(w.flush().has_value()); // user-space no-op for now
    } // RAII closes
    SLUICE_CHECK(file_contains(tp.str(), "hello world"));
}

SLUICE_TEST_CASE(file_reader_reads_back_written_bytes) {
    TempPath tp;
    {
        sluice::FileWriter w(tp.str());
        SLUICE_CHECK(w.write_all(sb("file payload")).has_value());
    }
    sluice::FileReader r(tp.str());
    SLUICE_CHECK(r.opened());
    std::vector<std::byte> out(12);
    auto res = r.read_exact(std::span<std::byte>(out));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(std::memcmp(out.data(), "file payload", 12) == 0);
}

SLUICE_TEST_CASE(file_reader_returns_zero_at_eof) {
    TempPath tp;
    {
        sluice::FileWriter w(tp.str());
        SLUICE_CHECK(w.write_all(sb("ab")).has_value());
    }
    sluice::FileReader r(tp.str());
    std::array<std::byte, 2> a{};
    (void)r.read_some(std::span<std::byte>(a));
    std::array<std::byte, 2> b{};
    auto res = r.read_some(std::span<std::byte>(b));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
}

SLUICE_TEST_CASE(file_reader_open_missing_preserves_enoent) {
    sluice::FileReader r("/no/such/file/should/exist/sluice");
    SLUICE_CHECK(!r.opened());
    // read_some on a failed-open file surfaces the real errno (ENOENT), not a
    // synthetic code.
    std::array<std::byte, 4> buf{};
    auto res = r.read_some(std::span<std::byte>(buf));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::permission_denied);
    SLUICE_CHECK(res.error().os_errno == ENOENT);
}

SLUICE_TEST_CASE(file_reader_open_under_a_file_preserves_enotdir) {
    // ENOTDIR: opening "<existing regular file>/child" must fail with ENOTDIR,
    // and the real errno must survive to read_some's error.
    TempPath tp;
    {
        sluice::FileWriter w(tp.str());
        (void)w.write_all(sb("x"));
    }
    sluice::FileReader r(tp.str() + "/child");
    SLUICE_CHECK(!r.opened());
    std::array<std::byte, 4> buf{};
    auto res = r.read_some(std::span<std::byte>(buf));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().os_errno == ENOTDIR);
}

SLUICE_TEST_CASE(file_writer_open_failure_preserves_real_errno) {
    // Try to write-create under a path whose parent is a regular file -> ENOTDIR.
    TempPath tp;
    {
        sluice::FileWriter w(tp.str());
        (void)w.write_all(sb("x"));
    }
    sluice::FileWriter w(tp.str() + "/child");
    SLUICE_CHECK(!w.opened());
    auto res = w.write_some(sb("data"));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().os_errno == ENOTDIR);
}

SLUICE_TEST_CASE(file_copy_all_round_trips_through_real_files) {
    TempPath src_tp;
    TempPath dst_tp;
    {
        sluice::FileWriter w(src_tp.str());
        std::string big(2048, 'z');
        SLUICE_CHECK(w.write_all(sb(big)).has_value());
    }
    {
        sluice::FileReader r(src_tp.str());
        sluice::FileWriter w(dst_tp.str());
        auto res = sluice::copy_all(r, w);
        SLUICE_CHECK(res.has_value());
        SLUICE_CHECK(res.value() == 2048);
    }
    SLUICE_CHECK(file_contains(dst_tp.str(), std::string(2048, 'z')));
}

SLUICE_TEST_CASE(file_wal_records_round_trip_on_disk) {
    TempPath tp;
    {
        sluice::FileWriter w(tp.str());
        SLUICE_CHECK(sluice::wal::write_record(w, sb("rec-one")).has_value());
        SLUICE_CHECK(sluice::wal::write_record(w, sb("rec-two")).has_value());
    }
    sluice::FileReader r(tp.str());
    auto r1 = sluice::wal::read_record(r);
    auto r2 = sluice::wal::read_record(r);
    SLUICE_CHECK(r1.has_value());
    SLUICE_CHECK(r2.has_value());
    SLUICE_CHECK(std::memcmp(r1.value().data(), "rec-one", 7) == 0);
    SLUICE_CHECK(std::memcmp(r2.value().data(), "rec-two", 7) == 0);
}

SLUICE_TEST_CASE(file_writer_is_move_only) {
    TempPath tp;
    sluice::FileWriter w(tp.str());
    SLUICE_CHECK(w.opened());
    sluice::FileWriter moved = std::move(w);
    SLUICE_CHECK(moved.opened());
    SLUICE_CHECK(!w.opened()); // moved-from is released
    SLUICE_CHECK(moved.write_all(sb("moved")).has_value());
}

SLUICE_TEST_CASE(file_reader_is_move_only) {
    // Parity with the FileWriter move test: verifies the moved-from reader
    // releases its fd (no double-close) and the moved-to reader reads correctly.
    TempPath tp;
    {
        sluice::FileWriter w(tp.str());
        SLUICE_CHECK(w.write_all(sb("moved")).has_value());
    }
    sluice::FileReader r(tp.str());
    SLUICE_CHECK(r.opened());
    sluice::FileReader moved = std::move(r);
    SLUICE_CHECK(moved.opened());
    SLUICE_CHECK(!r.opened()); // moved-from is released
    std::array<std::byte, 5> buf{};
    auto res = moved.read_exact(std::span<std::byte>(buf));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(std::memcmp(buf.data(), "moved", 5) == 0);
}

// ---------- SyscallStats attachment (CPPIO-CORE-004) ----------

SLUICE_TEST_CASE(file_writer_records_syscall_stats) {
    TempPath tp;
    sluice::SyscallStats stats{};
    {
        sluice::FileWriter w(tp.str(), &stats);
        SLUICE_CHECK(w.write_all(sb("hello")).has_value());
    }
    SLUICE_CHECK(stats.write_syscalls >= 1);
    SLUICE_CHECK(stats.write_syscall_bytes == 5);
    SLUICE_CHECK(stats.write_syscall_errors == 0);
}

SLUICE_TEST_CASE(file_reader_records_syscall_stats) {
    TempPath tp;
    {
        sluice::FileWriter w(tp.str());
        SLUICE_CHECK(w.write_all(sb("hello")).has_value());
    }
    sluice::SyscallStats stats{};
    sluice::FileReader r(tp.str(), &stats);
    std::array<std::byte, 5> buf{};
    SLUICE_CHECK(r.read_exact(std::span<std::byte>(buf)).has_value());

    SLUICE_CHECK(stats.read_syscalls >= 1);
    SLUICE_CHECK(stats.read_syscall_bytes == 5);
    SLUICE_CHECK(stats.read_syscall_errors == 0);
}

SLUICE_TEST_CASE(file_stats_null_is_zero_overhead_and_semantics_unchanged) {
    // Null stats must not crash and must not change behavior.
    TempPath tp;
    {
        sluice::FileWriter w(tp.str(), nullptr);
        SLUICE_CHECK(w.write_all(sb("x")).has_value());
    }
    sluice::FileReader r(tp.str()); // default: no stats
    std::array<std::byte, 1> buf{};
    SLUICE_CHECK(r.read_exact(std::span<std::byte>(buf)).has_value());
    SLUICE_CHECK(std::to_integer<char>(buf[0]) == 'x');
}

SLUICE_TEST_CASE(file_reader_records_syscall_errors_on_failed_open) {
    // A failed open surfaces on first read; the error path should count it.
    sluice::SyscallStats stats{};
    sluice::FileReader r("/no/such/sluice/path", &stats);
    std::array<std::byte, 4> buf{};
    auto res = r.read_some(std::span<std::byte>(buf));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(stats.read_syscall_errors >= 1);
    SLUICE_CHECK(stats.read_syscalls == 0); // no actual ::read happened
}

SLUICE_MAIN()
