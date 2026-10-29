#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: %s path bytes\n", argv[0]); return 2; }
    const std::size_t total = std::strtoull(argv[2], nullptr, 10);
    const int fd = ::open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 1;
    std::vector<std::byte> buf(1u << 20);
    std::size_t off = 0;
    while (off < total) {
        const std::size_t chunk = std::min(buf.size(), total - off);
        for (std::size_t i = 0; i < chunk; ++i)
            buf[i] = static_cast<std::byte>((off + i) * 1103515245u >> 13 & 0xff);
        if (::write(fd, buf.data(), chunk) < 0) return 1;
        off += chunk;
    }
    ::close(fd);
    std::printf("wrote %zu bytes\n", total);
    return 0;
}
