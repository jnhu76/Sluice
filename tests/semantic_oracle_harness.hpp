#pragma once

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
    // Written out by hand, never derived from the oracle, or the comparison
    // becomes tautological.
    DataOpVerdict expected_verdict;
    std::optional<IoError::Code> expected_rejection_code;
};

inline constexpr std::size_t kDirectScratchLimit = 64;

inline bool direct_drivable(const Input& input) {
    return input.length <= kDirectScratchLimit;
}

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

struct Observation {
    bool rejected = false;
    IoError error{};
    // nullopt means the path cannot observe the no-op for this input;
    // consulted only for a `complete_empty` expectation.
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

// The adapter must report only what its path observed and never consult the
// table.
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

inline bool matches_recorded_divergences(const char* path_name, std::vector<const char*> observed,
                                        std::vector<const char*> recorded) {
    std::sort(observed.begin(), observed.end(), name_less);
    std::sort(recorded.begin(), recorded.end(), name_less);
    // The two vectors hold pointers from different origins (the scenario
    // table and a separately spelled recorded set), so identity is the name
    // text, never the pointer value.
    const bool same =
        observed.size() == recorded.size() &&
        std::equal(observed.begin(), observed.end(), recorded.begin(),
                   [](const char* a, const char* b) { return std::strcmp(a, b) == 0; });
    if (same)
        return true;
    std::fprintf(stderr, "DIVERGENCE SET CHANGED [%s]: %zu observed, %zu recorded\n", path_name,
                 observed.size(), recorded.size());
    for (const char* name : observed)
        std::fprintf(stderr, "  observed: %s\n", name);
    for (const char* name : recorded)
        std::fprintf(stderr, "  recorded: %s\n", name);
    return false;
}

// Probe-liveness self-check for the acceptance comparators. Every expected
// value is a hand-authored constant; deriving one from agree, run_path or
// matches_recorded_divergences would make this tautological.
inline bool comparison_oracle_selfcheck() {
    struct AgreeCase {
        const char* case_name;
        Scenario scenario;
        Observation observed;
        bool expected;
    };
    const AgreeCase agree_cases[] = {
        {"A_success_vs_success",
         {"agree_A", {false, FileAccess::read_only, FileOperation::read, 0, 4},
          DataOpVerdict::execute, std::nullopt},
         observe_accepted(), true},
        {"B_success_vs_rejection",
         {"agree_B", {false, FileAccess::read_only, FileOperation::read, 0, 4},
          DataOpVerdict::execute, std::nullopt},
         observe_rejection(IoError{.code = IoError::Code::invalid_argument}), false},
        {"B_complete_empty_vs_rejection",
         {"agree_B2", {false, FileAccess::read_only, FileOperation::read, 0, 0},
          DataOpVerdict::complete_empty, std::nullopt},
         observe_rejection(IoError{.code = IoError::Code::invalid_state}), false},
        {"C_rejection_vs_success",
         {"agree_C", {false, FileAccess::read_only, FileOperation::read, 0, 4},
          DataOpVerdict::reject_range, IoError::Code::invalid_argument},
         observe_accepted(), false},
        {"D_rejection_wrong_code",
         {"agree_D", {false, FileAccess::read_only, FileOperation::read, 0, 4},
          DataOpVerdict::reject_range, IoError::Code::invalid_argument},
         observe_rejection(IoError{.code = IoError::Code::invalid_state}), false},
        {"D_rejection_exact_code",
         {"agree_D2", {false, FileAccess::read_only, FileOperation::read, 0, 4},
          DataOpVerdict::reject_range, IoError::Code::invalid_argument},
         observe_rejection(IoError{.code = IoError::Code::invalid_argument}), true},
        {"E_complete_empty_vs_dispatched",
         {"agree_E", {false, FileAccess::read_only, FileOperation::read, 0, 0},
          DataOpVerdict::complete_empty, std::nullopt},
         observe_accepted(false), false},
        {"E_complete_empty_vs_noop",
         {"agree_E2", {false, FileAccess::read_only, FileOperation::read, 0, 0},
          DataOpVerdict::complete_empty, std::nullopt},
         observe_accepted(true), true},
        {"E_complete_empty_unobservable",
         {"agree_E3", {false, FileAccess::read_only, FileOperation::read, 0, 0},
          DataOpVerdict::complete_empty, std::nullopt},
         observe_accepted(), true},
        {"noop_observation_ignored_for_execute",
         {"agree_F", {false, FileAccess::read_only, FileOperation::read, 0, 4},
          DataOpVerdict::execute, std::nullopt},
         observe_accepted(false), true},
    };
    for (const AgreeCase& entry : agree_cases) {
        if (agree(entry.scenario, entry.observed) == entry.expected)
            continue;
        std::fprintf(stderr, "SELF-CHECK FAIL [agree] %s: expected %s\n", entry.case_name,
                     entry.expected ? "agreement" : "disagreement");
        return false;
    }

    const Scenario synthetic_scenarios[] = {
        {"selfcheck_expected_execute", {false, FileAccess::read_only, FileOperation::read, 0, 4},
         DataOpVerdict::execute, std::nullopt},
        {"selfcheck_expected_rejection", {true, FileAccess::read_only, FileOperation::read, 0, 4},
         DataOpVerdict::reject_closed, IoError::Code::invalid_state},
    };
    std::vector<const char*> honest_names;
    const std::size_t honest_mismatches = run_path(
        "selfcheck-honest", synthetic_scenarios, 2,
        [](const Input& input) {
            return input.closed
                       ? observe_rejection(IoError{.code = IoError::Code::invalid_state})
                       : observe_accepted();
        },
        &honest_names);
    if (honest_mismatches != 0 || !honest_names.empty()) {
        std::fprintf(stderr,
                     "SELF-CHECK FAIL [run_path] honest observations reported %zu mismatches\n",
                     honest_mismatches);
        return false;
    }
    std::vector<const char*> wrong_names;
    const std::size_t wrong_mismatches = run_path(
        "selfcheck-wrong", synthetic_scenarios, 2,
        [](const Input&) { return observe_accepted(); }, &wrong_names);
    if (wrong_mismatches != 1 || wrong_names.size() != 1 ||
        std::strcmp(wrong_names[0], "selfcheck_expected_rejection") != 0) {
        std::fprintf(stderr,
                     "SELF-CHECK FAIL [run_path] a known wrong observation produced %zu "
                     "mismatches and %zu names\n",
                     wrong_mismatches, wrong_names.size());
        return false;
    }

    const char* const name_a = "closed_zero_length";
    const char* const name_b = "access_illegal_zero_length";
    const char* const name_c = "range_unrepresentable_offset";
    // Two arrays, not two literals: literal pooling may give equal text one
    // address, which would silently stop this case from discriminating
    // pointer identity from content identity.
    char observed_name[] = "zero_length_legal_offset";
    char recorded_name[] = "zero_length_legal_offset";
    struct DivergenceCase {
        const char* case_name;
        std::vector<const char*> observed;
        std::vector<const char*> recorded;
        bool expected;
    };
    const DivergenceCase divergence_cases[] = {
        {"F_both_empty", {}, {}, true},
        {"F_identical", {name_a, name_b}, {name_a, name_b}, true},
        {"same_text_distinct_address", {observed_name}, {recorded_name}, true},
        {"H_reorder", {name_b, name_a}, {name_a, name_b}, true},
        {"G_extra_observed", {name_a}, {}, false},
        {"G_missing_observed", {}, {name_a}, false},
        {"G_different_name", {name_a}, {name_c}, false},
        {"I_duplicate_observed", {name_a, name_a}, {name_a}, false},
        {"I_duplicate_recorded", {name_a}, {name_a, name_a}, false},
        {"I_partial_overlap", {name_a, name_b}, {name_a}, false},
    };
    for (const DivergenceCase& entry : divergence_cases) {
        if (matches_recorded_divergences("selfcheck", entry.observed, entry.recorded) ==
            entry.expected)
            continue;
        std::fprintf(stderr, "SELF-CHECK FAIL [matches_recorded_divergences] %s: expected %s\n",
                     entry.case_name, entry.expected ? "match" : "mismatch");
        return false;
    }
    return true;
}

}
