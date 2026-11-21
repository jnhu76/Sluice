#pragma once

#include <cstdint>

namespace sluice {

struct AsyncStats {
    std::uint64_t submit_calls = 0;
    std::uint64_t submitted_ops = 0;
    std::uint64_t poll_calls = 0;
    std::uint64_t wait_calls = 0;
    std::uint64_t completed_ops = 0;
    std::uint64_t canceled_ops = 0;
    std::uint64_t completion_errors = 0;
    std::uint64_t short_completions = 0;
    std::uint64_t max_outstanding = 0;

    std::uint64_t queue_full_retries = 0;

    std::uint64_t invalid_state_rejections = 0;
};

}
