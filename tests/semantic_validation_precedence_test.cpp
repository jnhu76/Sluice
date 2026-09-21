// SEM-03 validation precedence, frozen as a decision table and compared across
// execution paths through the one shared oracle.
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
#include "semantic_path_probes.hpp"

#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

namespace {

using sluice::FileAccess;
using sluice::detail::FileOperation;
using sluice_semantic::Input;
using sluice_semantic::Scenario;

constexpr std::uint64_t kUnrepresentableOffset =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) + 1;
constexpr std::uint64_t kLastRepresentableOffset =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());

const Scenario kScenarios[] = {
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

constexpr std::size_t kScenarioCount = sizeof(kScenarios) / sizeof(kScenarios[0]);

} // namespace

int main() {
    sluice_semantic::AccessFixtures fixtures = sluice_semantic::AccessFixtures::create(64);
    if (!fixtures.ok()) {
        std::fprintf(stderr, "FAIL: could not create fixtures\n");
        return 1;
    }

    // Direct execution: every scenario is expressible except an absent buffer
    // with a nonzero length, which std::span cannot describe.
    std::size_t direct_skipped = 0;
    std::vector<const char*> direct_divergences;
    for (std::size_t i = 0; i < kScenarioCount; ++i) {
        if (!sluice_semantic::direct_drivable(kScenarios[i].input)) {
            ++direct_skipped;
            continue;
        }
        sluice_semantic::run_path("direct", &kScenarios[i], 1,
                                  [&](const Input& input) {
                                      return sluice_semantic::direct_attempt(fixtures, input);
                                  },
                                  &direct_divergences);
    }
    if (!sluice_semantic::matches_recorded_divergences("direct", direct_divergences, {})) {
        std::fprintf(stderr, "FAIL: direct execution diverges from the oracle\n");
        return 1;
    }

    // ThreadPool request execution. A request that reaches the logical-no-op
    // verdict is accepted but still dispatches a zero-length data syscall, which
    // SEM-03 forbids; both zero-length scenarios are therefore recorded
    // divergences (see docs/roadmap/v1-conformance.md, ERR/SEM A1 gap row). The
    // recorded set is asserted exactly, so the test fails once the gap closes
    // and must then be updated together with the ledger.
    sluice_semantic::RequestProbe threadpool(
        std::make_unique<sluice::async::ThreadPoolBackend>(
            sluice::async::ThreadPoolConfig{8, 2}));
    std::vector<const char*> threadpool_divergences;
    std::size_t threadpool_skipped = 0;
    for (std::size_t i = 0; i < kScenarioCount; ++i) {
        if (!sluice_semantic::request_drivable(kScenarios[i].input)) {
            ++threadpool_skipped;
            continue;
        }
        (void)sluice_semantic::run_path(
            "threadpool", &kScenarios[i], 1,
            [&](const Input& input) { return threadpool.attempt(fixtures, input); },
            &threadpool_divergences);
    }
    const std::vector<const char*> recorded_threadpool{
        "zero_length_absent_buffer",
        "zero_length_legal_offset",
        "zero_length_unrepresentable_offset",
    };
    if (!sluice_semantic::matches_recorded_divergences("threadpool", threadpool_divergences,
                                                       recorded_threadpool)) {
        std::fprintf(stderr, "FAIL: ThreadPool divergence set changed; update the ledger row\n");
        return 1;
    }

    std::printf("all %zu precedence scenarios passed on direct (%zu not expressible) and "
                "ThreadPool (%zu not drivable, %zu recorded request-side divergences)\n",
                kScenarioCount, direct_skipped, threadpool_skipped, recorded_threadpool.size());
    return 0;
}
