// Tests for MemoryReader convenience constructors (CPPIO-CORE-015B). The new
// from_bytes(span) factory mirrors from_string and removes the reinterpret_cast
// boilerplate seen across tests/examples. Ownership: all factories COPY into the
// reader's owned storage, so the caller's buffer need not outlive the reader.
#include "harness.hpp"

#include <sluice/fault.hpp>

#include <cstring>
#include <span>
#include <string>
#include <vector>

SLUICE_TEST_CASE(memory_reader_default_constructs_empty) {
    sluice::MemoryReader rd;
    std::byte buf[4];
    auto r = rd.read_some(buf);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 0); // EOF immediately
    SLUICE_CHECK(rd.remaining() == 0);
}

SLUICE_TEST_CASE(memory_reader_from_string_copies_bytes) {
    std::string src = "hello";
    auto rd = sluice::MemoryReader::from_string(src);
    SLUICE_CHECK(rd.remaining() == 5);
    // Mutating the source after construction must not affect the reader.
    src[0] = 'X';
    std::byte buf[5];
    (void)rd.read_some(buf);
    SLUICE_CHECK(std::to_integer<char>(buf[0]) == 'h'); // independent copy
}

SLUICE_TEST_CASE(memory_reader_from_bytes_copies_span) {
    std::vector<std::byte> src(4, std::byte{0x7A});
    auto rd = sluice::MemoryReader::from_bytes(std::span<const std::byte>(src));
    SLUICE_CHECK(rd.remaining() == 4);
    // Mutating the source after construction must not affect the reader.
    src[0] = std::byte{0x00};
    std::byte buf[4];
    (void)rd.read_some(buf);
    SLUICE_CHECK(buf[0] == std::byte{0x7A}); // independent copy
}

SLUICE_TEST_CASE(memory_reader_from_bytes_empty_span) {
    auto rd = sluice::MemoryReader::from_bytes(std::span<const std::byte>{});
    SLUICE_CHECK(rd.remaining() == 0);
    std::byte buf[1];
    auto r = rd.read_some(buf);
    SLUICE_CHECK(r.has_value() && r.value() == 0);
}

SLUICE_TEST_CASE(memory_reader_from_string_temporary_is_safe) {
    // A factory call with a string temporary must not dangle: the reader owns
    // its own copy, so this is safe even though the temporary is gone.
    sluice::MemoryReader rd = sluice::MemoryReader::from_string(std::string("temp"));
    std::byte buf[4];
    auto r = rd.read_some(buf);
    SLUICE_CHECK(r.has_value() && r.value() == 4);
    SLUICE_CHECK(std::memcmp(buf, "temp", 4) == 0);
}

SLUICE_TEST_CASE(memory_reader_vector_ctor_moves) {
    std::vector<std::byte> src = {std::byte{1}, std::byte{2}, std::byte{3}};
    sluice::MemoryReader rd(std::move(src));
    SLUICE_CHECK(rd.remaining() == 3);
    std::byte buf[3];
    (void)rd.read_some(buf);
    SLUICE_CHECK(buf[0] == std::byte{1});
}

SLUICE_MAIN()
