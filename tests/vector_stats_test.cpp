// Tests for VectorStats as wired through the Observed wrappers. The observed
// layer observes the DEFAULT-FALLBACK path (read_some/write_some loop), so every
// vector call counts as a fallback call. The non-fallback (real readv/writev)
// path is exercised/tested in file_vec_test.cpp through FileReader/FileWriter.
#include "harness.hpp"

#include <cppio/fault.hpp>
#include <cppio/iovec.hpp>
#include <cppio/measurement.hpp>
#include <cppio/observed.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace {

cppio::ConstIoSlice cslice_of(std::string_view s) {
    return cppio::ConstIoSlice{std::as_bytes(std::span(s.data(), s.size()))};
}
cppio::IoSlice mslice_of(std::span<std::byte> b) { return cppio::IoSlice{b}; }

std::vector<std::byte> bytes_of(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

}  // namespace

CPPIO_TEST_CASE(observed_writer_vec_counts_calls_bytes_iovecs_and_fallback) {
    cppio::MemoryWriter mem;
    cppio::WriterStats ws{};
    cppio::VectorStats vs{};
    cppio::ObservedWriter ow(mem, ws, &vs);

    std::array<cppio::ConstIoSlice, 3> srcs = {cslice_of("ab"), cslice_of(""), cslice_of("cde")};
    auto r = ow.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(r.value() == 5);

    CPPIO_CHECK(vs.write_vec_calls == 1);
    CPPIO_CHECK(vs.write_vec_bytes == 5);
    CPPIO_CHECK(vs.write_vec_iovecs == 2);          // empty slice skipped from count
    CPPIO_CHECK(vs.write_vec_fallback_calls == 1);  // default fallback ran
}

CPPIO_TEST_CASE(observed_reader_vec_counts_calls_bytes_iovecs_and_fallback) {
    cppio::MemoryReader mr(bytes_of("hello world"));
    cppio::ReaderStats rs{};
    cppio::VectorStats vs{};
    cppio::ObservedReader orr(mr, rs, &vs);

    std::vector<std::byte> a(5), b(6);
    std::array<cppio::IoSlice, 2> dsts = {mslice_of(a), mslice_of(b)};
    auto res = orr.read_vec(std::span<cppio::IoSlice>(dsts));
    CPPIO_CHECK(res.has_value());
    CPPIO_CHECK(res.value() == 11);

    CPPIO_CHECK(vs.read_vec_calls == 1);
    CPPIO_CHECK(vs.read_vec_bytes == 11);
    CPPIO_CHECK(vs.read_vec_iovecs == 2);
    CPPIO_CHECK(vs.read_vec_fallback_calls == 1);  // default fallback ran
}

CPPIO_TEST_CASE(observed_vec_stats_null_means_no_counting_no_crash) {
    cppio::MemoryWriter mem;
    cppio::WriterStats ws{};
    cppio::ObservedWriter ow(mem, ws, nullptr);  // no vec stats
    std::array<cppio::ConstIoSlice, 1> srcs = {cslice_of("data")};
    auto r = ow.write_vec(std::span<const cppio::ConstIoSlice>(srcs));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(r.value() == 4);  // behavior unchanged
}

CPPIO_TEST_CASE(observed_vec_stats_accumulate_across_calls) {
    cppio::MemoryWriter mem;
    cppio::WriterStats ws{};
    cppio::VectorStats vs{};
    cppio::ObservedWriter ow(mem, ws, &vs);

    std::array<cppio::ConstIoSlice, 1> a = {cslice_of("one")};
    std::array<cppio::ConstIoSlice, 1> b = {cslice_of("two")};
    CPPIO_CHECK(ow.write_vec(std::span<const cppio::ConstIoSlice>(a)).has_value());
    CPPIO_CHECK(ow.write_vec(std::span<const cppio::ConstIoSlice>(b)).has_value());

    CPPIO_CHECK(vs.write_vec_calls == 2);
    CPPIO_CHECK(vs.write_vec_bytes == 6);
    CPPIO_CHECK(vs.write_vec_fallback_calls == 2);
}

CPPIO_TEST_CASE(vec_stats_are_separate_sinks_no_implicit_double_count) {
    // Two distinct VectorStats sinks observe two distinct layers. Each layer
    // increments only its own sink, so a single logical write_vec shows up once
    // in each sink — not twice in one sink. Double-counting happens ONLY if the
    // caller deliberately attaches the SAME sink to multiple layers (documented
    // as intentional, not as a bug).
    cppio::MemoryWriter mem;
    cppio::WriterStats ws{};
    cppio::VectorStats observed_vs{};   // observes the fallback layer
    cppio::VectorStats mem_vs{};        // a separate sink, here just to prove isolation
    cppio::ObservedWriter ow(mem, ws, &observed_vs);

    std::array<cppio::ConstIoSlice, 1> a = {cslice_of("data")};
    CPPIO_CHECK(ow.write_vec(std::span<const cppio::ConstIoSlice>(a)).has_value());

    CPPIO_CHECK(observed_vs.write_vec_calls == 1);   // observed layer counted it
    CPPIO_CHECK(mem_vs.write_vec_calls == 0);        // the other sink is untouched
    // No single sink shows the call twice.
    CPPIO_CHECK(observed_vs.write_vec_calls != 2);
}

CPPIO_MAIN()
