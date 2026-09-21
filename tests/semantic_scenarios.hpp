#pragma once

// The SEM-03 precedence decision table, shared by every path under test so no
// path can be compared against a table of its own.
//
// The table states the required order, not the current code's order:
//   1 closed -> invalid_state
//   2 access -> invalid_argument
//   3 logical no-op -> success 0 with no OS call
//   4 range -> invalid_argument
// The discriminating scenarios are the ones where two steps disagree
// (closed + zero length, illegal access + zero length, illegal access +
// impossible offset, zero length + impossible offset). A path that checks the
// steps in another order fails exactly one of them.

#include "semantic_oracle_harness.hpp"

#include <cstdint>
#include <iterator>
#include <limits>
#include <vector>

namespace sluice_semantic {

inline constexpr std::uint64_t kUnrepresentableOffset =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) + 1;
inline constexpr std::uint64_t kLastRepresentableOffset =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());

inline constexpr Scenario kPrecedenceScenarios[] = {
    // Step 1 outranks every later step.
    {"closed_zero_length", {true, FileAccess::read_only, FileOperation::read, 0, 0, true}},
    {"closed_nonzero_length", {true, FileAccess::read_only, FileOperation::read, 0, 4, true}},
    {"closed_unrepresentable_offset",
     {true, FileAccess::read_only, FileOperation::read, kUnrepresentableOffset, 4, true}},
    {"closed_illegal_access_zero_length",
     {true, FileAccess::write_only, FileOperation::read, 0, 0, true}},

    // Step 2 outranks steps 3 and 4.
    {"access_illegal_zero_length",
     {false, FileAccess::write_only, FileOperation::read, 0, 0, true}},
    {"access_illegal_unrepresentable_offset",
     {false, FileAccess::write_only, FileOperation::read, kUnrepresentableOffset, 4, true}},
    {"access_illegal_write_on_read_only_file",
     {false, FileAccess::read_only, FileOperation::write, 0, 4, true}},

    // Step 3 outranks step 4.
    {"zero_length_legal_offset",
     {false, FileAccess::read_only, FileOperation::read, 0, 0, true}},
    {"zero_length_unrepresentable_offset",
     {false, FileAccess::read_only, FileOperation::read, kUnrepresentableOffset, 0, true}},
    {"zero_length_absent_buffer",
     {false, FileAccess::read_only, FileOperation::read, 0, 0, false}},

    // Step 4.
    {"range_unrepresentable_offset",
     {false, FileAccess::read_only, FileOperation::read, kUnrepresentableOffset, 4, true}},
    {"range_last_byte_overflows",
     {false, FileAccess::read_only, FileOperation::read, kLastRepresentableOffset, 2, true}},
    {"range_absent_buffer_nonzero_length",
     {false, FileAccess::read_only, FileOperation::read, 0, 4, false}},

    // Accepted operations, including one whose range is exactly the last
    // representable byte and one that reads past EOF.
    {"legal_read_at_last_representable_byte",
     {false, FileAccess::read_only, FileOperation::read, kLastRepresentableOffset - 1, 2, true}},
    {"legal_read_past_eof",
     {false, FileAccess::read_only, FileOperation::read, 1u << 20, 4, true}},
    {"legal_read_via_read_write_file",
     {false, FileAccess::read_write, FileOperation::read, 0, 4, true}},
    {"legal_write", {false, FileAccess::read_write, FileOperation::write, 0, 4, true}},
    {"legal_write_only_write",
     {false, FileAccess::write_only, FileOperation::write, 0, 4, true}},

    // State operations travel the same precedence minus the no-op step.
    {"state_sync_data_on_read_only_file",
     {false, FileAccess::read_only, FileOperation::sync_data, 0, 0, true}},
    {"state_sync_all_on_write_only_file",
     {false, FileAccess::write_only, FileOperation::sync_all, 0, 0, true}},
    {"state_resize_on_read_only_file",
     {false, FileAccess::read_only, FileOperation::resize, 0, 0, true}},
    {"state_sync_data_on_closed_file",
     {true, FileAccess::read_write, FileOperation::sync_data, 0, 0, true}},
    {"state_resize_on_closed_file",
     {true, FileAccess::read_write, FileOperation::resize, 0, 0, true}},
};

inline constexpr std::size_t kPrecedenceScenarioCount =
    sizeof(kPrecedenceScenarios) / sizeof(kPrecedenceScenarios[0]);

// A request path currently accepts a logical no-op but does not publish it at
// acceptance, so a zero-length data call is dispatched, which SEM-03 forbids.
inline constexpr const char* kRequestZeroLengthDivergences[] = {
    "zero_length_absent_buffer",
    "zero_length_legal_offset",
    "zero_length_unrepresentable_offset",
};

inline std::vector<const char*> request_zero_length_divergence_set() {
    return {std::begin(kRequestZeroLengthDivergences),
            std::end(kRequestZeroLengthDivergences)};
}

} // namespace sluice_semantic
