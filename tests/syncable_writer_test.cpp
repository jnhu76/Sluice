// Tests for the SyncableWriter capability interface (CPPIO-CORE-008B). Verifies
// the interface compiles, is detectable via dynamic_cast, and is NOT forced onto
// writers that have no sync concept.
#include "harness.hpp"

#include <sluice/fault.hpp>
#include <sluice/sync.hpp>
#include <sluice/writer.hpp>

namespace {

// A writer that opts into SyncableWriter.
class SyncableMemoryWriter final : public sluice::Writer, public sluice::SyncableWriter {
public:
    int sync_data_calls = 0;
    int sync_all_calls = 0;
    sluice::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        return src.size();
    }
    sluice::Result<void> flush() override { return {}; }
    sluice::Result<void> sync_data() override {
        ++sync_data_calls;
        return {};
    }
    sluice::Result<void> sync_all() override {
        ++sync_all_calls;
        return {};
    }
};

}  // namespace

SLUICE_TEST_CASE(syncable_writer_interface_compiles_and_is_abstract) {
    // SyncableWriter is abstract (pure virtuals); the concrete subclass works.
    SyncableMemoryWriter w;
    SLUICE_CHECK(w.sync_data().has_value());
    SLUICE_CHECK(w.sync_all().has_value());
    SLUICE_CHECK(w.sync_data_calls == 1);
    SLUICE_CHECK(w.sync_all_calls == 1);
}

SLUICE_TEST_CASE(syncable_writer_detectable_via_dynamic_cast) {
    SyncableMemoryWriter w;
    sluice::Writer& as_writer = w;
    auto* cap = dynamic_cast<sluice::SyncableWriter*>(&as_writer);
    SLUICE_CHECK(cap != nullptr);
}

SLUICE_TEST_CASE(existing_writers_not_forced_to_implement_sync) {
    // A plain MemoryWriter is a Writer but NOT SyncableWriter: no forced impl.
    sluice::MemoryWriter mw;
    sluice::Writer& as_writer = mw;
    auto* cap = dynamic_cast<sluice::SyncableWriter*>(&as_writer);
    SLUICE_CHECK(cap == nullptr);  // capability absent, not forced
}

SLUICE_MAIN()
