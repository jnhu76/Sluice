// copy_strategy_bench (CPPIO-CORE-010D). Measures Scratch / BufferedFirst / Auto
// across input sizes and limit cases. CSV to stdout. NOT a performance claim.
#include "bench_common.hpp"

#include <sluice/buffer.hpp>
#include <sluice/copy.hpp>
#include <sluice/copy_strategy.hpp>
#include <sluice/fault.hpp>
#include <sluice/limit.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

// A reader over an in-memory payload, also BufferedReadable via BufferedReader.
class MemReader final : public sluice::Reader {
  public:
    sluice::MemoryReader mem;
    explicit MemReader(std::string_view s) : mem(sluice::MemoryReader::from_string(s)) {}
    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        return mem.read_some(dst);
    }
};

void run_cell(std::size_t total, sluice::CopyStrategy strategy, const char* strat_name,
              sluice::CopyLimit limit, const char* limit_name) {
    std::string payload(total, 'X');
    std::vector<std::byte> rbuf(64 * 1024);
    std::vector<std::byte> scratch(64 * 1024);
    sluice::MemoryWriter out;

    sluice::CopyStats st;
    sluice::BufferStats bs;
    // warmup
    for (int i = 0; i < 1; ++i) {
        MemReader rd(payload);
        sluice::BufferedReader br(rd, rbuf, &bs);
        sluice::MemoryWriter wo;
        sluice::CopyOptions o;
        o.strategy = strategy;
        o.limit = limit;
        (void)sluice::copy_all(br, wo, std::span<std::byte>(scratch), o);
    }
    st = {};
    bs = {};
    MemReader rd(payload);
    sluice::BufferedReader br(rd, rbuf, &bs);
    sluice::MemoryWriter wo;
    sluice::CopyOptions o;
    o.strategy = strategy;
    o.limit = limit;
    auto t0 = now_ns();
    auto res = sluice::copy_all(br, wo, std::span<std::byte>(scratch), o, &st);
    auto elapsed = now_ns() - t0;
    if (!res.has_value())
        return;

    sluice::bench::BenchResult r;
    r.case_name = "copy_strategy";
    r.mode = std::string(strat_name) + "/" + limit_name;
    r.bytes = res.value();
    r.iterations = st.copy_loop_iterations;
    r.elapsed_ns = elapsed;
    r.copy_stats = st;
    r.buffer_stats = bs;
    sluice::bench::print_csv_row(std::cout, r);
}

} // namespace

int main() {
    sluice::bench::print_csv_header(std::cout);
    for (std::size_t total : {4u * 1024u, 64u * 1024u, 1024u * 1024u, 16u * 1024u * 1024u}) {
        run_cell(total, sluice::CopyStrategy::Scratch, "scratch", sluice::CopyLimit::unlimited(),
                 "unlimited");
        run_cell(total, sluice::CopyStrategy::BufferedFirst, "buffered_first",
                 sluice::CopyLimit::unlimited(), "unlimited");
        run_cell(total, sluice::CopyStrategy::Auto, "auto", sluice::CopyLimit::unlimited(),
                 "unlimited");
        // small + exact limit cases
        run_cell(total, sluice::CopyStrategy::BufferedFirst, "buffered_first",
                 sluice::CopyLimit::bytes(total / 4), "small_limit");
        run_cell(total, sluice::CopyStrategy::BufferedFirst, "buffered_first",
                 sluice::CopyLimit::bytes(total), "exact_limit");
    }
    return 0;
}
