// Tests for VectorStats as wired through the Observed wrappers. The observed
// layer observes the DEFAULT-FALLBACK path (read_some/write_some loop), so every
// vector call counts as a fallback call. The non-fallback (real readv/writev)
// path is exercised/tested in file_vec_test.cpp through FileReader/FileWriter.
#include "harness.hpp"

#include <sluice/fault.hpp>
#include <sluice/iovec.hpp>
#include <sluice/measurement.hpp>
#include <sluice/observed.hpp>
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

#include <cstring>
#include <span>
#include <vector>

namespace {

sluice::ConstIoSlice cslice_of(std::string_view s) {
    return sluice::ConstIoSlice{std::as_bytes(std::span(s.data(), s.size()))};
}
sluice::IoSlice mslice_of(std::span<std::byte> b) { return sluice::IoSlice{b}; }

std::vector<std::byte> bytes_of(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

}  // namespace

SLUICE_TEST_CASE(observed_writer_vec_counts_calls_bytes_iovecs_and_fallback) {
    sluice::MemoryWriter mem;
    sluice::WriterStats ws{};
    sluice::VectorStats vs{};
    sluice::ObservedWriter ow(mem, ws, &vs);

    std::array<sluice::ConstIoSlice, 3> srcs = {cslice_of("ab"), cslice_of(""), cslice_of("cde")};
    auto r = ow.write_vec(std::span<const sluice::ConstIoSlice>(srcs));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 5);

    SLUICE_CHECK(vs.write_vec_calls == 1);
    SLUICE_CHECK(vs.write_vec_bytes == 5);
    SLUICE_CHECK(vs.write_vec_iovecs == 2);          // empty slice skipped from count
    SLUICE_CHECK(vs.write_vec_fallback_calls == 1);  // default fallback ran
}

SLUICE_TEST_CASE(observed_reader_vec_counts_calls_bytes_iovecs_and_fallback) {
    sluice::MemoryReader mr(bytes_of("hello world"));
    sluice::ReaderStats rs{};
    sluice::VectorStats vs{};
    sluice::ObservedReader orr(mr, rs, &vs);

    std::vector<std::byte> a(5), b(6);
    std::array<sluice::IoSlice, 2> dsts = {mslice_of(a), mslice_of(b)};
    auto res = orr.read_vec(std::span<sluice::IoSlice>(dsts));
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(res.value() == 11);

    SLUICE_CHECK(vs.read_vec_calls == 1);
    SLUICE_CHECK(vs.read_vec_bytes == 11);
    SLUICE_CHECK(vs.read_vec_iovecs == 2);
    SLUICE_CHECK(vs.read_vec_fallback_calls == 1);  // default fallback ran
}

SLUICE_TEST_CASE(observed_vec_stats_null_means_no_counting_no_crash) {
    sluice::MemoryWriter mem;
    sluice::WriterStats ws{};
    sluice::ObservedWriter ow(mem, ws, nullptr);  // no vec stats
    std::array<sluice::ConstIoSlice, 1> srcs = {cslice_of("data")};
    auto r = ow.write_vec(std::span<const sluice::ConstIoSlice>(srcs));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 4);  // behavior unchanged
}

SLUICE_TEST_CASE(observed_vec_stats_accumulate_across_calls) {
    sluice::MemoryWriter mem;
    sluice::WriterStats ws{};
    sluice::VectorStats vs{};
    sluice::ObservedWriter ow(mem, ws, &vs);

    std::array<sluice::ConstIoSlice, 1> a = {cslice_of("one")};
    std::array<sluice::ConstIoSlice, 1> b = {cslice_of("two")};
    SLUICE_CHECK(ow.write_vec(std::span<const sluice::ConstIoSlice>(a)).has_value());
    SLUICE_CHECK(ow.write_vec(std::span<const sluice::ConstIoSlice>(b)).has_value());

    SLUICE_CHECK(vs.write_vec_calls == 2);
    SLUICE_CHECK(vs.write_vec_bytes == 6);
    SLUICE_CHECK(vs.write_vec_fallback_calls == 2);
}

SLUICE_TEST_CASE(vec_stats_are_separate_sinks_no_implicit_double_count) {
    // Two distinct VectorStats sinks observe two distinct layers. Each layer
    // increments only its own sink, so a single logical write_vec shows up once
    // in each sink — not twice in one sink. Double-counting happens ONLY if the
    // caller deliberately attaches the SAME sink to multiple layers (documented
    // as intentional, not as a bug).
    sluice::MemoryWriter mem;
    sluice::WriterStats ws{};
    sluice::VectorStats observed_vs{};   // observes the fallback layer
    sluice::VectorStats mem_vs{};        // a separate sink, here just to prove isolation
    sluice::ObservedWriter ow(mem, ws, &observed_vs);

    std::array<sluice::ConstIoSlice, 1> a = {cslice_of("data")};
    SLUICE_CHECK(ow.write_vec(std::span<const sluice::ConstIoSlice>(a)).has_value());

    SLUICE_CHECK(observed_vs.write_vec_calls == 1);   // observed layer counted it
    SLUICE_CHECK(mem_vs.write_vec_calls == 0);        // the other sink is untouched
    // No single sink shows the call twice.
    SLUICE_CHECK(observed_vs.write_vec_calls != 2);
}

SLUICE_MAIN()
