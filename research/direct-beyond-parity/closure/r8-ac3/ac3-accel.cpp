// AC-3 acceleration-attribution probe: measures coarse wall time of a
// 64-MiB copy_file_range loop at a chosen (source,destination) offset
// alignment, plus the same range via cp(1) reflink modes for the ceiling
// contrast. Values are coarse-grained on purpose (R8 review item:
// filesystem acceleration influence).
// Build: g++ -O2 -std=c++20 ac3-accel.cpp -o ac3-accel
// usage: ac3-accel <src> <dst> <align|misalign>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::size_t kTotal = 67108864;
constexpr std::size_t kChunk = 65536;

double wall_ms() {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(t).count();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <src> <dst> <align|misalign>\n", argv[0]);
        return 2;
    }
    const off_t off = std::string(argv[3]) == "misalign" ? 1 : 0;
    const int s = ::open(argv[1], O_RDONLY | O_CLOEXEC);
    const int d = ::open(argv[2], O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (s < 0 || d < 0) {
        std::fprintf(stderr, "open failed\n");
        return 1;
    }
    // warm both ranges once
    off_t w = 0, wd = 0;
    while (w < static_cast<off_t>(kTotal)) {
        if (::copy_file_range(s, &w, d, &wd, kChunk, 0) <= 0)
            break;
    }
    const double t0 = wall_ms();
    off_t a = off, b = off;
    std::size_t copied = 0;
    const std::size_t limit = (kTotal - static_cast<std::size_t>(off)) /
                              kChunk * kChunk;
    while (copied + kChunk <= limit) {
        off_t oa = static_cast<off_t>(off + copied);
        off_t ob = static_cast<off_t>(off + copied);
        const ssize_t r = ::copy_file_range(s, &oa, d, &ob, kChunk, 0);
        if (r != static_cast<ssize_t>(kChunk)) {
            std::fprintf(stderr, "short cfr r=%zd errno=%d at %zu\n", r, errno,
                         copied);
            return 1;
        }
        copied += kChunk;
    }
    const double t1 = wall_ms();
    (void)a;
    (void)b;
    std::printf("mode=%s off=%ld copied=%zu wall_ms=%.1f ns_per_chunk=%.0f\n",
                argv[3], static_cast<long>(off), copied, t1 - t0,
                (t1 - t0) * 1e6 / static_cast<double>(copied / kChunk));
    ::close(s);
    ::close(d);
    return 0;
}
