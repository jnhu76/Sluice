#include <sluice/async/detail/progress_source.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

using namespace sluice::async;

using DrainObservation = detail::ProgressSource::DrainObservation;

struct Tracker {
    const char* name;
    int failures = 0;

    void check(bool ok, const char* label) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s\n", name, label);
        }
    }
};

std::uint64_t reads(const DrainObservation& observation) {
    return observation.reads.load(std::memory_order_acquire);
}

std::uint64_t eagain_returns(const DrainObservation& observation) {
    return observation.eagain.load(std::memory_order_acquire);
}

std::uint64_t eintr_returns(const DrainObservation& observation) {
    return observation.eintr.load(std::memory_order_acquire);
}

bool successful_drain_consumes_the_counter_in_one_read(Tracker& t) {
    detail::ProgressSource source;
    DrainObservation observation;
    source.set_drain_observation_for_test(&observation);

    source.signal();
    source.acknowledge_notification();
    t.check(reads(observation) == 1, "a non-empty drain issues exactly one read");
    t.check(eagain_returns(observation) == 0,
            "the successful drain does not probe the emptied counter");

    source.signal();
    source.acknowledge_notification();
    t.check(reads(observation) == 2 && eagain_returns(observation) == 0,
            "every notification round trip stays at one read");
    return t.failures == 0;
}

bool empty_drain_takes_the_eagain_branch(Tracker& t) {
    detail::ProgressSource source;
    DrainObservation observation;
    source.set_drain_observation_for_test(&observation);

    source.acknowledge_notification();
    t.check(reads(observation) == 1 && eagain_returns(observation) == 1,
            "an empty drain observes exactly one EAGAIN");
    t.check(eintr_returns(observation) == 0, "no interruption was injected");

    source.signal();
    source.acknowledge_notification();
    t.check(reads(observation) == 2 && eagain_returns(observation) == 1,
            "the notification after an empty drain is consumed in one read");
    return t.failures == 0;
}

bool interrupted_read_retries_without_losing_the_notification(Tracker& t) {
    detail::ProgressSource source;
    DrainObservation observation;
    source.set_drain_observation_for_test(&observation);

    source.signal();
    source.inject_drain_eintr_for_test(1);
    source.acknowledge_notification();
    t.check(eintr_returns(observation) == 1, "the injected interruption took the EINTR branch");
    t.check(reads(observation) == 1, "the retry issued one real read");
    t.check(eagain_returns(observation) == 0, "the retry consumed the notification");

    source.signal();
    source.acknowledge_notification();
    t.check(reads(observation) == 2 && eagain_returns(observation) == 0,
            "the notification after an interrupted drain is consumed in one read");
    return t.failures == 0;
}

bool retired_source_issues_no_drain_read(Tracker& t) {
    detail::ProgressSource source;
    DrainObservation observation;
    source.set_drain_observation_for_test(&observation);

    source.retire_notification();
    source.acknowledge_notification();
    source.signal();
    t.check(reads(observation) == 0 && eagain_returns(observation) == 0 &&
                eintr_returns(observation) == 0,
            "a retired source issues no drain read");
    return t.failures == 0;
}

} // namespace

int main() {
    using Fn = bool (*)(Tracker&);
    struct Case {
        const char* name;
        Fn fn;
    };
    const Case cases[] = {
        {"successful_drain_consumes_the_counter_in_one_read",
         successful_drain_consumes_the_counter_in_one_read},
        {"empty_drain_takes_the_eagain_branch", empty_drain_takes_the_eagain_branch},
        {"interrupted_read_retries_without_losing_the_notification",
         interrupted_read_retries_without_losing_the_notification},
        {"retired_source_issues_no_drain_read", retired_source_issues_no_drain_read},
    };

    int failures = 0;
    for (const auto& c : cases) {
        Tracker tracker{c.name, 0};
        (void)c.fn(tracker);
        failures += tracker.failures;
    }
    return failures == 0 ? 0 : 1;
}
