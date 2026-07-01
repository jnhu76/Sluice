// fault_write: demonstrates a deterministic write failure via FaultWriter.
#include <cppio/fault.hpp>
#include <cppio/observed.hpp>

#include <cstdio>
#include <span>
#include <string>

int main() {
    cppio::MemoryWriter sink;
    cppio::FaultPlan plan;
    plan.max_write_size = 4;
    plan.fail_after_write_calls = 2;  // 3rd call fails
    plan.error = cppio::IoError{cppio::IoError::Code::no_space};
    cppio::FaultWriter faulted(sink, plan);

    cppio::WriterStats stats{};
    cppio::ObservedWriter observed(faulted, stats);

    std::string msg = "this payload is longer than the budget allows here";
    auto res = observed.write_all(
        std::as_bytes(std::span(msg.data(), msg.size())));

    if (!res.has_value()) {
        std::printf("fault_write: deterministic failure as expected -> code=%s\n",
                    cppio::to_string(res.error().code).data());
    } else {
        std::printf("fault_write: unexpected success\n");
    }
    std::printf("stats: write_calls=%llu write_bytes=%llu short_writes=%llu\n",
                static_cast<unsigned long long>(stats.write_calls),
                static_cast<unsigned long long>(stats.write_bytes),
                static_cast<unsigned long long>(stats.short_writes));
    return res.has_value() ? 0 : 0;  // demo always exits 0
}
