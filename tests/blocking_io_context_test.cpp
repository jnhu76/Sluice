// Tests for BlockingIoContext (CPPIO-CORE-009C). Verifies opens existing/new
// files, read/write through polymorphic pointers, missing/invalid paths error at
// open time, stats are wired, and the returned Writer is detectable as
// SyncableWriter.
#include "harness.hpp"

#include <sluice/file.hpp>
#include <sluice/io_context.hpp>
#include <sluice/measurement.hpp>
#include <sluice/sync.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath(const char* tag) {
        std::ostringstream oss;
        oss << "sluice_ioctx_" << tag << "_" << std::hex << reinterpret_cast<std::uintptr_t>(this)
            << ".tmp";
        p = std::filesystem::temp_directory_path() / oss.str();
    }
    ~TempPath() {
        try {
            std::filesystem::remove(p);
        } catch (...) {}
    }
    std::string str() const { return p.string(); }
};

bool file_has(const std::string& path, std::string_view want) {
    std::ifstream in(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    return content == std::string(want);
}

} // namespace

SLUICE_TEST_CASE(blocking_context_opens_existing_file_for_reading) {
    TempPath tp("r");
    {
        std::ofstream o(tp.str(), std::ios::binary);
        o.write("hello", 5);
    }
    sluice::BlockingIoContext ctx;
    auto r = ctx.open_reader(tp.str());
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() != nullptr);
    std::byte buf[5];
    auto rr = r.value()->read_some(buf);
    SLUICE_CHECK(rr.has_value() && rr.value() == 5);
}

SLUICE_TEST_CASE(blocking_context_opens_new_file_for_writing) {
    TempPath tp("w");
    sluice::BlockingIoContext ctx;
    auto w = ctx.open_writer(tp.str());
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w.value() != nullptr);
    auto wr = w.value()->write_some(std::as_bytes(std::span("data", 4)));
    SLUICE_CHECK(wr.has_value() && wr.value() == 4);
    SLUICE_CHECK(w.value()->flush().has_value());
    SLUICE_CHECK(file_has(tp.str(), "data"));
}

SLUICE_TEST_CASE(blocking_context_missing_reader_file_errors_at_open_time) {
    sluice::BlockingIoContext ctx;
    auto r = ctx.open_reader("/no/such/sluice/ioctx/missing");
    SLUICE_CHECK(!r.has_value()); // error immediate, not deferred
}

SLUICE_TEST_CASE(blocking_context_invalid_writer_path_errors_at_open_time) {
    sluice::BlockingIoContext ctx;
    auto w = ctx.open_writer("/no/such/sluice/ioctx/dir/out");
    SLUICE_CHECK(!w.has_value());
}

SLUICE_TEST_CASE(blocking_context_wires_stats_pointers) {
    TempPath tp("s");
    sluice::SyscallStats ss{};
    sluice::VectorStats vs{};
    sluice::SyncStats sync{};
    sluice::BlockingIoContext ctx;
    {
        auto w = ctx.open_writer(tp.str(), sluice::OpenWriterOptions{.syscall_stats = &ss,
                                                                     .vector_stats = &vs,
                                                                     .sync_stats = &sync});
        SLUICE_CHECK(w.has_value());
        SLUICE_CHECK(w.value()->write_some(std::as_bytes(std::span("ab", 2))).has_value());
        // Reach sync_all through the SyncableWriter capability the FileWriter
        // exposes, rather than downcasting to the concrete (hidden) type.
        auto* sw = dynamic_cast<sluice::SyncableWriter*>(w.value().get());
        SLUICE_CHECK(sw != nullptr);
        SLUICE_CHECK(sw->sync_all().has_value());
    }
    SLUICE_CHECK(ss.write_syscalls >= 1);
    SLUICE_CHECK(sync.sync_all_calls == 1);
    // vector_stats only moves when write_vec is used; just confirm it's wired
    // (non-null path didn't crash and the writer accepted it).
}

SLUICE_TEST_CASE(blocking_context_returned_writer_is_syncable_writer) {
    TempPath tp("sy");
    sluice::BlockingIoContext ctx;
    auto w = ctx.open_writer(tp.str());
    SLUICE_CHECK(w.has_value());
    sluice::Writer& as_writer = *w.value();
    auto* cap = dynamic_cast<sluice::SyncableWriter*>(&as_writer);
    SLUICE_CHECK(cap != nullptr); // FileWriter is SyncableWriter
}

SLUICE_TEST_CASE(blocking_context_copy_through_context_handles) {
    TempPath in_tp("ci");
    TempPath out_tp("co");
    {
        std::ofstream o(in_tp.str(), std::ios::binary);
        o.write("round-trip", 10);
    }
    sluice::BlockingIoContext ctx;
    auto r = ctx.open_reader(in_tp.str());
    auto w = ctx.open_writer(out_tp.str());
    SLUICE_CHECK(r.has_value() && w.has_value());
    std::byte buf[10];
    auto rr = r.value()->read_some(buf);
    SLUICE_CHECK(rr.has_value() && rr.value() == 10);
    auto wr = w.value()->write_some(std::as_bytes(std::span(buf, 10)));
    SLUICE_CHECK(wr.has_value() && wr.value() == 10);
    SLUICE_CHECK(w.value()->flush().has_value());
    SLUICE_CHECK(file_has(out_tp.str(), "round-trip"));
}

SLUICE_MAIN()
