// Cross-checks the two macro views' layout facts. A mismatch (exit 1) is the
// expected deterministic outcome under the liburing profile: it proves the
// ODR discriminator can tell the two views apart.
#include <cstddef>
#include <cstdio>

std::size_t odr_backend_bytes_without_uring_view();
std::size_t odr_backend_bytes_with_uring_view();

int main() {
    const std::size_t without = odr_backend_bytes_without_uring_view();
    const std::size_t with = odr_backend_bytes_with_uring_view();
    if (without != with) {
        std::printf("odr-divergent-mismatch without=%zu with=%zu\n", without, with);
        return 1;
    }
    std::printf("odr-divergent-insensitive %zu\n", without);
    return 0;
}
