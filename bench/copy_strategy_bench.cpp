// copy_strategy_bench (CPPIO-CORE-010D). Measures Scratch / BufferedFirst / Auto
// across input sizes and limit cases. CSV to stdout. NOT a performance claim.
#include "bench_common.hpp"

#include <cppio/buffer.hpp>
#include <cppio/copy.hpp>
#include <cppio/copy_strategy.hpp>
#include <cppio/fault.hpp>
#include <cppio/limit.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

// A reader over an in-memory payload, also BufferedReadable via BufferedReader.
class MemReader final : public cppio::Reader {
public:
    cppio::MemoryReader mem;
    explicit MemReader(std::string_view s) : mem(cppio::MemoryReader::from_string(s)) {}
    cppio::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        return mem.read_some(dst);
    }
};

void run_cell(std::size_t total, cppio::CopyStrategy strategy, const char* strat_name,
              cppio::CopyLimit limit, const char* limit_name) {
    std::string payload(total, 'X');
    std::vector<std::byte> rbuf(64 * 1024);
    std::vector<std::byte> scratch(64 * 1024);
    cppio::MemoryWriter out;

    cppio::CopyStats st;
    cppio::BufferStats bs;
    // warmup
    for (int i = 0; i < 1; ++i) {
        MemReader rd(payload); cppio::BufferedReader br(rd, rbuf, &bs);
        cppio::MemoryWriter wo;
        cppio::CopyOptions o; o.strategy = strategy; o.limit = limit;
        (void)cppio::copy_all(br, wo, std::span<std::byte>(scratch), o);
    }
    st = {}; bs = {};
    MemReader rd(payload); cppio::BufferedReader br(rd, rbuf, &bs);
    cppio::MemoryWriter wo;
    cppio::CopyOptions o; o.strategy = strategy; o.limit = limit;
    auto t0 = now_ns();
    auto res = cppio::copy_all(br, wo, std::span<std::byte>(scratch), o, &st);
    auto elapsed = now_ns() - t0;
    if (!res.has_value()) return;

    cppio::bench::BenchResult r;
    r.case_name = "copy_strategy";
    r.mode = std::string(strat_name) + "/" + limit_name;
    r.bytes = res.value();
    r.iterations = st.copy_loop_iterations;
    r.elapsed_ns = elapsed;
    r.copy_stats = st;
    r.buffer_stats = bs;
    cppio::bench::print_csv_row(std::cout, r);
}

}  // namespace

int main() {
    cppio::bench::print_csv_header(std::cout);
    for (std::size_t total : {4u * 1024u, 64u * 1024u, 1024u * 1024u, 16u * 1024u * 1024u}) {
        run_cell(total, cppio::CopyStrategy::Scratch,       "scratch",       cppio::CopyLimit::unlimited(), "unlimited");
        run_cell(total, cppio::CopyStrategy::BufferedFirst, "buffered_first",cppio::CopyLimit::unlimited(), "unlimited");
        run_cell(total, cppio::CopyStrategy::Auto,          "auto",          cppio::CopyLimit::unlimited(), "unlimited");
        // small + exact limit cases
        run_cell(total, cppio::CopyStrategy::BufferedFirst, "buffered_first",cppio::CopyLimit::bytes(total / 4), "small_limit");
        run_cell(total, cppio::CopyStrategy::BufferedFirst, "buffered_first",cppio::CopyLimit::bytes(total),     "exact_limit");
    }
    return 0;
}
