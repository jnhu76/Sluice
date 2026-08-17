// sluice-hash — bounded streaming file hashing over the Sluice async runtime.
//
// One ApplicationRuntime + ONE task hash a list of files in CLI order: for
// each file, positional async reads into ONE reusable buffer feed the SHA-256
// streaming state until EOF. Files are processed sequentially (V1): a
// read/error on one file never interleaves output of another, and memory is
// bounded by configuration, not input size.
//
// All code uses installed/public headers only. No SLUICE_ASYNC_INTERNAL_TESTING.
#pragma once

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/async_io_context.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sluice_hash {

// App-level resource limits (fixed and explainable; same philosophy as
// sluice-copy's kMax*). The memory upper bound of the hashing engine is ONE
// buffer of buffer_size plus O(1) hasher state — independent of file count
// and file size.
constexpr std::size_t kMinBufferSize = 4 * 1024;         // 4 KiB
constexpr std::size_t kMaxBufferSize = 64 * 1024 * 1024; // 64 MiB
constexpr unsigned kMaxWorkers = 64;

// One input file: the path (for reporting) and a read-only fd opened and
// validated (regular file) by the caller. The caller owns the fds and closes
// them after the run.
struct HashInput {
    std::string path;
    int fd = -1;
};

// Per-file outcome. `error` empty <=> success (hex valid, bytes hashed).
struct FileHash {
    std::string path;
    std::string hex;                    // lowercase 64-char digest on success
    std::uint64_t bytes_hashed = 0;     // bytes consumed on success
    std::optional<sluice::IoError> error;
};

// Hash every input, in order, inside one Runtime (ThreadPoolBackend). A
// per-file error (read failure, cancellation) is recorded on that file and
// does NOT stop later files. Returns one FileHash per input, same order.
//
// `buffer_size` is validated against the limits above (out-of-range =>
// every file gets invalid_state, nothing runs); `workers` likewise.
std::vector<FileHash> hash_files(std::vector<HashInput> inputs,
                                 std::size_t buffer_size, unsigned workers);

// Backend-injecting variant for deterministic fault tests (same algorithm;
// the abstract AsyncBackend keeps the app target production-clean).
std::vector<FileHash> hash_files_with_backend(
    std::vector<HashInput> inputs, std::size_t buffer_size, unsigned workers,
    std::unique_ptr<sluice::async::AsyncBackend> backend);

}  // namespace sluice_hash
