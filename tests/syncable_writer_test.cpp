// Tests for the SyncableWriter capability interface (CPPIO-CORE-008B). Verifies
// the interface compiles, is detectable via dynamic_cast, and is NOT forced onto
// writers that have no sync concept.
#include "harness.hpp"

#include <cppio/fault.hpp>
#include <cppio/sync.hpp>
#include <cppio/writer.hpp>

namespace {

// A writer that opts into SyncableWriter.
class SyncableMemoryWriter final : public cppio::Writer, public cppio::SyncableWriter {
public:
    int sync_data_calls = 0;
    int sync_all_calls = 0;
    cppio::Result<std::size_t> write_some(std::span<const std::byte> src) override {
        return src.size();
    }
    cppio::Result<void> flush() override { return {}; }
    cppio::Result<void> sync_data() override {
        ++sync_data_calls;
        return {};
    }
    cppio::Result<void> sync_all() override {
        ++sync_all_calls;
        return {};
    }
};

}  // namespace

CPPIO_TEST_CASE(syncable_writer_interface_compiles_and_is_abstract) {
    // SyncableWriter is abstract (pure virtuals); the concrete subclass works.
    SyncableMemoryWriter w;
    CPPIO_CHECK(w.sync_data().has_value());
    CPPIO_CHECK(w.sync_all().has_value());
    CPPIO_CHECK(w.sync_data_calls == 1);
    CPPIO_CHECK(w.sync_all_calls == 1);
}

CPPIO_TEST_CASE(syncable_writer_detectable_via_dynamic_cast) {
    SyncableMemoryWriter w;
    cppio::Writer& as_writer = w;
    auto* cap = dynamic_cast<cppio::SyncableWriter*>(&as_writer);
    CPPIO_CHECK(cap != nullptr);
}

CPPIO_TEST_CASE(existing_writers_not_forced_to_implement_sync) {
    // A plain MemoryWriter is a Writer but NOT SyncableWriter: no forced impl.
    cppio::MemoryWriter mw;
    cppio::Writer& as_writer = mw;
    auto* cap = dynamic_cast<cppio::SyncableWriter*>(&as_writer);
    CPPIO_CHECK(cap == nullptr);  // capability absent, not forced
}

CPPIO_MAIN()
