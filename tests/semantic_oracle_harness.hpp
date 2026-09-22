#pragma once

// One semantic oracle, several execution paths.
//
// Expectations are written out in the scenario table, never read back from the
// oracle implementation. Two comparisons run against that same table:
//   - `check_oracle_against_table` checks the shared rules themselves, so a
//     rule regression cannot move both sides of the comparison;
//   - `run_path` checks one execution path, which reports only what it
//     observed.
//
// The comparison covers only what every path can observe: rejection with its
// canonical error, acceptance, or a completed logical no-op. Byte counts and
// physical traces are the execution's business and are not compared here.

#include <sluice/detail/file_semantics.hpp>
#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace sluice_semantic {

using sluice::FileAccess;
using sluice::IoError;
using sluice::detail::DataOpVerdict;
using sluice::detail::FileOperation;

// A backend-neutral description of one logical operation.
struct Input {
    bool closed = false;
    sluice::FileAccess access = sluice::FileAccess::read_only;
    FileOperation operation = FileOperation::read;
    std::uint64_t offset = 0;
    std::size_t length = 0;
};

struct Scenario {
    const char* name;
    Input input;
    // Written out from the root, not read back from the oracle;
    // `expected_rejection_code` is populated exactly when the verdict is a
    // rejection.
    DataOpVerdict expected_verdict;
    std::optional<IoError::Code> expected_rejection_code;
};

// Largest scenario length a direct probe may allocate. Scenarios that need a
// bigger span are reported as not drivable on the direct path.
inline constexpr std::size_t kDirectScratchLimit = 64;

inline bool direct_drivable(const Input& input) {
    return input.length <= kDirectScratchLimit;
}

// One temp file opened with each access mode, so every scenario has the
// resource it describes without the probe inventing an access claim. It lives
// in the async-free harness so a direct-only target can drive the same
// scenarios without linking a request path.
class AccessFixtures {
  public:
    static AccessFixtures create(std::size_t content_size) {
        AccessFixtures fixtures;
        char path[] = "/tmp/sluice_semantic_fixtures_XXXXXX";
        const int fd = ::mkstemp(path);
        if (fd < 0)
            return fixtures;
        std::vector<char> payload(content_size, 'x');
        if (content_size > 0 && ::write(fd, payload.data(), content_size) !=
                                    static_cast<ssize_t>(content_size)) {
            ::close(fd);
            return fixtures;
        }
        ::close(fd);
        fixtures.path_ = path;

        sluice::FileOpen mode;
        mode.existence = sluice::FileExistence::open_existing;
        mode.access = sluice::FileAccess::read_only;
        auto ro = sluice::File::open(fixtures.path_, mode);
        mode.access = sluice::FileAccess::write_only;
        auto wo = sluice::File::open(fixtures.path_, mode);
        mode.access = sluice::FileAccess::read_write;
        auto rw = sluice::File::open(fixtures.path_, mode);
        if (!ro.has_value() || !wo.has_value() || !rw.has_value()) {
            fixtures.path_.clear();
            return fixtures;
        }
        fixtures.read_only_ = std::move(ro.value());
        fixtures.write_only_ = std::move(wo.value());
        fixtures.read_write_ = std::move(rw.value());
        return fixtures;
    }

    AccessFixtures() = default;
    AccessFixtures(AccessFixtures&&) noexcept = default;
    AccessFixtures& operator=(AccessFixtures&&) noexcept = default;
    AccessFixtures(const AccessFixtures&) = delete;
    AccessFixtures& operator=(const AccessFixtures&) = delete;

    ~AccessFixtures() {
        read_only_.reset();
        write_only_.reset();
        read_write_.reset();
        if (!path_.empty())
            ::unlink(path_.c_str());
    }

    bool ok() const { return !path_.empty(); }
    const std::string& path() const { return path_; }

    const sluice::File* for_access(sluice::FileAccess access) const {
        switch (access) {
        case sluice::FileAccess::read_only:
            return read_only_.has_value() ? &*read_only_ : nullptr;
        case sluice::FileAccess::write_only:
            return write_only_.has_value() ? &*write_only_ : nullptr;
        case sluice::FileAccess::read_write:
            return read_write_.has_value() ? &*read_write_ : nullptr;
        }
        return nullptr;
    }

  private:
    std::string path_;
    std::optional<sluice::File> read_only_;
    std::optional<sluice::File> write_only_;
    std::optional<sluice::File> read_write_;
};

// Caller-visible outcome of one path attempt.
struct Observation {
    bool rejected = false;
    IoError error{};
    // Whether the path completed a logical no-op without an OS call (direct)
    // or without a data dispatch (request). Empty when the path cannot observe
    // this for the given input; only consulted for a `complete_empty`
    // expectation.
    std::optional<bool> no_op_without_dispatch;
};

inline Observation observe_rejection(IoError error) {
    return Observation{true, error, std::nullopt};
}

inline Observation observe_accepted(std::optional<bool> no_op_without_dispatch = std::nullopt) {
    return Observation{false, IoError{.code = IoError::Code::backend_error},
                       no_op_without_dispatch};
}

inline Observation observe_result(const sluice::Result<std::size_t>& result,
                                 std::optional<bool> no_op_without_dispatch = std::nullopt) {
    if (result.has_value())
        return observe_accepted(no_op_without_dispatch);
    return observe_rejection(result.error());
}

inline Observation observe_result(const sluice::Result<void>& result) {
    if (result.has_value())
        return observe_accepted();
    return observe_rejection(result.error());
}

inline const char* describe(DataOpVerdict verdict) {
    switch (verdict) {
    case DataOpVerdict::execute:
        return "execute";
    case DataOpVerdict::complete_empty:
        return "complete_empty(no-op)";
    case DataOpVerdict::reject_closed:
        return "reject_closed(invalid_state)";
    case DataOpVerdict::reject_access:
        return "reject_access(invalid_argument)";
    case DataOpVerdict::reject_range:
        return "reject_range(invalid_argument)";
    }
    return "unknown";
}

inline void describe(const Observation& observed, char* buffer, std::size_t size) {
    if (!observed.rejected) {
        std::snprintf(buffer, size, "%s",
                      observed.no_op_without_dispatch.value_or(false) ? "accepted(no-op)"
                                                                      : "accepted");
        return;
    }
    std::snprintf(buffer, size, "rejected(%s)", sluice::to_string(observed.error.code).data());
}

// A rejection where the table expects an accepted operation is a mismatch: a
// valid range refused by a filesystem is an operation result, not a precedence
// statement, so it does not belong in the table.
inline bool agree(const Scenario& scenario, const Observation& observed) {
    if (scenario.expected_rejection_code.has_value())
        return observed.rejected && observed.error.code == *scenario.expected_rejection_code;
    if (observed.rejected)
        return false;
    if (scenario.expected_verdict == DataOpVerdict::complete_empty) {
        if (observed.no_op_without_dispatch.has_value())
            return *observed.no_op_without_dispatch;
    }
    return true;
}

// Checks the shared rules against the written table, independently of any path.
// Returns the number of disagreements; a rule regression fails here.
inline std::size_t check_oracle_against_table(const Scenario* scenarios, std::size_t count) {
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Input& input = scenarios[i].input;
        const DataOpVerdict derived =
            sluice::detail::is_byte_operation(input.operation)
                ? sluice::detail::precheck_data_op({input.closed, input.access, input.operation,
                                                   input.offset, input.length})
                : sluice::detail::precheck_state_op(input.closed, input.access, input.operation);
        const std::optional<IoError> rejection = sluice::detail::rejection_of(derived);
        bool agrees = derived == scenarios[i].expected_verdict;
        if (agrees) {
            if (scenarios[i].expected_rejection_code.has_value()) {
                agrees = rejection.has_value() &&
                         rejection->code == *scenarios[i].expected_rejection_code;
            } else {
                agrees = !rejection.has_value();
            }
        }
        if (agrees)
            continue;
        std::fprintf(stderr, "ORACLE/TABLE MISMATCH %s: table requires %s, the shared rules answer %s\n",
                     scenarios[i].name, describe(scenarios[i].expected_verdict),
                     describe(derived));
        ++mismatches;
    }
    return mismatches;
}

// Runs every scenario against one path adapter. The adapter receives the Input
// and returns what that path observed; it must not consult the table. Returns the
// number of mismatches, prints each one, and optionally appends the mismatched
// scenario names for gap pinning.
template <class Attempt>
std::size_t run_path(const char* path_name, const Scenario* scenarios, std::size_t count,
                     Attempt&& attempt, std::vector<const char*>* mismatched_names = nullptr) {
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Observation observed = attempt(scenarios[i].input);
        if (agree(scenarios[i], observed))
            continue;
        char observed_text[64];
        describe(observed, observed_text, sizeof(observed_text));
        std::fprintf(stderr, "MISMATCH [%s] %s: table requires %s, path observed %s\n", path_name,
                     scenarios[i].name, describe(scenarios[i].expected_verdict), observed_text);
        if (mismatched_names != nullptr)
            mismatched_names->push_back(scenarios[i].name);
        ++mismatches;
    }
    return mismatches;
}

inline bool name_less(const char* a, const char* b) {
    return std::strcmp(a, b) < 0;
}

// Compares a path's observed divergences against the divergence set recorded
// for it. Equality is required in both directions — a divergence that closes
// and a new divergence that appears both fail — so a recorded gap cannot rot.
// The recorded set lives beside the scenario table and is cited by the
// conformance ledger; the two must be updated together.
inline bool matches_recorded_divergences(const char* path_name, std::vector<const char*> observed,
                                        std::vector<const char*> recorded) {
    std::sort(observed.begin(), observed.end(), name_less);
    std::sort(recorded.begin(), recorded.end(), name_less);
    if (observed == recorded)
        return true;
    std::fprintf(stderr, "DIVERGENCE SET CHANGED [%s]: %zu observed, %zu recorded\n", path_name,
                 observed.size(), recorded.size());
    for (const char* name : observed)
        std::fprintf(stderr, "  observed: %s\n", name);
    for (const char* name : recorded)
        std::fprintf(stderr, "  recorded: %s\n", name);
    return false;
}

} // namespace sluice_semantic
