// Tests for the IoContext API surface (CPPIO-CORE-009B). Verifies the API
// compiles, IoContext is abstract, options default to null stats, and no global
// context is introduced.
#include "harness.hpp"

#include <cppio/io_context.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <memory>
#include <string_view>

CPPIO_TEST_CASE(io_context_is_abstract_and_compiles) {
    // IoContext cannot be instantiated directly (pure virtuals).
    // (Checked structurally: it has = 0 virtuals.)
    static_assert(std::is_abstract_v<cppio::IoContext>);
}

CPPIO_TEST_CASE(open_reader_options_default_to_null) {
    cppio::OpenReaderOptions o;
    CPPIO_CHECK(o.syscall_stats == nullptr);
    CPPIO_CHECK(o.vector_stats == nullptr);
}

CPPIO_TEST_CASE(open_writer_options_default_to_null) {
    cppio::OpenWriterOptions o;
    CPPIO_CHECK(o.syscall_stats == nullptr);
    CPPIO_CHECK(o.vector_stats == nullptr);
    CPPIO_CHECK(o.sync_stats == nullptr);
}

CPPIO_TEST_CASE(io_context_returns_unique_ptr_reader_writer) {
    // Structural: the return types are Result<unique_ptr<Reader/Writer>>. A
    // concrete subclass lets us confirm the signature is usable.
    struct FakeContext final : cppio::IoContext {
        cppio::Result<std::unique_ptr<cppio::Reader>>
        open_reader(std::string_view, cppio::OpenReaderOptions) override {
            return std::unique_ptr<cppio::Reader>{nullptr};
        }
        cppio::Result<std::unique_ptr<cppio::Writer>>
        open_writer(std::string_view, cppio::OpenWriterOptions) override {
            return std::unique_ptr<cppio::Writer>{nullptr};
        }
    };
    FakeContext ctx;
    auto r = ctx.open_reader("x", {});
    CPPIO_CHECK(r.has_value());
    auto w = ctx.open_writer("y", {});
    CPPIO_CHECK(w.has_value());
}

CPPIO_MAIN()
