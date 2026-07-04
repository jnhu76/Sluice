// sync_random_read: positional blocking read demo (sluice-CORE-024S §7).
//
// Demonstrates G6 (positional I/O does not mutate the shared file offset):
// reads two disjoint regions of a file via read_at_exact, then a sequential
// read_some that proves the cursor was untouched. Illustrates the sync
// positional contract without async/coroutine machinery.
#include <sluice/file.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
    }
    sluice::FileReader r(argv[1]);
    if (!r.opened()) {
        std::fprintf(stderr, "error: cannot open %s\n", argv[1]);
        return 1;
    }

    // Positional reads at two disjoint offsets. These do NOT move the cursor.
    std::byte a[4]{}, b[4]{};
    if (auto res = r.read_at_exact(0, std::span<std::byte>(a)); !res.has_value()) {
        std::fprintf(stderr, "read_at_exact(0) failed\n");
        return 1;
    }
    if (auto res = r.read_at_exact(100, std::span<std::byte>(b)); !res.has_value()) {
        std::fprintf(stderr, "read_at_exact(100) failed (file may be < 104 bytes)\n");
        // not fatal — the contract demo still holds for the first read
    }

    // Cursor-based read: starts at offset 0 (positional reads left it untouched).
    std::byte cur[4]{};
    auto n = r.read_some(std::span<std::byte>(cur));
    if (!n.has_value()) {
        std::fprintf(stderr, "read_some failed\n");
        return 1;
    }
    // cur should equal a (both read offset 0) — positional reads didn't advance.
    bool cursor_intact = (n.value() == 4 && std::memcmp(cur, a, 4) == 0);
    std::printf("positional reads left cursor at 0: %s\n",
                cursor_intact ? "yes (G6 holds)" : "no");
    return cursor_intact ? 0 : 1;
}
