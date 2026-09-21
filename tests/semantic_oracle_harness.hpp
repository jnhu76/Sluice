#pragma once

// A1 reference harness: one semantic oracle, several execution paths.
//
// The expectation for a scenario is written out from the root requirement in the
// scenario table, not read back from the oracle implementation. Two comparisons
// then run against that same table:
//   - `check_oracle_against_table` checks the shared rules themselves, so a rule
//     regression cannot move both sides of the comparison;
//   - `run_path` checks one execution path, which reports only what it observed.
//
// The comparison is deliberately restricted to what the root fixes and what every
// path can observe: whether the operation was rejected and with which canonical
// error, or was allowed to proceed, or completed as a logical no-op. Byte counts
// and physical traces are the execution's business (BACKEND-01) and are not
// compared here.

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
#include <vector>

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
    // The requirement, written from SEM-03. `expected_rejection_code` is
    // populated exactly when the verdict is a rejection.
    DataOpVerdict expected_verdict;
    std::optional<IoError::Code> expected_rejection_code;
};

// Caller-visible outcome of one path attempt.
struct Observation {
    bool rejected = false;
    IoError error{};
    // Whether the path completed a logical no-op without an OS call (direct) or
    // without a data dispatch (request). Left empty by a path that cannot observe
    // this for the given input; a direct call has nothing outside it that would
    // reveal whether a syscall happened, so it always reports empty here. Only
    // consulted for a `complete_empty` expectation.
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

// The scenario table is the requirement. A path is compared against it, not
// against the oracle implementation, and any rejection where the table expects an
// accepted operation is a mismatch: an accepted operation that the environment
// refuses does not belong in this table, because a valid range refused by a
// filesystem is an operation result rather than a precedence statement.
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

// Compares a path's observed divergences against the divergence set recorded for
// it. Equality is required in both directions: a divergence that closes, and a new
// divergence that appears, both fail, so a recorded gap cannot rot. The recorded
// set lives beside the scenario table and is cited by the conformance ledger; the
// two must be updated together.
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
