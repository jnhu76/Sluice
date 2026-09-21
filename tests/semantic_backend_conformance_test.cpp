// SEM-03 precedence, compared on the real io_uring execution path.
//
// The scenarios and the oracle expectations are the same objects the direct and
// ThreadPool comparison uses, so this file adds a mechanism, not a contract. It
// is registered only under --liburing=y; when the kernel refuses io_uring setup
// the run is reported NOT RUN rather than passed.
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

} // namespace

int main() {
    if (!UringAsyncBackend().available()) {
        std::printf("NOT RUN: io_uring unavailable on this host (kernel/policy blocked)\n");
        return 0;
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

    std::printf("all %zu precedence scenarios passed on io_uring (%zu not drivable, %zu recorded "
                "request-side divergences)\n",
                sluice_semantic::kPrecedenceScenarioCount, skipped, divergences.size());
    return 0;
}
