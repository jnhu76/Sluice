// Tests for wal::WalWriter durability wrapper (CPPIO-CORE-008E). Verifies the
// three LSNs and the invariant durable <= flushed <= written across
// write/flush/sync, failure paths, and the vector write path.
#include "harness.hpp"

#include <cppio/fault.hpp>
#include <cppio/wal.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace {

bool invariant(const cppio::wal::WalWriter& w) {
    return w.durable_lsn() <= w.flushed_lsn() && w.flushed_lsn() <= w.written_lsn();
}

std::vector<std::byte> bytes_of(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}
std::span<const std::byte> span_of(const std::vector<std::byte>& v) {
    return std::span<const std::byte>(v.data(), v.size());
}

// A Writer that can be made to fail write/flush on demand, plus an optional
// sync capability that can be made to fail.
class ControlledWriter final : public cppio::Writer, public cppio::SyncableWriter {
public:
    std::vector<std::byte> sink;
    bool fail_write = false;
    bool fail_flush = false;
    bool fail_sync = false;
    int flush_calls = 0;
    int sync_data_calls = 0;

    cppio::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        if (fail_write) {
            return cppio::make_unexpected<std::size_t>(
                cppio::IoError{cppio::IoError::Code::no_space});
        }
        sink.insert(sink.end(), src.begin(), src.end());
        return src.size();
    }
    cppio::Result<void> flush() override {
        ++flush_calls;
        if (fail_flush) {
            return cppio::make_unexpected<void>(
                cppio::IoError{cppio::IoError::Code::backend_error});
        }
        return {};
    }
    cppio::Result<void> sync_data() override {
        ++sync_data_calls;
        if (fail_sync) {
            return cppio::make_unexpected<void>(
                cppio::IoError{cppio::IoError::Code::backend_error});
        }
        return {};
    }
    cppio::Result<void> sync_all() override { return sync_data(); }
};

}  // namespace

CPPIO_TEST_CASE(wal_writer_initial_lsns_all_zero) {
    cppio::MemoryWriter sink;
    cppio::wal::WalWriter w(sink);
    CPPIO_CHECK(w.written_lsn() == 0);
    CPPIO_CHECK(w.flushed_lsn() == 0);
    CPPIO_CHECK(w.durable_lsn() == 0);
    CPPIO_CHECK(invariant(w));
}

CPPIO_TEST_CASE(wal_writer_write_advances_only_written_lsn) {
    cppio::MemoryWriter sink;
    cppio::wal::WalWriter w(sink);
    auto payload = bytes_of("hello");
    CPPIO_CHECK(w.write_record(span_of(payload)).has_value());
    // framed size = 8 + 5 + 4 = 17
    CPPIO_CHECK(w.written_lsn() == 17);
    CPPIO_CHECK(w.flushed_lsn() == 0);
    CPPIO_CHECK(w.durable_lsn() == 0);
    CPPIO_CHECK(invariant(w));
}

CPPIO_TEST_CASE(wal_writer_flush_advances_flushed_not_durable) {
    cppio::MemoryWriter sink;
    cppio::wal::WalWriter w(sink);
    auto payload = bytes_of("hello");
    CPPIO_CHECK(w.write_record(span_of(payload)).has_value());
    CPPIO_CHECK(w.flush().has_value());
    CPPIO_CHECK(w.flushed_lsn() == 17);
    CPPIO_CHECK(w.durable_lsn() == 0);  // not synced yet
    CPPIO_CHECK(invariant(w));
}

CPPIO_TEST_CASE(wal_writer_sync_advances_durable_after_flush_and_sync) {
    ControlledWriter cw;
    cppio::wal::WalWriter w(cw, &cw);  // writer IS the SyncableWriter
    auto payload = bytes_of("data");
    CPPIO_CHECK(w.write_record(span_of(payload)).has_value());  // 8+4+4=16
    CPPIO_CHECK(w.sync().has_value());
    CPPIO_CHECK(w.flushed_lsn() == 16);
    CPPIO_CHECK(w.durable_lsn() == 16);
    CPPIO_CHECK(cw.sync_data_calls == 1);
    CPPIO_CHECK(invariant(w));
}

CPPIO_TEST_CASE(wal_writer_sync_without_syncable_returns_invalid_state) {
    cppio::MemoryWriter sink;
    cppio::wal::WalWriter w(sink);  // no SyncableWriter
    auto payload = bytes_of("data");
    CPPIO_CHECK(w.write_record(span_of(payload)).has_value());
    auto s = w.sync();
    CPPIO_CHECK(!s.has_value());
    CPPIO_CHECK(s.error().code == cppio::IoError::Code::invalid_state);
    // flush still succeeded before the sync attempt, so flushed advanced.
    CPPIO_CHECK(w.flushed_lsn() == 16);
    CPPIO_CHECK(w.durable_lsn() == 0);
    CPPIO_CHECK(invariant(w));
}

CPPIO_TEST_CASE(wal_writer_write_failure_does_not_advance_written_lsn) {
    ControlledWriter cw;
    cw.fail_write = true;
    cppio::wal::WalWriter w(cw, &cw);
    auto payload = bytes_of("nope");
    auto r = w.write_record(span_of(payload));
    CPPIO_CHECK(!r.has_value());
    CPPIO_CHECK(w.written_lsn() == 0);  // unchanged on failure
    CPPIO_CHECK(invariant(w));
}

CPPIO_TEST_CASE(wal_writer_flush_failure_does_not_advance_flushed_lsn) {
    ControlledWriter cw;
    cw.fail_flush = true;
    cppio::wal::WalWriter w(cw, &cw);
    auto payload = bytes_of("x");
    CPPIO_CHECK(w.write_record(span_of(payload)).has_value());  // written=13
    auto f = w.flush();
    CPPIO_CHECK(!f.has_value());
    CPPIO_CHECK(w.flushed_lsn() == 0);  // unchanged on failure
    CPPIO_CHECK(w.written_lsn() == 13);
    CPPIO_CHECK(invariant(w));
}

CPPIO_TEST_CASE(wal_writer_sync_failure_does_not_advance_durable_lsn) {
    ControlledWriter cw;
    cw.fail_sync = true;
    cppio::wal::WalWriter w(cw, &cw);
    auto payload = bytes_of("x");
    CPPIO_CHECK(w.write_record(span_of(payload)).has_value());
    auto s = w.sync();
    CPPIO_CHECK(!s.has_value());
    // sync() flushes first (succeeds) -> flushed advances, durable does not.
    CPPIO_CHECK(w.flushed_lsn() == 13);
    CPPIO_CHECK(w.durable_lsn() == 0);
    CPPIO_CHECK(invariant(w));
}

CPPIO_TEST_CASE(wal_writer_vector_write_path_works) {
    cppio::MemoryWriter sink;
    cppio::wal::WalWriter w(sink);
    auto payload = bytes_of("vector record");
    CPPIO_CHECK(w.write_record_vec(span_of(payload)).has_value());
    // 8 + 13 + 4 = 25
    CPPIO_CHECK(w.written_lsn() == 25);
    CPPIO_CHECK(w.flush().has_value());
    CPPIO_CHECK(w.flushed_lsn() == 25);

    // Round-trips through the scalar reader.
    cppio::MemoryReader in(sink.bytes());
    auto res = cppio::wal::read_record(in);
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value().size() == 13);
    CPPIO_CHECK(std::memcmp(res.value().data(), "vector record", 13) == 0);
    CPPIO_CHECK(invariant(w));
}

CPPIO_TEST_CASE(wal_writer_invariant_holds_after_mixed_operations) {
    ControlledWriter cw;
    cppio::wal::WalWriter w(cw, &cw);
    auto a = bytes_of("aaa");
    auto b = bytes_of("bbbb");
    CPPIO_CHECK(w.write_record(span_of(a)).has_value());   // 8+3+4=15
    CPPIO_CHECK(invariant(w));
    CPPIO_CHECK(w.write_record_vec(span_of(b)).has_value()); // +8+4+4=+16 -> 31
    CPPIO_CHECK(invariant(w));
    CPPIO_CHECK(w.flush().has_value());
    CPPIO_CHECK(invariant(w));
    CPPIO_CHECK(w.sync().has_value());
    CPPIO_CHECK(invariant(w));
    CPPIO_CHECK(w.written_lsn() == 31);
    CPPIO_CHECK(w.flushed_lsn() == 31);
    CPPIO_CHECK(w.durable_lsn() == 31);
    // One more write after sync: written pulls ahead, invariant still holds.
    CPPIO_CHECK(w.write_record(span_of(a)).has_value());
    CPPIO_CHECK(invariant(w));
    CPPIO_CHECK(w.written_lsn() == 46);
    CPPIO_CHECK(w.flushed_lsn() == 31);
    CPPIO_CHECK(w.durable_lsn() == 31);
}

CPPIO_MAIN()
