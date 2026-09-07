









#pragma once

#include <cstdint>

namespace sluice {


struct SyscallStats {
    std::uint64_t read_syscalls = 0;
    std::uint64_t read_syscall_bytes = 0;
    std::uint64_t read_syscall_errors = 0;
    std::uint64_t write_syscalls = 0;
    std::uint64_t write_syscall_bytes = 0;
    std::uint64_t write_syscall_errors = 0;
};


struct BufferStats {
    std::uint64_t read_requests = 0;
    std::uint64_t read_request_bytes = 0;
    std::uint64_t read_buffer_hits = 0;
    std::uint64_t read_buffer_hit_bytes = 0;
    std::uint64_t read_buffer_misses = 0;
    std::uint64_t read_refill_calls = 0;
    std::uint64_t read_refill_bytes = 0;

    std::uint64_t write_requests = 0;
    std::uint64_t write_request_bytes = 0;
    std::uint64_t write_buffered_calls = 0;
    std::uint64_t write_buffered_bytes = 0;
    std::uint64_t write_flush_calls = 0;
    std::uint64_t write_flush_bytes = 0;
    std::uint64_t write_direct_calls = 0;
    std::uint64_t write_direct_bytes = 0;
};





struct CopyStats {
    std::uint64_t copy_calls = 0;
    std::uint64_t copy_loop_iterations = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t eof_stops = 0;
    std::uint64_t limit_stops = 0;
    std::uint64_t reader_error_stops = 0;
    std::uint64_t writer_error_stops = 0;


    std::uint64_t buffered_fast_path_calls = 0;
    std::uint64_t buffered_fast_path_bytes = 0;



    std::uint64_t scratch_path_calls = 0;
    std::uint64_t scratch_path_bytes = 0;




    std::uint64_t strategy_auto_calls = 0;
    std::uint64_t strategy_scratch_calls = 0;
    std::uint64_t strategy_buffered_first_calls = 0;
};



struct SyncStats {
    std::uint64_t sync_data_calls = 0;
    std::uint64_t sync_data_errors = 0;
    std::uint64_t sync_all_calls = 0;
    std::uint64_t sync_all_errors = 0;
};




struct UringStats {
    std::uint64_t queue_init_calls = 0;
    std::uint64_t submit_calls = 0;
    std::uint64_t submitted_ops = 0;
    std::uint64_t completed_ops = 0;
    std::uint64_t completion_errors = 0;
    std::uint64_t bytes_completed = 0;
};









struct VectorStats {
    std::uint64_t read_vec_calls = 0;
    std::uint64_t read_vec_bytes = 0;
    std::uint64_t read_vec_iovecs = 0;
    std::uint64_t read_vec_fallback_calls = 0;
    std::uint64_t write_vec_calls = 0;
    std::uint64_t write_vec_bytes = 0;
    std::uint64_t write_vec_iovecs = 0;
    std::uint64_t write_vec_fallback_calls = 0;
};






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
