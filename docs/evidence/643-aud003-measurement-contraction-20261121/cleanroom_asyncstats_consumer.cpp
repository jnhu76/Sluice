#include <sluice/measurement.hpp>

#include <cstdio>

static_assert(sizeof(sluice::AsyncStats) == 11 * sizeof(std::uint64_t),
              "AsyncStats field count drifted from the contracted 11 counters");

int main() {
    sluice::AsyncStats stats{};
    stats.submit_calls = 3;
    stats.submitted_ops = 4;
    stats.poll_calls = 5;
    stats.wait_calls = 6;
    stats.completed_ops = 4;
    stats.canceled_ops = 0;
    stats.completion_errors = 0;
    stats.short_completions = 1;
    stats.max_outstanding = 2;
    stats.queue_full_retries = 0;
    stats.invalid_state_rejections = 0;

    sluice::AsyncStats copy = stats;
    copy.completed_ops += 1;

    const sluice::AsyncStats borrowed = copy;
    std::printf("submit_calls=%zu completed_ops=%zu sizeof=%zu\n",
                static_cast<std::size_t>(borrowed.submit_calls),
                static_cast<std::size_t>(borrowed.completed_ops),
                sizeof(borrowed));
    return borrowed.submit_calls == 3 && borrowed.completed_ops == 5 ? 0 : 1;
}
