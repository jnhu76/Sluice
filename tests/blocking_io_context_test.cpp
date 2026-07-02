// Tests for BlockingIoContext (CPPIO-CORE-009C). Verifies opens existing/new
// files, read/write through polymorphic pointers, missing/invalid paths error at
// open time, stats are wired, and the returned Writer is detectable as
// SyncableWriter.
#include "harness.hpp"

#include <cppio/file.hpp>
#include <cppio/io_context.hpp>
#include <cppio/measurement.hpp>
#include <cppio/sync.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath(const char* tag) {
        p = std::filesystem::temp_directory_path() /
            ("cppio_ioctx_" + std::string(tag) + "_" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tmp");
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

CPPIO_TEST_CASE(blocking_context_opens_existing_file_for_reading) {
    TempPath tp("r");
    {
        std::ofstream o(tp.str(), std::ios::binary);
        o.write("hello", 5);
    }
    cppio::BlockingIoContext ctx;
    auto r = ctx.open_reader(tp.str());
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(r.value() != nullptr);
    std::byte buf[5];
    auto rr = r.value()->read_some(buf);
    CPPIO_CHECK(rr.has_value() && rr.value() == 5);
}

CPPIO_TEST_CASE(blocking_context_opens_new_file_for_writing) {
    TempPath tp("w");
    cppio::BlockingIoContext ctx;
    auto w = ctx.open_writer(tp.str());
    CPPIO_CHECK(w.has_value());
    CPPIO_CHECK(w.value() != nullptr);
    auto wr = w.value()->write_some(std::as_bytes(std::span("data", 4)));
    CPPIO_CHECK(wr.has_value() && wr.value() == 4);
    CPPIO_CHECK(w.value()->flush().has_value());
    CPPIO_CHECK(file_has(tp.str(), "data"));
}

CPPIO_TEST_CASE(blocking_context_missing_reader_file_errors_at_open_time) {
    cppio::BlockingIoContext ctx;
    auto r = ctx.open_reader("/no/such/cppio/ioctx/missing");
    CPPIO_CHECK(!r.has_value());  // error immediate, not deferred
}

CPPIO_TEST_CASE(blocking_context_invalid_writer_path_errors_at_open_time) {
    cppio::BlockingIoContext ctx;
    auto w = ctx.open_writer("/no/such/cppio/ioctx/dir/out");
    CPPIO_CHECK(!w.has_value());
}

CPPIO_TEST_CASE(blocking_context_wires_stats_pointers) {
    TempPath tp("s");
    cppio::SyscallStats ss{};
    cppio::VectorStats vs{};
    cppio::SyncStats sync{};
    cppio::BlockingIoContext ctx;
    {
        auto w = ctx.open_writer(tp.str(),
                                 cppio::OpenWriterOptions{&ss, &vs, &sync});
        CPPIO_CHECK(w.has_value());
        CPPIO_CHECK(w.value()->write_some(std::as_bytes(std::span("ab", 2))).has_value());
        // Reach sync_all through the SyncableWriter capability the FileWriter
        // exposes, rather than downcasting to the concrete (hidden) type.
        auto* sw = dynamic_cast<cppio::SyncableWriter*>(w.value().get());
        CPPIO_CHECK(sw != nullptr);
        CPPIO_CHECK(sw->sync_all().has_value());
    }
    CPPIO_CHECK(ss.write_syscalls >= 1);
    CPPIO_CHECK(sync.sync_all_calls == 1);
    // vector_stats only moves when write_vec is used; just confirm it's wired
    // (non-null path didn't crash and the writer accepted it).
}

CPPIO_TEST_CASE(blocking_context_returned_writer_is_syncable_writer) {
    TempPath tp("sy");
    cppio::BlockingIoContext ctx;
    auto w = ctx.open_writer(tp.str());
    CPPIO_CHECK(w.has_value());
    cppio::Writer& as_writer = *w.value();
    auto* cap = dynamic_cast<cppio::SyncableWriter*>(&as_writer);
    CPPIO_CHECK(cap != nullptr);  // FileWriter is SyncableWriter
}

CPPIO_TEST_CASE(blocking_context_copy_through_context_handles) {
    TempPath in_tp("ci"), out_tp("co");
    {
        std::ofstream o(in_tp.str(), std::ios::binary);
        o.write("round-trip", 10);
    }
    cppio::BlockingIoContext ctx;
    auto r = ctx.open_reader(in_tp.str());
    auto w = ctx.open_writer(out_tp.str());
    CPPIO_CHECK(r.has_value() && w.has_value());
    std::byte buf[10];
    auto rr = r.value()->read_some(buf);
    CPPIO_CHECK(rr.has_value() && rr.value() == 10);
    auto wr = w.value()->write_some(std::as_bytes(std::span(buf, 10)));
    CPPIO_CHECK(wr.has_value() && wr.value() == 10);
    CPPIO_CHECK(w.value()->flush().has_value());
    CPPIO_CHECK(file_has(out_tp.str(), "round-trip"));
}

CPPIO_MAIN()
