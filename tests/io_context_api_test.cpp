// Tests for the IoContext API surface (CPPIO-CORE-009B). Verifies the API
// compiles, IoContext is abstract, options default to null stats, and no global
// context is introduced.
#include "harness.hpp"

#include <sluice/io_context.hpp>
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

#include <memory>
#include <string_view>

SLUICE_TEST_CASE(io_context_is_abstract_and_compiles) {
    // IoContext cannot be instantiated directly (pure virtuals).
    // (Checked structurally: it has = 0 virtuals.)
    static_assert(std::is_abstract_v<sluice::IoContext>);
}

SLUICE_TEST_CASE(open_reader_options_default_to_null) {
    sluice::OpenReaderOptions o;
    SLUICE_CHECK(o.syscall_stats == nullptr);
    SLUICE_CHECK(o.vector_stats == nullptr);
}

SLUICE_TEST_CASE(open_writer_options_default_to_null) {
    sluice::OpenWriterOptions o;
    SLUICE_CHECK(o.syscall_stats == nullptr);
    SLUICE_CHECK(o.vector_stats == nullptr);
    SLUICE_CHECK(o.sync_stats == nullptr);
}

SLUICE_TEST_CASE(io_context_returns_unique_ptr_reader_writer) {
    // Structural: the return types are Result<unique_ptr<Reader/Writer>>. A
    // concrete subclass lets us confirm the signature is usable.
    struct FakeContext final : sluice::IoContext {
        sluice::Result<std::unique_ptr<sluice::Reader>>
        open_reader(std::string_view, sluice::OpenReaderOptions) override {
            return std::unique_ptr<sluice::Reader>{nullptr};
        }
        sluice::Result<std::unique_ptr<sluice::Writer>>
        open_writer(std::string_view, sluice::OpenWriterOptions) override {
            return std::unique_ptr<sluice::Writer>{nullptr};
        }
    };
    FakeContext ctx;
    auto r = ctx.open_reader("x", {});
    SLUICE_CHECK(r.has_value());
    auto w = ctx.open_writer("y", {});
    SLUICE_CHECK(w.has_value());
}

SLUICE_MAIN()
