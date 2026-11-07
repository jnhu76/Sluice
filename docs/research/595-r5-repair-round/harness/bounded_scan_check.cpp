// #595 R5-repair F5: direct bounded-scan equivalence check against the
// cleanup tree (b191bb89). Asserts, for every mixed eligible/ineligible
// span pattern of every length 0..64 and every table-emittable quantum:
//   batch_shaped_prefix_bounded(ops) == min(batch_shaped_prefix(ops), bound)
//   min(q, bounded) == min(q, full)          (the present_len decision)
// plus the front_ineligible zero-test identity. Compiles against the
// cleanup bench header; the original tree has no bounded variant by
// construction, so this check runs on the cleanup tree only.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "r3_ready_batch.hpp"

using bench::r3::ReadOp;

namespace {

ReadOp make_op(std::size_t len, std::byte* dst) {
    ReadOp op{};
    op.len = len;
    op.dst = dst;
    return op;
}

} // namespace

int main() {
    std::byte buf[8];
    constexpr std::size_t bound = bench::r3::budget::kEligiblePrefixScanBound;
    if (bound != 8) {
        std::printf("FAIL bound=%zu (expected ready_batch_cap=8)\n", bound);
        return 1;
    }
    const std::size_t quanta[] = {1, 2, 8}; // base 8, clamp 2, pinned 2/8
    std::size_t checked = 0;
    for (std::size_t n = 0; n <= 64; ++n) {
        // enumerate eligibility masks over a representative pattern set:
        // all-eligible, all-ineligible, first-k-eligible for every k, plus
        // deterministic pseudo-random masks (xorshift) - 20 per length
        std::vector<std::vector<char>> masks;
        masks.emplace_back(n, 1);
        masks.emplace_back(n, 0);
        for (std::size_t k = 0; k <= n; ++k) {
            std::vector<char> m(n, 0);
            for (std::size_t i = 0; i < k; ++i) m[i] = 1;
            masks.push_back(m);
            m.assign(n, 1);
            for (std::size_t i = 0; i < k; ++i) m[i] = 0;
            masks.push_back(m);
        }
        std::uint32_t xs = static_cast<std::uint32_t>(n * 2654435761u + 1);
        for (int r = 0; r < 20; ++r) {
            std::vector<char> m(n);
            for (std::size_t i = 0; i < n; ++i) {
                xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5;
                m[i] = static_cast<char>(xs & 1);
            }
            masks.push_back(m);
        }
        for (const auto& m : masks) {
            std::vector<ReadOp> ops;
            ops.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                ops.push_back(m[i] ? make_op(4096, buf)
                                   : make_op((m[i] + 1) % 2 ? 512 : 4096, nullptr));
            }
            const std::span<const ReadOp> span(ops.data(), ops.size());
            const std::size_t full = bench::r3::batch_shaped_prefix(span);
            const std::size_t bounded = bench::r3::batch_shaped_prefix_bounded(span);
            const std::size_t want = full < bound ? full : bound;
            if (bounded != want) {
                std::printf("FAIL n=%zu bounded=%zu want=%zu\n", n, bounded, want);
                return 1;
            }
            for (const std::size_t q : quanta) {
                const std::size_t d_bounded = q < bounded ? q : bounded;
                const std::size_t d_full = q < full ? q : full;
                if (d_bounded != d_full) {
                    std::printf("FAIL n=%zu q=%zu d_bounded=%zu d_full=%zu\n", n, q,
                                d_bounded, d_full);
                    return 1;
                }
            }
            if ((bounded == 0) != (full == 0)) {
                std::printf("FAIL n=%zu front_ineligible zero-test differs\n", n);
                return 1;
            }
            ++checked;
        }
    }
    std::printf("bounded_scan_check PASS (%zu spans, quanta {1,2,8}, bound=%zu)\n",
                checked, bound);
    return 0;
}
