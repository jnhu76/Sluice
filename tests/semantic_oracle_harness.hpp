#pragma once

// A1 reference harness: one semantic oracle, several execution paths.
//
// An execution path never carries its own expected outcome. The harness derives
// the requirement from the shared oracle, the path under test reports only what
// it observed, and the harness compares the two. Adding a backend means adding
// an adapter — never another expectation table.
//
// The comparison is deliberately restricted to what SEM-03 fixes and what every
// path can observe: whether the operation was rejected, with which canonical
// error, or was allowed to proceed, or completed as a logical no-op without an
// OS call. Byte counts and physical traces are the execution's business
// (BACKEND-01) and are not compared here.

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
    bool buffer_present = true;
};

struct Expectation {
    DataOpVerdict verdict = DataOpVerdict::execute;
    std::optional<IoError> rejection_error;
};

inline Expectation expected_for(const Input& input) {
    Expectation expectation;
    expectation.verdict = sluice::detail::precheck_data_op(
        {input.closed, input.access, input.operation, input.offset, input.length,
         input.buffer_present});
    expectation.rejection_error = sluice::detail::rejection_of(expectation.verdict);
    return expectation;
}

// file_info / resize / sync_data / sync_all travel the same harness: they have
// no length, so they can never reach the logical-no-op verdict.
inline Expectation expected_for_state(const Input& input) {
    Expectation expectation;
    expectation.verdict = sluice::detail::precheck_state_op(input.closed, input.access,
                                                           input.operation);
    expectation.rejection_error = sluice::detail::rejection_of(expectation.verdict);
    return expectation;
}

// Caller-visible outcome of one path attempt.
struct Observation {
    bool rejected = false;
    IoError error{};
    // The operation completed without an OS call (direct) or without dispatch
    // (request). Only a logical no-op is allowed to report this.
    bool short_circuited = false;
};

inline Observation observe_rejection(IoError error) {
    return Observation{true, error, false};
}

inline Observation observe_accepted(bool short_circuited = false) {
    return Observation{false, IoError{}, short_circuited};
}

inline Observation observe_result(const sluice::Result<std::size_t>& result,
                                 bool short_circuited = false) {
    if (result.has_value())
        return observe_accepted(short_circuited);
    return observe_rejection(result.error());
}

inline Observation observe_result(const sluice::Result<void>& result) {
    if (result.has_value())
        return observe_accepted();
    return observe_rejection(result.error());
}

inline bool agree(const Expectation& expected, const Observation& observed) {
    if (expected.rejection_error.has_value()) {
        return observed.rejected && observed.error.code == expected.rejection_error->code;
    }
    if (observed.rejected) {
        // `execute` means the operation must reach the execution, not that the
        // environment must succeed. An operation-result failure carries native
        // detail; a synthesized validation/admission rejection does not, so the
        // two are distinguishable and only the second is a precedence defect.
        return observed.error.os_errno != 0;
    }
    if (expected.verdict == DataOpVerdict::complete_empty)
        return observed.short_circuited;
    return true;
}

inline const char* describe(DataOpVerdict verdict) {
    switch (verdict) {
    case DataOpVerdict::execute:
        return "execute";
    case DataOpVerdict::complete_empty:
        return "complete_empty(no OS call)";
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
                      observed.short_circuited ? "accepted(no OS call)" : "accepted");
        return;
    }
    std::snprintf(buffer, size, "rejected(%s)", sluice::to_string(observed.error.code).data());
}

struct Scenario {
    const char* name;
    Input input;
};

inline Expectation expected_for_scenario(const Input& input) {
    if (sluice::detail::is_byte_operation(input.operation))
        return expected_for(input);
    return expected_for_state(input);
}

// Runs every scenario's oracle expectation against one path adapter. The adapter
// receives the Input and returns what that path observed; it must not consult
// the expectation. Returns the number of mismatches, prints each one, and
// optionally appends the mismatched scenario names for gap pinning.
template <class Attempt>
std::size_t run_path(const char* path_name, const Scenario* scenarios, std::size_t count,
                     Attempt&& attempt, std::vector<const char*>* mismatched_names = nullptr) {
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Expectation expected = expected_for_scenario(scenarios[i].input);
        const Observation observed = attempt(scenarios[i].input);
        if (agree(expected, observed))
            continue;
        char observed_text[64];
        describe(observed, observed_text, sizeof(observed_text));
        std::fprintf(stderr, "MISMATCH [%s] %s: oracle requires %s, path observed %s\n", path_name,
                     scenarios[i].name, describe(expected.verdict), observed_text);
        if (mismatched_names != nullptr)
            mismatched_names->push_back(scenarios[i].name);
        ++mismatches;
    }
    return mismatches;
}

// Compares a path's observed divergences against the set recorded for it in the
// ledger. Equality is required in both directions: a divergence that closes, and
// a new divergence that appears, both fail, so the recorded gap set cannot rot.
inline bool name_less(const char* a, const char* b) {
    return std::strcmp(a, b) < 0;
}

inline bool matches_recorded_divergences(const char* path_name,
                                        std::vector<const char*> observed,
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
