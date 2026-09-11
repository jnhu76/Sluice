#include <sluice/detail/io_validation.hpp>

#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

namespace {

using sluice::IoError;
using sluice::Result;
using sluice::detail::checked_posix_offset;
using sluice::detail::uring_chunk_length;

bool chunk_length_is_identity_below_native_max() {
    if (uring_chunk_length(0) != 0u)
        return false;
    if (uring_chunk_length(1) != 1u)
        return false;
    constexpr std::size_t native_max =
        static_cast<std::size_t>(std::numeric_limits<unsigned>::max());
    if (uring_chunk_length(native_max - 1) != native_max - 1)
        return false;
    return uring_chunk_length(native_max) == native_max;
}

bool chunk_length_clamps_above_native_max() {
    constexpr std::size_t native_max =
        static_cast<std::size_t>(std::numeric_limits<unsigned>::max());
    if (uring_chunk_length(native_max + 1) != native_max)
        return false;
    if (uring_chunk_length(native_max * 2) != native_max)
        return false;
    return uring_chunk_length(std::numeric_limits<std::size_t>::max()) == native_max;
}

bool posix_offset_accepts_representable_offsets() {
    if (!checked_posix_offset(0).has_value())
        return false;
    if (!checked_posix_offset(1).has_value())
        return false;
    const std::uint64_t native_max =
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    return checked_posix_offset(native_max).has_value();
}

bool posix_offset_rejects_unrepresentable_offsets() {
    const std::uint64_t native_max =
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    auto over = checked_posix_offset(native_max + 1);
    if (over.has_value())
        return false;
    if (over.error().code != IoError::Code::invalid_state)
        return false;
    auto huge = checked_posix_offset(std::numeric_limits<std::uint64_t>::max());
    if (huge.has_value())
        return false;
    return huge.error().code == IoError::Code::invalid_state;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"chunk_length_is_identity_below_native_max",
         chunk_length_is_identity_below_native_max},
        {"chunk_length_clamps_above_native_max", chunk_length_clamps_above_native_max},
        {"posix_offset_accepts_representable_offsets",
         posix_offset_accepts_representable_offsets},
        {"posix_offset_rejects_unrepresentable_offsets",
         posix_offset_rejects_unrepresentable_offsets},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu io validation boundary tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
