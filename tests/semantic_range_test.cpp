#include <sluice/blocking/file.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/file_resource.hpp>

#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

using sluice::IoError;
using sluice::detail::DataOpVerdict;
using sluice::detail::kMaxNativeOffset;
using sluice::detail::kMaxNativeTransfer;
using sluice::detail::range_is_valid;
using sluice::detail::size_is_representable;

struct RangeCase {
    std::uint64_t offset;
    std::size_t length;
    bool expected_valid;
};

// Wide arithmetic on purpose, so the cross-check cannot share the
// implementation's overflow behaviour.
bool rule_by_definition(std::uint64_t offset, std::size_t length) {
    if (length == 0)
        return true;
    const auto last = static_cast<unsigned __int128>(offset) +
                      static_cast<unsigned __int128>(length) - 1;
    return last <= static_cast<unsigned __int128>(kMaxNativeOffset);
}

const RangeCase kCases[] = {
    {0, 0, true},
    {0, 1, true},
    {1, 1, true},
    {kMaxNativeOffset, 0, true},
    {kMaxNativeOffset, 1, true},
    {kMaxNativeOffset - 1, 1, true},
    {kMaxNativeOffset - 1, 2, true},
    {kMaxNativeOffset, 2, false},
    {kMaxNativeOffset + 1, 0, true},
    {kMaxNativeOffset + 1, 1, false},
    {std::numeric_limits<std::uint64_t>::max(), 0, true},
    {std::numeric_limits<std::uint64_t>::max(), 1, false},
    {std::numeric_limits<std::uint64_t>::max() - 3, 4, false},
};

bool range_table_holds() {
    for (const RangeCase& c : kCases) {
        if (range_is_valid(c.offset, c.length) != c.expected_valid) {
            std::fprintf(stderr, "FAIL: range_is_valid(%llu, %zu) != %d\n",
                         static_cast<unsigned long long>(c.offset), c.length,
                         static_cast<int>(c.expected_valid));
            return false;
        }
        if (range_is_valid(c.offset, c.length) != rule_by_definition(c.offset, c.length)) {
            std::fprintf(stderr, "FAIL: implementation and restated rule disagree\n");
            return false;
        }
    }
    return true;
}

bool no_wrap_at_the_top_of_the_range() {
    if (!range_is_valid(kMaxNativeOffset - 7, 8))
        return false;
    if (range_is_valid(kMaxNativeOffset - 7, 9))
        return false;
    return !range_is_valid(kMaxNativeOffset, std::numeric_limits<std::size_t>::max());
}

bool zero_length_has_no_range() {
    return range_is_valid(0, 0) && range_is_valid(kMaxNativeOffset + 1, 0) &&
           range_is_valid(std::numeric_limits<std::uint64_t>::max(), 0);
}

bool size_bound_holds() {
    if (!size_is_representable(0) || !size_is_representable(kMaxNativeOffset))
        return false;
    return !size_is_representable(kMaxNativeOffset + 1);
}

bool checked_offset_reports_invalid_argument() {
    const sluice::detail::DataOpRequest request{false,
                                               sluice::FileAccess::read_only,
                                               sluice::detail::FileOperation::read,
                                               kMaxNativeOffset + 1,
                                               1};
    const DataOpVerdict verdict = sluice::detail::precheck_data_op(request);
    if (verdict != DataOpVerdict::reject_range)
        return false;
    const auto rejection = sluice::detail::rejection_of(verdict);
    return rejection.has_value() && rejection->code == IoError::Code::invalid_argument;
}

bool native_transfer_limit_is_a_range_rejection() {
    if (kMaxNativeTransfer == 0)
        return false;
    const sluice::detail::DataOpRequest request{false,
                                               sluice::FileAccess::read_only,
                                               sluice::detail::FileOperation::read,
                                               0,
                                               kMaxNativeTransfer + 1};
    const DataOpVerdict verdict = sluice::detail::precheck_data_op(request);
    if (verdict != DataOpVerdict::reject_range)
        return false;
    const auto rejection = sluice::detail::rejection_of(verdict);
    return rejection.has_value() && rejection->code == IoError::Code::invalid_argument;
}

bool canonical_positional_io_reports_invalid_argument() {
    char path[] = "/tmp/sluice_range_canonical_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return false;
    const ssize_t wrote = ::write(fd, "seed", 4);
    ::close(fd);
    if (wrote != 4) {
        ::unlink(path);
        return false;
    }

    bool ok = true;
    std::vector<std::byte> dst(4, std::byte{0});
    {
        auto opened = sluice::File::open(
            path,
            sluice::FileOpen{sluice::FileAccess::read_only,
                             sluice::FileExistence::open_existing,
                             sluice::FileInitialContents::preserve});
        if (!opened.has_value()) {
            ::unlink(path);
            return false;
        }
        auto file = std::move(opened).value();
        auto result = sluice::blocking::read_at(file, kMaxNativeOffset + 1,
                                                std::span<std::byte>(dst));
        ok = ok && !result.has_value() &&
             result.error().code == IoError::Code::invalid_argument;
    }
    {
        auto opened = sluice::File::open(
            path,
            sluice::FileOpen{sluice::FileAccess::write_only,
                             sluice::FileExistence::open_existing,
                             sluice::FileInitialContents::preserve});
        if (!opened.has_value()) {
            ::unlink(path);
            return false;
        }
        auto file = std::move(opened).value();
        const std::vector<std::byte> src(4, std::byte{0});
        auto result = sluice::blocking::write_at(file, kMaxNativeOffset + 1,
                                                 std::span<const std::byte>(src));
        ok = ok && !result.has_value() &&
             result.error().code == IoError::Code::invalid_argument;
    }
    ::unlink(path);
    return ok;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"range_table_holds", range_table_holds},
        {"no_wrap_at_the_top_of_the_range", no_wrap_at_the_top_of_the_range},
        {"zero_length_has_no_range", zero_length_has_no_range},
        {"size_bound_holds", size_bound_holds},
        {"checked_offset_reports_invalid_argument", checked_offset_reports_invalid_argument},
        {"native_transfer_limit_is_a_range_rejection", native_transfer_limit_is_a_range_rejection},
        {"canonical_positional_io_reports_invalid_argument",
         canonical_positional_io_reports_invalid_argument},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu range semantic tests passed (%zu range table cases)\n",
                sizeof(tests) / sizeof(tests[0]), sizeof(kCases) / sizeof(kCases[0]));
    return 0;
}
