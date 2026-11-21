#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/result.hpp>

#include <cstddef>

namespace sluice::async {

struct UringConfig {
    std::size_t request_capacity = 64;
    unsigned queue_depth = 64;
};

// Explicit io_uring selection. Fails with not_supported when this build
// carries no io_uring profile; with the profile present, construction-time
// ring/capability setup failures throw as documented for the backend. Never
// falls back to another execution.
//
// stats is a caller-owned borrow that must outlive the context: a diagnostic
// tally only. Writes are partitioned across the context lock, the backend
// locks and the single progress-drive thread, so unsynchronized reads
// concurrent with a live context race.
Result<AsyncIoContext> make_uring_context(UringConfig config = UringConfig{},
                                          AsyncStats* stats = nullptr);

}
