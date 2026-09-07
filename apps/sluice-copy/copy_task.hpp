











#pragma once

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace sluice_copy {


enum class SyncPolicy {
    none,
    data,
    all,
};












constexpr std::size_t kMaxBufferSize = 64 * 1024 * 1024;
constexpr std::size_t kMaxPipelineDepth = 64;
constexpr std::size_t kMaxPipelineBytes = 512 * 1024 * 1024;
constexpr unsigned kMaxWorkers = 64;




struct CopyStats {
    std::uint64_t bytes_copied = 0;
    std::uint64_t read_ops = 0;
    std::uint64_t write_ops = 0;
    std::uint64_t short_writes = 0;
    SyncPolicy sync = SyncPolicy::none;
};










sluice::Result<CopyStats> run_sequential_copy(int src_fd, int dst_fd,
                                              std::size_t buffer_size,
                                              unsigned workers,
                                              SyncPolicy sync);








sluice::Result<CopyStats> run_sequential_copy_with_backend(
    int src_fd, int dst_fd, std::size_t buffer_size, unsigned workers,
    SyncPolicy sync, std::unique_ptr<sluice::async::AsyncBackend> backend);






































sluice::Result<CopyStats> run_pipelined_copy(int src_fd, int dst_fd,
                                             std::size_t buffer_size,
                                             std::size_t pipeline_depth,
                                             unsigned workers,
                                             SyncPolicy sync);




sluice::Result<CopyStats> run_pipelined_copy_with_backend(
    int src_fd, int dst_fd, std::size_t buffer_size,
    std::size_t pipeline_depth, unsigned workers, SyncPolicy sync,
    std::unique_ptr<sluice::async::AsyncBackend> backend);

}
