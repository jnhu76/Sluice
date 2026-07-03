// Tests for MemoryIoContext (CPPIO-CORE-015C) — a deterministic in-memory
// IoContext for tests/examples. Seeded paths open as MemoryReader; open_writer
// returns a fresh independent MemoryWriter. Does not change BlockingIoContext
// or default backend selection.
#include "harness.hpp"

#include <cppio/fault.hpp>
#include <cppio/io_context.hpp>
#include <cppio/memory_io_context.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace {
std::vector<std::byte> to_bytes(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}
}  // namespace

CPPIO_TEST_CASE(memory_io_context_open_reader_succeeds_for_seeded_path) {
    cppio::MemoryIoContext ctx;
    ctx.seed("a", to_bytes("hello"));
    auto r = ctx.open_reader("a");
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(r.value() != nullptr);
    std::byte buf[5];
    auto rr = r.value()->read_some(buf);
    CPPIO_CHECK(rr.has_value() && rr.value() == 5);
    CPPIO_CHECK(std::memcmp(buf, "hello", 5) == 0);
}

CPPIO_TEST_CASE(memory_io_context_open_writer_succeeds) {
    cppio::MemoryIoContext ctx;
    auto w = ctx.open_writer("out");
    CPPIO_CHECK(w.has_value());
    CPPIO_CHECK(w.value() != nullptr);
    auto buf = to_bytes("data");
    auto wr = w.value()->write_some(std::span<const std::byte>(buf));
    CPPIO_CHECK(wr.has_value() && wr.value() == 4);
}

CPPIO_TEST_CASE(memory_io_context_open_reader_missing_path_errors) {
    cppio::MemoryIoContext ctx;
    auto r = ctx.open_reader("nope");
    CPPIO_CHECK(!r.has_value());
}

CPPIO_TEST_CASE(memory_io_context_reader_handles_are_independent) {
    cppio::MemoryIoContext ctx;
    ctx.seed("k", to_bytes("shared?"));
    auto r1 = ctx.open_reader("k");
    auto r2 = ctx.open_reader("k");
    CPPIO_CHECK(r1.has_value() && r2.has_value());
    std::byte b1[7], b2[7];
    (void)r1.value()->read_some(b1);
    (void)r2.value()->read_some(b2);
    CPPIO_CHECK(std::memcmp(b1, "shared?", 7) == 0);
    CPPIO_CHECK(std::memcmp(b2, "shared?", 7) == 0);
}

CPPIO_TEST_CASE(memory_io_context_writers_are_independent_objects) {
    cppio::MemoryIoContext ctx;
    auto w1 = ctx.open_writer("w");
    auto w2 = ctx.open_writer("w");
    CPPIO_CHECK(w1.has_value() && w2.has_value());
    (void)w1.value()->write_some(std::span<const std::byte>(to_bytes("abc")));
    (void)w2.value()->write_some(std::span<const std::byte>(to_bytes("XYZ")));
    auto* mw1 = dynamic_cast<cppio::MemoryWriter*>(w1.value().get());
    auto* mw2 = dynamic_cast<cppio::MemoryWriter*>(w2.value().get());
    CPPIO_CHECK(mw1 != nullptr && mw2 != nullptr);
    CPPIO_CHECK(mw1->bytes().size() == 3);
    CPPIO_CHECK(mw2->bytes().size() == 3);
    CPPIO_CHECK(std::memcmp(mw1->bytes().data(), "abc", 3) == 0);
    CPPIO_CHECK(std::memcmp(mw2->bytes().data(), "XYZ", 3) == 0);
}

CPPIO_TEST_CASE(memory_io_context_is_an_io_context) {
    cppio::MemoryIoContext ctx;
    cppio::IoContext& as_base = ctx;
    ctx.seed("p", to_bytes("x"));
    auto r = as_base.open_reader("p");
    CPPIO_CHECK(r.has_value());
}

CPPIO_MAIN()
