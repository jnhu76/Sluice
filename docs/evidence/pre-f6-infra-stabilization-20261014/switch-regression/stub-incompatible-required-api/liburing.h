#pragma once
struct io_uring { unsigned dummy; };
struct io_uring_sqe { unsigned dummy; };
struct io_uring_cqe { unsigned dummy; };
int io_uring_queue_init(unsigned entries, struct io_uring* ring, unsigned flags);
