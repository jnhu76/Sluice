#pragma once

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
    {"closed_zero_length", {true, FileAccess::read_only, FileOperation::read, 0, 0},
     kRejectClosed, kInvalidState},
    {"closed_nonzero_length", {true, FileAccess::read_only, FileOperation::read, 0, 4},
     kRejectClosed, kInvalidState},
    {"closed_unrepresentable_offset",
     {true, FileAccess::read_only, FileOperation::read, kUnrepresentableOffset, 4},
     kRejectClosed, kInvalidState},
    {"closed_illegal_access_zero_length",
     {true, FileAccess::write_only, FileOperation::read, 0, 0}, kRejectClosed, kInvalidState},

    {"access_illegal_zero_length",
     {false, FileAccess::write_only, FileOperation::read, 0, 0}, kRejectAccess, kInvalidArgument},
    {"access_illegal_unrepresentable_offset",
     {false, FileAccess::write_only, FileOperation::read, kUnrepresentableOffset, 4},
     kRejectAccess, kInvalidArgument},
    {"access_illegal_write_on_read_only_file",
     {false, FileAccess::read_only, FileOperation::write, 0, 4}, kRejectAccess, kInvalidArgument},

    {"zero_length_legal_offset", {false, FileAccess::read_only, FileOperation::read, 0, 0},
     kNoOp, std::nullopt},
    {"zero_length_unrepresentable_offset",
     {false, FileAccess::read_only, FileOperation::read, kUnrepresentableOffset, 0}, kNoOp,
     std::nullopt},

    {"range_unrepresentable_offset",
     {false, FileAccess::read_only, FileOperation::read, kUnrepresentableOffset, 4},
     kRejectRange, kInvalidArgument},
    {"range_last_byte_overflows",
     {false, FileAccess::read_only, FileOperation::read, kLastRepresentableOffset, 2},
     kRejectRange, kInvalidArgument},

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

inline constexpr const char* kRequestZeroLengthDivergences[] = {
    "zero_length_legal_offset",
    "zero_length_unrepresentable_offset",
};

inline std::vector<const char*> request_zero_length_divergence_set() {
    return {std::begin(kRequestZeroLengthDivergences), std::end(kRequestZeroLengthDivergences)};
}

}
