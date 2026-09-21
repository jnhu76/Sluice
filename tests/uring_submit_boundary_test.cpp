// io_uring submission-path mechanism boundaries. These are transfer limits of
// the io_uring execution, not File semantics: a chunk length clamps the SQE
// length, and the resulting short count is the ordinary allowed short transfer.
// File range/offset rules are covered by semantic_range_test.
#include <sluice/detail/uring_submit.hpp>

#include <cstdio>
#include <limits>

namespace {

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

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"chunk_length_is_identity_below_native_max", chunk_length_is_identity_below_native_max},
        {"chunk_length_clamps_above_native_max", chunk_length_clamps_above_native_max},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu io_uring submit boundary tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
