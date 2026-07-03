// Tests for FileWriter sync operations and SyncStats (CPPIO-CORE-008C/008D).
// Verifies sync_data/sync_all behavior: success path, open-error preservation,
// invalid-state on bad fd, that flush() does not sync, and that SyncStats counts.
#include "harness.hpp"

#include <sluice/file.hpp>
#include <sluice/measurement.hpp>
#include <sluice/sync.hpp>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath() {
        std::ostringstream oss;
        oss << "sluice_sync_" << std::hex << reinterpret_cast<std::uintptr_t>(this) << ".tmp";
        p = std::filesystem::temp_directory_path() / oss.str();
    }
    ~TempPath() {
        // Swallow cleanup errors: a failing remove (already gone, permission)
        // must not throw during stack unwinding and terminate the test process.
        try { std::filesystem::remove(p); } catch (...) {}
    }
    std::string str() const { return p.string(); }
};

bool file_has(const std::string& path, std::string_view want) {
    std::ifstream in(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    return content == std::string(want);
}

}  // namespace

SLUICE_TEST_CASE(file_writer_is_syncable_writer) {
    TempPath tp;
    sluice::FileWriter w(tp.str());
    SLUICE_CHECK(w.opened());
    sluice::Writer& as_writer = w;
    auto* cap = dynamic_cast<sluice::SyncableWriter*>(&as_writer);
    SLUICE_CHECK(cap != nullptr);
}

SLUICE_TEST_CASE(file_writer_sync_all_succeeds_after_write) {
    TempPath tp;
    sluice::FileWriter w(tp.str());
    SLUICE_CHECK(w.opened());
    auto wr = w.write_some(std::as_bytes(std::span("data", 4)));
    SLUICE_CHECK(wr.has_value() && wr.value() == 4);
    auto s = w.sync_all();
    SLUICE_CHECK(s.has_value());  // fsync on a freshly written temp file succeeds
    SLUICE_CHECK(file_has(tp.str(), "data"));
}

SLUICE_TEST_CASE(file_writer_sync_data_succeeds_after_write) {
    TempPath tp;
    sluice::FileWriter w(tp.str());
    SLUICE_CHECK(w.opened());
    auto wr = w.write_some(std::as_bytes(std::span("payload", 7)));
    SLUICE_CHECK(wr.has_value() && wr.value() == 7);
    auto s = w.sync_data();  // fdatasync; succeeds where supported
    SLUICE_CHECK(s.has_value());
}

SLUICE_TEST_CASE(file_writer_sync_preserves_open_error_errno) {
    sluice::FileWriter w("/no/such/sluice/sync/path/xyz");
    SLUICE_CHECK(!w.opened());
    auto s = w.sync_all();
    SLUICE_CHECK(!s.has_value());
    SLUICE_CHECK(s.error().code == sluice::IoError::Code::permission_denied);
    SLUICE_CHECK(s.error().os_errno == ENOENT);
    auto sd = w.sync_data();
    SLUICE_CHECK(!sd.has_value());
    SLUICE_CHECK(sd.error().code == sluice::IoError::Code::permission_denied);
}

SLUICE_TEST_CASE(file_writer_sync_on_invalid_writer_returns_invalid_state) {
    sluice::FileWriter w;  // default-constructed: no fd, no open_error_
    SLUICE_CHECK(!w.opened());
    auto s = w.sync_all();
    SLUICE_CHECK(!s.has_value());
    SLUICE_CHECK(s.error().code == sluice::IoError::Code::invalid_state);
}

SLUICE_TEST_CASE(file_writer_flush_does_not_invoke_sync) {
    // Structural + behavioral: flush() is a documented no-op that returns ok and
    // does not touch SyncStats. We attach stats and confirm flush leaves them 0.
    TempPath tp;
    sluice::SyncStats st{};
    sluice::FileWriter w(tp.str(), nullptr, nullptr, &st);
    SLUICE_CHECK(w.write_some(std::as_bytes(std::span("x", 1))).has_value());
    auto f = w.flush();
    SLUICE_CHECK(f.has_value());
    SLUICE_CHECK(st.sync_data_calls == 0);  // flush is NOT a sync
    SLUICE_CHECK(st.sync_all_calls == 0);
}

SLUICE_TEST_CASE(sync_stats_count_calls_and_errors) {
    TempPath tp;
    sluice::SyncStats st{};
    {
        sluice::FileWriter w(tp.str(), nullptr, nullptr, &st);
        SLUICE_CHECK(w.write_some(std::as_bytes(std::span("ab", 2))).has_value());
        SLUICE_CHECK(w.sync_all().has_value());
        SLUICE_CHECK(w.sync_data().has_value());
    }
    SLUICE_CHECK(st.sync_all_calls == 1);
    SLUICE_CHECK(st.sync_data_calls == 1);
    SLUICE_CHECK(st.sync_all_errors == 0);
    SLUICE_CHECK(st.sync_data_errors == 0);

    // Error path: sync on a bad-open file increments the error counter.
    sluice::SyncStats st2{};
    sluice::FileWriter bad("/no/such/sluice/sync/path2", nullptr, nullptr, &st2);
    auto s = bad.sync_all();
    SLUICE_CHECK(!s.has_value());
    SLUICE_CHECK(st2.sync_all_errors == 1);
    SLUICE_CHECK(st2.sync_all_calls == 0);
}

SLUICE_TEST_CASE(sync_stats_nullptr_changes_nothing) {
    TempPath tp;
    sluice::FileWriter w(tp.str());  // no sync stats
    SLUICE_CHECK(w.write_some(std::as_bytes(std::span("c", 1))).has_value());
    auto s = w.sync_all();
    SLUICE_CHECK(s.has_value());  // behavior unchanged with null stats
}

SLUICE_MAIN()
