#if defined(SLUICE_HAS_LIBURING)
#include <sluice/async/uring_backend.hpp>
#endif

#include "semantic_path_probes.hpp"
#include "semantic_scenarios.hpp"

#include <cstdio>
#include <memory>
#include <vector>

namespace {

using sluice_semantic::Input;

}

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

    // The MISMATCH lines this section prints are expected and asserted
    // explicitly; an empty recorded set is the acceptance condition.
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
    if (!sluice_semantic::matches_recorded_divergences("threadpool", threadpool_divergences, {})) {
        std::fprintf(stderr,
                     "FAIL: ThreadPool diverges from the oracle; the B1-B cutover regressed it\n");
        return 1;
    }
    const std::size_t recorded_threadpool = 0;

    std::size_t uring_compared = 0;
    std::size_t uring_skipped = 0;
#if defined(SLUICE_HAS_LIBURING)
    sluice::async::UringAsyncBackend availability_probe(sluice::async::UringConfig{8, 8});
    if (!availability_probe.available()) {
        std::fprintf(stderr, "NOT RUN: io_uring unavailable on this host (kernel/policy "
                             "blocked); the precedence oracle ran without the uring half\n");
    } else {
        sluice_semantic::RequestProbe uring_probe(
            std::make_unique<sluice::async::UringAsyncBackend>(sluice::async::UringConfig{8, 8}));
        std::vector<const char*> uring_divergences;
        for (std::size_t i = 0; i < sluice_semantic::kPrecedenceScenarioCount; ++i) {
            if (!sluice_semantic::request_drivable(sluice_semantic::kPrecedenceScenarios[i].input)) {
                ++uring_skipped;
                continue;
            }
            (void)sluice_semantic::run_path(
                "uring", &sluice_semantic::kPrecedenceScenarios[i], 1,
                [&](const Input& input) { return uring_probe.attempt(fixtures, input); },
                &uring_divergences);
        }
        if (!sluice_semantic::matches_recorded_divergences("uring", uring_divergences, {})) {
            std::fprintf(stderr,
                         "FAIL: io_uring diverges from the oracle; the B1-C cutover regressed "
                         "it\n");
            return 1;
        }
        uring_compared = sluice_semantic::kPrecedenceScenarioCount - uring_skipped;
    }
#endif
    const std::size_t recorded_uring = 0;

    std::printf("%zu precedence scenarios: direct %zu compared, %zu not expressible; ThreadPool "
                "%zu compared, %zu not drivable; io_uring %zu compared, %zu not drivable; %zu "
                "recorded request-side divergences\n",
                sluice_semantic::kPrecedenceScenarioCount,
                sluice_semantic::kPrecedenceScenarioCount - direct_skipped, direct_skipped,
                sluice_semantic::kPrecedenceScenarioCount - threadpool_skipped, threadpool_skipped,
                uring_compared, uring_skipped,
                recorded_threadpool + recorded_uring);
    return 0;
}
