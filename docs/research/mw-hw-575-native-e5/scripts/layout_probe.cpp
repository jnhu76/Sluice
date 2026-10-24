#include <cstdio>
#include <sluice/async/detail/threadpool_backend.hpp>
#include <sluice/async/detail/uring_backend.hpp>
int main() {
    std::printf("sizeof(ThreadPoolBackend)=%zu\n", sizeof(sluice::async::ThreadPoolBackend));
    std::printf("sizeof(UringAsyncBackend)=%zu\n", sizeof(sluice::async::UringAsyncBackend));
    return 0;
}
