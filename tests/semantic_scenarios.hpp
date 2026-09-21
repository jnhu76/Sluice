#pragma once

// The SEM-03 precedence decision table, shared by every path under test so no
// path can be compared against a table of its own.
//
// The expectations in this table are written out from the root requirement, not
// read back from `file_semantics.hpp`. The required order is:
//   1 closed -> invalid_state
//   2 access -> invalid_argument
//   3 logical no-op -> success 0, without an OS call
//   4 range -> invalid_argument
// The discriminating scenarios are the ones where two steps disagree
// (closed + zero length, illegal access + zero length, illegal access +
// impossible offset, zero length + impossible offset). A path that checks the
// steps in another order fails exactly one of them.
//
// Buffer presence is deliberately not a scenario here: SEM-03 treats caller
// preconditions such as valid memory as not generally dynamically detectable
// and defines no buffer-presence step, so a null buffer is not a shared
// semantic expectation. A raw-pointer surface may still fail fast on one as its
// own implementation precondition, beside its pointers.
//
// The native representability boundary (last addressed byte == off_t max) is
// deliberately absent here: a filesystem may refuse an extreme but representable
// range, which is an operation result rather than a precedence statement. That
// boundary is covered as a pure property in semantic_range_test.

#include "semantic_oracle_harness.hpp"

#include <cstdint>
#include <iterator>
#include <limits>
#include <vector>

namespace sluice_semantic {

using sluice::IoError;
using sluice::detail::DataOpVerdict;

inline constexpr std::uint64_t kUnrepresentableOffset =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) + 1;
inline constexpr std::uint64_t kLastRepresentableOffset =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());

constexpr IoError::Code kInvalidState = IoError::Code::invalid_state;
constexpr IoError::Code kInvalidArgument = IoError::Code::invalid_argument;
constexpr DataOpVerdict kExecute = DataOpVerdict::execute;
constexpr DataOpVerdict kNoOp = DataOpVerdict::complete_empty;
constexpr DataOpVerdict kRejectClosed = DataOpVerdict::reject_closed;
constexpr DataOpVerdict kRejectAccess = DataOpVerdict::reject_access;
constexpr DataOpVerdict kRejectRange = DataOpVerdict::reject_range;

inline constexpr Scenario kPrecedenceScenarios[] = {
    // Step 1 outranks every later step.
    {"closed_zero_length", {true, FileAccess::read_only, FileOperation::read, 0, 0},
     kRejectClosed, kInvalidState},
    {"closed_nonzero_length", {true, FileAccess::read_only, FileOperation::read, 0, 4},
     kRejectClosed, kInvalidState},
    {"closed_unrepresentable_offset",
     {true, FileAccess::read_only, FileOperation::read, kUnrepresentableOffset, 4},
     kRejectClosed, kInvalidState},
    {"closed_illegal_access_zero_length",
     {true, FileAccess::write_only, FileOperation::read, 0, 0}, kRejectClosed, kInvalidState},

    // Step 2 outranks steps 3 and 4.
    {"access_illegal_zero_length",
     {false, FileAccess::write_only, FileOperation::read, 0, 0}, kRejectAccess, kInvalidArgument},
    {"access_illegal_unrepresentable_offset",
     {false, FileAccess::write_only, FileOperation::read, kUnrepresentableOffset, 4},
     kRejectAccess, kInvalidArgument},
    {"access_illegal_write_on_read_only_file",
     {false, FileAccess::read_only, FileOperation::write, 0, 4}, kRejectAccess, kInvalidArgument},

    // Step 3 outranks step 4.
    {"zero_length_legal_offset", {false, FileAccess::read_only, FileOperation::read, 0, 0},
     kNoOp, std::nullopt},
    {"zero_length_unrepresentable_offset",
     {false, FileAccess::read_only, FileOperation::read, kUnrepresentableOffset, 0}, kNoOp,
     std::nullopt},

    // Step 4.
    {"range_unrepresentable_offset",
     {false, FileAccess::read_only, FileOperation::read, kUnrepresentableOffset, 4},
     kRejectRange, kInvalidArgument},
    {"range_last_byte_overflows",
     {false, FileAccess::read_only, FileOperation::read, kLastRepresentableOffset, 2},
     kRejectRange, kInvalidArgument},

    // Accepted operations, including one that reads past EOF.
    {"legal_read", {false, FileAccess::read_only, FileOperation::read, 0, 4}, kExecute,
     std::nullopt},
    {"legal_read_past_eof",
     {false, FileAccess::read_only, FileOperation::read, 1u << 20, 4}, kExecute, std::nullopt},
    {"legal_read_via_read_write_file",
     {false, FileAccess::read_write, FileOperation::read, 0, 4}, kExecute, std::nullopt},
    {"legal_write", {false, FileAccess::read_write, FileOperation::write, 0, 4}, kExecute,
     std::nullopt},
    {"legal_write_only_write",
     {false, FileAccess::write_only, FileOperation::write, 0, 4}, kExecute, std::nullopt},

    // State operations travel the same precedence minus the no-op step.
    {"state_sync_data_on_read_only_file",
     {false, FileAccess::read_only, FileOperation::sync_data, 0, 0}, kExecute, std::nullopt},
    {"state_sync_all_on_write_only_file",
     {false, FileAccess::write_only, FileOperation::sync_all, 0, 0}, kExecute, std::nullopt},
    {"state_resize_on_read_only_file",
     {false, FileAccess::read_only, FileOperation::resize, 0, 0}, kRejectAccess,
     kInvalidArgument},
    {"state_sync_data_on_closed_file",
     {true, FileAccess::read_write, FileOperation::sync_data, 0, 0}, kRejectClosed,
     kInvalidState},
    {"state_resize_on_closed_file",
     {true, FileAccess::read_write, FileOperation::resize, 0, 0}, kRejectClosed, kInvalidState},
};

inline constexpr std::size_t kPrecedenceScenarioCount =
    sizeof(kPrecedenceScenarios) / sizeof(kPrecedenceScenarios[0]);

// Divergences recorded for a request path, cited by
// docs/roadmap/v1-conformance.md. A request path accepts a logical no-op but does
// not publish it at acceptance, so a zero-length data call is dispatched, which
// SEM-03 forbids. Both request backends are asserted against this exact set.
inline constexpr const char* kRequestZeroLengthDivergences[] = {
    "zero_length_legal_offset",
    "zero_length_unrepresentable_offset",
};

inline std::vector<const char*> request_zero_length_divergence_set() {
    return {std::begin(kRequestZeroLengthDivergences), std::end(kRequestZeroLengthDivergences)};
}

} // namespace sluice_semantic
