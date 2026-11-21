#pragma once

#include <sluice/async/async_io_context.hpp>

#include <cstddef>

namespace sluice::async {

struct ThreadPoolConfig {
    std::size_t request_capacity = 64;
    std::size_t worker_count = 4;
};

// Invalid capacities are std::invalid_argument; setup failures throw as
// documented for the backend. There is no fallback to another execution.
//
// stats is a caller-owned borrow that must outlive the context: a diagnostic
// tally only. Writes are partitioned across the context lock, the backend
// locks and the single progress-drive thread, so unsynchronized reads
// concurrent with a live context race.
AsyncIoContext make_threadpool_context(ThreadPoolConfig config = ThreadPoolConfig{},
                                       AsyncStats* stats = nullptr);

}
