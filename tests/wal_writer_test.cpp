// Tests for wal::WalWriter durability wrapper (CPPIO-CORE-008E). Verifies the
// three LSNs and the invariant durable <= flushed <= written across
// write/flush/sync, failure paths, and the vector write path.
#include "harness.hpp"

#include <sluice/fault.hpp>
#include <sluice/wal.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace {

bool invariant(const sluice::wal::WalWriter& w) {
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
class ControlledWriter final : public sluice::Writer, public sluice::SyncableWriter {
public:
    std::vector<std::byte> sink;
    bool fail_write = false;
    bool fail_flush = false;
    bool fail_sync = false;
    int flush_calls = 0;
    int sync_data_calls = 0;

    sluice::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        if (fail_write) {
            return sluice::make_unexpected<std::size_t>(
                sluice::IoError{sluice::IoError::Code::no_space});
        }
        sink.insert(sink.end(), src.begin(), src.end());
        return src.size();
    }
    sluice::Result<void> flush() override {
        ++flush_calls;
        if (fail_flush) {
            return sluice::make_unexpected<void>(
                sluice::IoError{sluice::IoError::Code::backend_error});
        }
        return {};
    }
    sluice::Result<void> sync_data() override {
        ++sync_data_calls;
        if (fail_sync) {
            return sluice::make_unexpected<void>(
                sluice::IoError{sluice::IoError::Code::backend_error});
        }
        return {};
    }
    sluice::Result<void> sync_all() override { return sync_data(); }
};

}  // namespace

SLUICE_TEST_CASE(wal_writer_initial_lsns_all_zero) {
    sluice::MemoryWriter sink;
    sluice::wal::WalWriter w(sink);
    SLUICE_CHECK(w.written_lsn() == 0);
    SLUICE_CHECK(w.flushed_lsn() == 0);
    SLUICE_CHECK(w.durable_lsn() == 0);
    SLUICE_CHECK(invariant(w));
}

SLUICE_TEST_CASE(wal_writer_write_advances_only_written_lsn) {
    sluice::MemoryWriter sink;
    sluice::wal::WalWriter w(sink);
    auto payload = bytes_of("hello");
    SLUICE_CHECK(w.write_record(span_of(payload)).has_value());
    // framed size = 8 + 5 + 4 = 17
    SLUICE_CHECK(w.written_lsn() == 17);
    SLUICE_CHECK(w.flushed_lsn() == 0);
    SLUICE_CHECK(w.durable_lsn() == 0);
    SLUICE_CHECK(invariant(w));
}

SLUICE_TEST_CASE(wal_writer_flush_advances_flushed_not_durable) {
    sluice::MemoryWriter sink;
    sluice::wal::WalWriter w(sink);
    auto payload = bytes_of("hello");
    SLUICE_CHECK(w.write_record(span_of(payload)).has_value());
    SLUICE_CHECK(w.flush().has_value());
    SLUICE_CHECK(w.flushed_lsn() == 17);
    SLUICE_CHECK(w.durable_lsn() == 0);  // not synced yet
    SLUICE_CHECK(invariant(w));
}

SLUICE_TEST_CASE(wal_writer_sync_advances_durable_after_flush_and_sync) {
    ControlledWriter cw;
    sluice::wal::WalWriter w(cw, &cw);  // writer IS the SyncableWriter
    auto payload = bytes_of("data");
    SLUICE_CHECK(w.write_record(span_of(payload)).has_value());  // 8+4+4=16
    SLUICE_CHECK(w.sync().has_value());
    SLUICE_CHECK(w.flushed_lsn() == 16);
    SLUICE_CHECK(w.durable_lsn() == 16);
    SLUICE_CHECK(cw.sync_data_calls == 1);
    SLUICE_CHECK(invariant(w));
}

SLUICE_TEST_CASE(wal_writer_sync_without_syncable_returns_invalid_state) {
    sluice::MemoryWriter sink;
    sluice::wal::WalWriter w(sink);  // no SyncableWriter
    auto payload = bytes_of("data");
    SLUICE_CHECK(w.write_record(span_of(payload)).has_value());
    auto s = w.sync();
    SLUICE_CHECK(!s.has_value());
    SLUICE_CHECK(s.error().code == sluice::IoError::Code::invalid_state);
    // flush still succeeded before the sync attempt, so flushed advanced.
    SLUICE_CHECK(w.flushed_lsn() == 16);
    SLUICE_CHECK(w.durable_lsn() == 0);
    SLUICE_CHECK(invariant(w));
}

SLUICE_TEST_CASE(wal_writer_write_failure_does_not_advance_written_lsn) {
    ControlledWriter cw;
    cw.fail_write = true;
    sluice::wal::WalWriter w(cw, &cw);
    auto payload = bytes_of("nope");
    auto r = w.write_record(span_of(payload));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(w.written_lsn() == 0);  // unchanged on failure
    SLUICE_CHECK(invariant(w));
}

SLUICE_TEST_CASE(wal_writer_flush_failure_does_not_advance_flushed_lsn) {
    ControlledWriter cw;
    cw.fail_flush = true;
    sluice::wal::WalWriter w(cw, &cw);
    auto payload = bytes_of("x");
    SLUICE_CHECK(w.write_record(span_of(payload)).has_value());  // written=13
    auto f = w.flush();
    SLUICE_CHECK(!f.has_value());
    SLUICE_CHECK(w.flushed_lsn() == 0);  // unchanged on failure
    SLUICE_CHECK(w.written_lsn() == 13);
    SLUICE_CHECK(invariant(w));
}

SLUICE_TEST_CASE(wal_writer_sync_failure_does_not_advance_durable_lsn) {
    ControlledWriter cw;
    cw.fail_sync = true;
    sluice::wal::WalWriter w(cw, &cw);
    auto payload = bytes_of("x");
    SLUICE_CHECK(w.write_record(span_of(payload)).has_value());
    auto s = w.sync();
    SLUICE_CHECK(!s.has_value());
    // sync() flushes first (succeeds) -> flushed advances, durable does not.
    SLUICE_CHECK(w.flushed_lsn() == 13);
    SLUICE_CHECK(w.durable_lsn() == 0);
    SLUICE_CHECK(invariant(w));
}

SLUICE_TEST_CASE(wal_writer_vector_write_path_works) {
    sluice::MemoryWriter sink;
    sluice::wal::WalWriter w(sink);
    auto payload = bytes_of("vector record");
    SLUICE_CHECK(w.write_record_vec(span_of(payload)).has_value());
    // 8 + 13 + 4 = 25
    SLUICE_CHECK(w.written_lsn() == 25);
    SLUICE_CHECK(w.flush().has_value());
    SLUICE_CHECK(w.flushed_lsn() == 25);

    // Round-trips through the scalar reader.
    sluice::MemoryReader in(sink.bytes());
    auto res = sluice::wal::read_record(in);
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value().size() == 13);
    SLUICE_CHECK(std::memcmp(res.value().data(), "vector record", 13) == 0);
    SLUICE_CHECK(invariant(w));
}

SLUICE_TEST_CASE(wal_writer_invariant_holds_after_mixed_operations) {
    ControlledWriter cw;
    sluice::wal::WalWriter w(cw, &cw);
    auto a = bytes_of("aaa");
    auto b = bytes_of("bbbb");
    SLUICE_CHECK(w.write_record(span_of(a)).has_value());   // 8+3+4=15
    SLUICE_CHECK(invariant(w));
    SLUICE_CHECK(w.write_record_vec(span_of(b)).has_value()); // +8+4+4=+16 -> 31
    SLUICE_CHECK(invariant(w));
    SLUICE_CHECK(w.flush().has_value());
    SLUICE_CHECK(invariant(w));
    SLUICE_CHECK(w.sync().has_value());
    SLUICE_CHECK(invariant(w));
    SLUICE_CHECK(w.written_lsn() == 31);
    SLUICE_CHECK(w.flushed_lsn() == 31);
    SLUICE_CHECK(w.durable_lsn() == 31);
    // One more write after sync: written pulls ahead, invariant still holds.
    SLUICE_CHECK(w.write_record(span_of(a)).has_value());
    SLUICE_CHECK(invariant(w));
    SLUICE_CHECK(w.written_lsn() == 46);
    SLUICE_CHECK(w.flushed_lsn() == 31);
    SLUICE_CHECK(w.durable_lsn() == 31);
}

SLUICE_MAIN()
