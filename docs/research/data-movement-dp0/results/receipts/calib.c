#include <stdint.h>
volatile uint64_t sink;
__attribute__((noinline)) uint64_t work(uint64_t n) {
    uint64_t x = 0;
    for (uint64_t i = 0; i < n; ++i) x += i;
    return x;
}
int main(int argc, char** argv) {
    uint64_t n = 100000000ULL;
    for (int r = 0; r < 3; ++r) sink += work(n);
    return 0;
}
