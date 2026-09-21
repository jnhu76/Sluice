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
#include "semantic_scenarios.hpp"

#include <cstdio>
#include <memory>
#include <vector>

namespace {

using sluice_semantic::Input;

} // namespace

int main() {
    if (sluice_semantic::check_oracle_against_table(
            sluice_semantic::kPrecedenceScenarios,
            sluice_semantic::kPrecedenceScenarioCount) != 0) {
        std::fprintf(stderr,
                     "FAIL: the shared rules disagree with the frozen precedence table\n");
        return 1;
    }

    sluice_semantic::AccessFixtures fixtures = sluice_semantic::AccessFixtures::create(64);
    if (!fixtures.ok()) {
        std::fprintf(stderr, "FAIL: could not create fixtures\n");
        return 1;
    }

    // Direct execution: every scenario is expressible except an absent buffer
    // with a nonzero length, which std::span cannot describe.
    std::size_t direct_skipped = 0;
    std::vector<const char*> direct_divergences;
    for (std::size_t i = 0; i < sluice_semantic::kPrecedenceScenarioCount; ++i) {
        if (!sluice_semantic::direct_drivable(sluice_semantic::kPrecedenceScenarios[i].input)) {
            ++direct_skipped;
            continue;
        }
        sluice_semantic::run_path("direct", &sluice_semantic::kPrecedenceScenarios[i], 1,
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
    for (std::size_t i = 0; i < sluice_semantic::kPrecedenceScenarioCount; ++i) {
        if (!sluice_semantic::request_drivable(sluice_semantic::kPrecedenceScenarios[i].input)) {
            ++threadpool_skipped;
            continue;
        }
        (void)sluice_semantic::run_path(
            "threadpool", &sluice_semantic::kPrecedenceScenarios[i], 1,
            [&](const Input& input) { return threadpool.attempt(fixtures, input); },
            &threadpool_divergences);
    }
    const std::vector<const char*> recorded_threadpool =
        sluice_semantic::request_zero_length_divergence_set();
    if (!sluice_semantic::matches_recorded_divergences("threadpool", threadpool_divergences,
                                                       recorded_threadpool)) {
        std::fprintf(stderr, "FAIL: ThreadPool divergence set changed; update the ledger row\n");
        return 1;
    }

    std::printf("%zu precedence scenarios: direct %zu compared, %zu not expressible; ThreadPool "
                "%zu compared, %zu not drivable; %zu recorded request-side divergences\n",
                sluice_semantic::kPrecedenceScenarioCount,
                sluice_semantic::kPrecedenceScenarioCount - direct_skipped, direct_skipped,
                sluice_semantic::kPrecedenceScenarioCount - threadpool_skipped, threadpool_skipped,
                recorded_threadpool.size());
    return 0;
}
