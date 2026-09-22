#include "semantic_path_probes.hpp"
#include "semantic_scenarios.hpp"

#include <sluice/async/uring_backend.hpp>

#include <cstdio>
#include <memory>
#include <vector>

#ifndef SLUICE_HAS_LIBURING
#error "the io_uring conformance consumer must receive SLUICE_HAS_LIBURING through sluice_async"
#endif

namespace {

using sluice::async::UringAsyncBackend;

}


// A kernel that refuses io_uring setup must produce NOT RUN, never a vacuous pass.
int main() {
    if (!UringAsyncBackend().available()) {
        std::printf("NOT RUN: io_uring unavailable on this host (kernel/policy blocked)\n");
        return 0;
    }

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

    sluice_semantic::RequestProbe uring(std::make_unique<UringAsyncBackend>());
    std::vector<const char*> divergences;
    std::size_t skipped = 0;
    for (std::size_t i = 0; i < sluice_semantic::kPrecedenceScenarioCount; ++i) {
        if (!sluice_semantic::request_drivable(sluice_semantic::kPrecedenceScenarios[i].input)) {
            ++skipped;
            continue;
        }
        (void)sluice_semantic::run_path(
            "uring", &sluice_semantic::kPrecedenceScenarios[i], 1,
            [&](const sluice_semantic::Input& input) { return uring.attempt(fixtures, input); },
            &divergences);
    }

    if (!sluice_semantic::matches_recorded_divergences(
            "uring", divergences, sluice_semantic::request_zero_length_divergence_set())) {
        std::fprintf(stderr, "FAIL: io_uring divergence set changed; update the ledger row\n");
        return 1;
    }

    std::printf("%zu precedence scenarios: io_uring %zu compared, %zu not drivable; %zu recorded "
                "request-side divergences\n",
                sluice_semantic::kPrecedenceScenarioCount,
                sluice_semantic::kPrecedenceScenarioCount - skipped, skipped, divergences.size());
    return 0;
}
