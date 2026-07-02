// Tests for FileWriter sync operations and SyncStats (CPPIO-CORE-008C/008D).
// Verifies sync_data/sync_all behavior: success path, open-error preservation,
// invalid-state on bad fd, that flush() does not sync, and that SyncStats counts.
#include "harness.hpp"

#include <cppio/file.hpp>
#include <cppio/measurement.hpp>
#include <cppio/sync.hpp>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath() {
        p = std::filesystem::temp_directory_path() /
            ("cppio_sync_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tmp");
    }
    ~TempPath() { std::filesystem::remove(p); }
    std::string str() const { return p.string(); }
};

bool file_has(const std::string& path, std::string_view want) {
    std::ifstream in(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    return content == std::string(want);
}

}  // namespace

CPPIO_TEST_CASE(file_writer_is_syncable_writer) {
    TempPath tp;
    cppio::FileWriter w(tp.str());
    CPPIO_CHECK(w.opened());
    cppio::Writer& as_writer = w;
    auto* cap = dynamic_cast<cppio::SyncableWriter*>(&as_writer);
    CPPIO_CHECK(cap != nullptr);
}

CPPIO_TEST_CASE(file_writer_sync_all_succeeds_after_write) {
    TempPath tp;
    cppio::FileWriter w(tp.str());
    CPPIO_CHECK(w.opened());
    auto wr = w.write_some(std::as_bytes(std::span("data", 4)));
    CPPIO_CHECK(wr.has_value() && wr.value() == 4);
    auto s = w.sync_all();
    CPPIO_CHECK(s.has_value());  // fsync on a freshly written temp file succeeds
    CPPIO_CHECK(file_has(tp.str(), "data"));
}

CPPIO_TEST_CASE(file_writer_sync_data_succeeds_after_write) {
    TempPath tp;
    cppio::FileWriter w(tp.str());
    CPPIO_CHECK(w.opened());
    auto wr = w.write_some(std::as_bytes(std::span("payload", 7)));
    CPPIO_CHECK(wr.has_value() && wr.value() == 7);
    auto s = w.sync_data();  // fdatasync; succeeds where supported
    CPPIO_CHECK(s.has_value());
}

CPPIO_TEST_CASE(file_writer_sync_preserves_open_error_errno) {
    cppio::FileWriter w("/no/such/cppio/sync/path/xyz");
    CPPIO_CHECK(!w.opened());
    auto s = w.sync_all();
    CPPIO_CHECK(!s.has_value());
    CPPIO_CHECK(s.error().code == cppio::IoError::Code::permission_denied);
    CPPIO_CHECK(s.error().os_errno == ENOENT);
    auto sd = w.sync_data();
    CPPIO_CHECK(!sd.has_value());
    CPPIO_CHECK(sd.error().code == cppio::IoError::Code::permission_denied);
}

CPPIO_TEST_CASE(file_writer_sync_on_invalid_writer_returns_invalid_state) {
    cppio::FileWriter w;  // default-constructed: no fd, no open_error_
    CPPIO_CHECK(!w.opened());
    auto s = w.sync_all();
    CPPIO_CHECK(!s.has_value());
    CPPIO_CHECK(s.error().code == cppio::IoError::Code::invalid_state);
}

CPPIO_TEST_CASE(file_writer_flush_does_not_invoke_sync) {
    // Structural + behavioral: flush() is a documented no-op that returns ok and
    // does not touch SyncStats. We attach stats and confirm flush leaves them 0.
    TempPath tp;
    cppio::SyncStats st{};
    cppio::FileWriter w(tp.str(), nullptr, nullptr, &st);
    CPPIO_CHECK(w.write_some(std::as_bytes(std::span("x", 1))).has_value());
    auto f = w.flush();
    CPPIO_CHECK(f.has_value());
    CPPIO_CHECK(st.sync_data_calls == 0);  // flush is NOT a sync
    CPPIO_CHECK(st.sync_all_calls == 0);
}

CPPIO_TEST_CASE(sync_stats_count_calls_and_errors) {
    TempPath tp;
    cppio::SyncStats st{};
    {
        cppio::FileWriter w(tp.str(), nullptr, nullptr, &st);
        CPPIO_CHECK(w.write_some(std::as_bytes(std::span("ab", 2))).has_value());
        CPPIO_CHECK(w.sync_all().has_value());
        CPPIO_CHECK(w.sync_data().has_value());
    }
    CPPIO_CHECK(st.sync_all_calls == 1);
    CPPIO_CHECK(st.sync_data_calls == 1);
    CPPIO_CHECK(st.sync_all_errors == 0);
    CPPIO_CHECK(st.sync_data_errors == 0);

    // Error path: sync on a bad-open file increments the error counter.
    cppio::SyncStats st2{};
    cppio::FileWriter bad("/no/such/cppio/sync/path2", nullptr, nullptr, &st2);
    auto s = bad.sync_all();
    CPPIO_CHECK(!s.has_value());
    CPPIO_CHECK(st2.sync_all_errors == 1);
    CPPIO_CHECK(st2.sync_all_calls == 0);
}

CPPIO_TEST_CASE(sync_stats_nullptr_changes_nothing) {
    TempPath tp;
    cppio::FileWriter w(tp.str());  // no sync stats
    CPPIO_CHECK(w.write_some(std::as_bytes(std::span("c", 1))).has_value());
    auto s = w.sync_all();
    CPPIO_CHECK(s.has_value());  // behavior unchanged with null stats
}

CPPIO_MAIN()
