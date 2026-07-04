// Tests for positional blocking file I/O (sluice-CORE-018S, Phase 4).
//
// Exercises read_at / write_at / read_vec_at / write_vec_at on FileReader /
// FileWriter through public interfaces. Verifies: explicit offset, no shared
// cursor mutation, EOF at offset, zero-length ops, zero-progress failure, and
// vector positional forms. Driven by real temp files (no fake needed — these
// are the real POSIX pread/pwrite/preadv/pwritev paths).
#include "harness.hpp"

#include <sluice/file.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

// RAII temp file path; cleaned up on destruction. Unique per instance.
class TempPath {
public:
    TempPath() {
        auto base = std::filesystem::temp_directory_path() /
                    ("sluice_posional_test_" + std::to_string(counter_++) + ".tmp");
        path_ = base.string();
    }
    ~TempPath() {
        // Guard the moved-from state: the move ctor clears path_, and remove("")
        // on an empty path is a needless (if non-throwing) no-op. Defensive.
        if (!path_.empty()) std::filesystem::remove(path_);
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    TempPath(TempPath&& o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
    TempPath& operator=(TempPath&& o) noexcept {
        if (this != &o) {
            if (!path_.empty()) std::filesystem::remove(path_);
            path_ = std::move(o.path_);
            o.path_.clear();
        }
        return *this;
    }
    const std::string& path() const { return path_; }
private:
    std::string path_;
    static inline long counter_ = 0;
};

// Seed a file with `contents` and return its path. Uses raw assert/exit rather
// than SLUICE_CHECK because SLUICE_CHECK's `return;` is invalid in a function
// returning TempPath by value.
TempPath seed_file(std::string_view contents) {
    TempPath tp;
    FILE* f = std::fopen(tp.path().c_str(), "wb");
    if (f == nullptr) { std::fprintf(stderr, "fopen failed\n"); std::exit(1); }
    if (std::fwrite(contents.data(), 1, contents.size(), f) != contents.size()) {
        std::fprintf(stderr, "fwrite short\n"); std::exit(1);
    }
    std::fclose(f);
    return tp;
}

}  // namespace

// ---- Slice 1: read_at reads from an explicit offset -------------------------

SLUICE_TEST_CASE(read_at_reads_from_explicit_offset) {
    auto tp = seed_file("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    sluice::FileReader r(tp.path());
    SLUICE_CHECK(r.opened());
    std::array<std::byte, 5> buf{};
    auto res = r.read_at(10, buf);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 5);
    SLUICE_CHECK(std::memcmp(buf.data(), "KLMNO", 5) == 0);
}

// ---- Slice 2: read_at does not move the shared cursor -----------------------

SLUICE_TEST_CASE(read_at_does_not_move_shared_cursor) {
    auto tp = seed_file("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    sluice::FileReader r(tp.path());
    std::array<std::byte, 5> buf{};
    // read_at(20,...) must NOT advance the implicit cursor (which starts at 0).
    auto at = r.read_at(20, buf);
    SLUICE_CHECK(at.has_value());
    SLUICE_CHECK(at.value() == 5);  // "UVXYZ"
    // A subsequent cursor-based read_some should still read from offset 0.
    auto cur = r.read_some(buf);
    SLUICE_CHECK(cur.has_value());
    SLUICE_CHECK(cur.value() == 5);
    SLUICE_CHECK(std::memcmp(buf.data(), "ABCDE", 5) == 0);
}

// ---- Slice 3: read_at EOF at offset -----------------------------------------

SLUICE_TEST_CASE(read_at_returns_zero_at_eof) {
    auto tp = seed_file("ABC");
    sluice::FileReader r(tp.path());
    std::array<std::byte, 4> buf{};
    // Reading past the end returns 0 (EOF), not an error.
    auto res = r.read_at(3, buf);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
}

// ---- Slice 4: read_at zero-length is a no-op --------------------------------

SLUICE_TEST_CASE(read_at_zero_length_is_noop) {
    auto tp = seed_file("ABC");
    sluice::FileReader r(tp.path());
    auto res = r.read_at(0, std::span<std::byte>{});
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 0);
}

// ---- Slice 5: write_at writes at an explicit offset -------------------------

SLUICE_TEST_CASE(write_at_writes_at_explicit_offset) {
    // FileWriter ctor truncates, so test write_at by writing at an offset into a
    // fresh file and verifying a gap of zeros appears before the written bytes.
    TempPath tp;
    sluice::FileWriter w(tp.path());
    SLUICE_CHECK(w.opened());
    const std::string payload = "XY";
    std::span<const std::byte> src{
        reinterpret_cast<const std::byte*>(payload.data()), payload.size()};
    auto res = w.write_at(3, src);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 2);
    SLUICE_CHECK(w.flush().has_value());
    // Read back: bytes 0-2 are zero (sparse gap), 3-4 are "XY".
    sluice::FileReader r(tp.path());
    std::array<std::byte, 5> buf{};
    auto got = r.read_at(0, buf);
    SLUICE_CHECK(got.has_value());
    SLUICE_CHECK(got.value() == 5);
    SLUICE_CHECK(buf[0] == std::byte{0});
    SLUICE_CHECK(buf[1] == std::byte{0});
    SLUICE_CHECK(buf[2] == std::byte{0});
    SLUICE_CHECK(buf[3] == std::byte{'X'});
    SLUICE_CHECK(buf[4] == std::byte{'Y'});
}

// ---- Slice 6: write_at does not move the shared cursor ----------------------

SLUICE_TEST_CASE(write_at_does_not_move_shared_cursor) {
    TempPath tp;
    sluice::FileWriter w(tp.path());
    SLUICE_CHECK(w.opened());
    // Write "AB" at the cursor (offset 0).
    std::string ab = "AB";
    auto a = w.write_some(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(ab.data()), ab.size()});
    SLUICE_CHECK(a.has_value());
    SLUICE_CHECK(a.value() == 2);
    // write_at(10, "Z") must NOT advance the cursor (still at 2).
    std::string z = "Z";
    auto at = w.write_at(10, std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(z.data()), z.size()});
    SLUICE_CHECK(at.has_value());
    SLUICE_CHECK(at.value() == 1);
    // Next cursor-based write goes to offset 2, not 11.
    std::string cd = "CD";
    auto b = w.write_some(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(cd.data()), cd.size()});
    SLUICE_CHECK(b.has_value());
    SLUICE_CHECK(b.value() == 2);
    SLUICE_CHECK(w.flush().has_value());
    // Verify: offset 0-1 "AB", 2-3 "CD", 10 "Z".
    sluice::FileReader r(tp.path());
    std::array<std::byte, 11> buf{};
    auto got = r.read_at(0, buf);
    SLUICE_CHECK(got.has_value());
    SLUICE_CHECK(got.value() == 11);
    SLUICE_CHECK(buf[0] == std::byte{'A'});
    SLUICE_CHECK(buf[1] == std::byte{'B'});
    SLUICE_CHECK(buf[2] == std::byte{'C'});
    SLUICE_CHECK(buf[3] == std::byte{'D'});
    SLUICE_CHECK(buf[10] == std::byte{'Z'});
}

// ---- Slice 7: read_vec_at (preadv) ------------------------------------------

SLUICE_TEST_CASE(read_vec_at_scatters_from_offset) {
    auto tp = seed_file("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    sluice::FileReader r(tp.path());
    std::array<std::byte, 3> a{};
    std::array<std::byte, 3> b{};
    sluice::IoSlice dsts[2] = {sluice::IoSlice{a}, sluice::IoSlice{b}};
    // read_vec_at(10,...) fills a="KLM", b="NOP".
    auto res = r.read_vec_at(10, dsts);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 6);
    SLUICE_CHECK(std::memcmp(a.data(), "KLM", 3) == 0);
    SLUICE_CHECK(std::memcmp(b.data(), "NOP", 3) == 0);
}

SLUICE_TEST_CASE(read_vec_at_does_not_move_cursor) {
    auto tp = seed_file("ABCDEFGHIJ");
    sluice::FileReader r(tp.path());
    std::array<std::byte, 2> a{};
    sluice::IoSlice dsts[1] = {sluice::IoSlice{a}};
    auto at = r.read_vec_at(5, dsts);  // "FG"
    SLUICE_CHECK(at.has_value());
    SLUICE_CHECK(at.value() == 2);
    // Cursor still at 0.
    std::array<std::byte, 3> buf{};
    auto cur = r.read_some(buf);
    SLUICE_CHECK(cur.has_value());
    SLUICE_CHECK(cur.value() == 3);
    SLUICE_CHECK(std::memcmp(buf.data(), "ABC", 3) == 0);
}

// ---- Slice 8: write_vec_at (pwritev) ----------------------------------------

SLUICE_TEST_CASE(write_vec_at_gathers_at_offset) {
    TempPath tp;
    sluice::FileWriter w(tp.path());
    SLUICE_CHECK(w.opened());
    std::string p1 = "AB";
    std::string p2 = "CDE";
    sluice::ConstIoSlice srcs[2] = {
        sluice::ConstIoSlice{std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(p1.data()), p1.size()}},
        sluice::ConstIoSlice{std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(p2.data()), p2.size()}}};
    // write_vec_at(2,...) writes "ABCDE" at offset 2 (zeros at 0-1).
    auto res = w.write_vec_at(2, srcs);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 5);
    SLUICE_CHECK(w.flush().has_value());
    sluice::FileReader r(tp.path());
    std::array<std::byte, 7> buf{};
    auto got = r.read_at(0, buf);
    SLUICE_CHECK(got.has_value());
    SLUICE_CHECK(got.value() == 7);
    SLUICE_CHECK(buf[0] == std::byte{0});
    SLUICE_CHECK(buf[1] == std::byte{0});
    SLUICE_CHECK(buf[2] == std::byte{'A'});
    SLUICE_CHECK(buf[6] == std::byte{'E'});
}

// ---- Slice 9: read_at_exact (derived) --------------------------------------

SLUICE_TEST_CASE(read_at_exact_fills_buffer_from_offset) {
    auto tp = seed_file("0123456789");
    sluice::FileReader r(tp.path());
    std::array<std::byte, 4> buf{};
    auto res = r.read_at_exact(3, buf);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(std::memcmp(buf.data(), "3456", 4) == 0);
}

SLUICE_TEST_CASE(read_at_exact_eof_before_any_bytes) {
    auto tp = seed_file("ABC");  // 3 bytes
    sluice::FileReader r(tp.path());
    std::array<std::byte, 4> buf{};
    // offset 3 is at EOF before any byte: eof.
    auto res = r.read_at_exact(3, buf);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST_CASE(read_at_exact_eof_after_partial_bytes) {
    auto tp = seed_file("ABC");  // 3 bytes
    sluice::FileReader r(tp.path());
    std::array<std::byte, 5> buf{};
    // offset 1: can read "BC" (2 bytes) then EOF before filling 5 -> eof.
    auto res = r.read_at_exact(1, buf);
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == sluice::IoError::Code::eof);
}

SLUICE_TEST_CASE(read_at_exact_zero_length_is_success) {
    auto tp = seed_file("ABC");
    sluice::FileReader r(tp.path());
    auto res = r.read_at_exact(0, std::span<std::byte>{});
    SLUICE_CHECK(res.has_value());
}

// ---- Slice 10: write_at_all (derived) --------------------------------------

SLUICE_TEST_CASE(write_at_all_writes_full_buffer_at_offset) {
    TempPath tp;
    sluice::FileWriter w(tp.path());
    SLUICE_CHECK(w.opened());
    std::string payload = "HELLO";
    auto res = w.write_at_all(2, std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(payload.data()), payload.size()});
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(w.flush().has_value());
    sluice::FileReader r(tp.path());
    std::array<std::byte, 7> buf{};
    auto got = r.read_at(0, buf);
    SLUICE_CHECK(got.has_value());
    SLUICE_CHECK(got.value() == 7);
    SLUICE_CHECK(buf[0] == std::byte{0});
    SLUICE_CHECK(buf[1] == std::byte{0});
    SLUICE_CHECK(std::memcmp(buf.data() + 2, "HELLO", 5) == 0);
}

SLUICE_TEST_CASE(write_at_all_zero_length_is_success) {
    TempPath tp;
    sluice::FileWriter w(tp.path());
    auto res = w.write_at_all(0, std::span<const std::byte>{});
    SLUICE_CHECK(res.has_value());
}

SLUICE_MAIN()




