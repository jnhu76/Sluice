// SEM-06 durability reference rules, with the observable half of V27 exercised
// on a real file.
//
// The rules are pure functions over a mutation/sync history; the sequence field
// stands in for happens-before, not for a runtime counter. V16 and V17 are
// request-path schedules that the direct path cannot express (a direct operation
// has completed before the call returns and before any later call is initiated),
// so their production evidence belongs to the request slice; the rules
// themselves are asserted here, and the ledger records which half has which
// evidence.
//
// A real sync cannot be shown to survive power loss from inside a process. The
// filesystem cases below therefore assert the observable half only: the call
// sequence succeeds, the file-size change is visible afterwards, and no
// durability fact is claimed by the resize itself.
#include <sluice/blocking/file.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/file_resource.hpp>

#include <cstdio>
#include <string>

#include <unistd.h>

namespace {

using sluice::detail::CompletionState;
using sluice::detail::covers;
using sluice::detail::grants_durability_alone;
using sluice::detail::MutationKind;
using sluice::detail::MutationRecord;
using sluice::detail::ordered_after_sync;
using sluice::detail::preserves_exact_state;
using sluice::detail::SyncKind;
using sluice::detail::SyncRecord;

constexpr MutationRecord kSubmittedWrite{MutationKind::write, CompletionState::submitted, 0};
constexpr MutationRecord kObservedWrite{MutationKind::write, CompletionState::observed, 4};
constexpr MutationRecord kObservedShrink{MutationKind::resize_shrink, CompletionState::observed, 4};
constexpr MutationRecord kObservedGrow{MutationKind::resize_grow, CompletionState::observed, 4};
constexpr MutationRecord kObservedMetadata{MutationKind::metadata, CompletionState::observed, 4};
constexpr SyncRecord kDataSync{SyncKind::data, true, 8};
constexpr SyncRecord kAllSync{SyncKind::all, true, 8};

// V16: a write merely submitted before the sync was initiated is not covered, for
// either sync kind.
bool v16_submitted_write_is_not_covered() {
    if (covers(kDataSync, kSubmittedWrite))
        return false;
    return !covers(kAllSync, kSubmittedWrite);
}

// V27: a completed file-size change is covered without any covered write, in
// both directions, by both sync kinds.
bool v27_completed_resize_is_covered() {
    if (!covers(kDataSync, kObservedShrink) || !covers(kAllSync, kObservedShrink))
        return false;
    return covers(kDataSync, kObservedGrow) && covers(kAllSync, kObservedGrow);
}

// V27 negative rule: a resize on its own establishes no durability, and neither
// does a completed write or metadata change.
bool v27_resize_alone_grants_no_durability() {
    if (grants_durability_alone(kObservedShrink) || grants_durability_alone(kObservedGrow))
        return false;
    if (grants_durability_alone(kObservedWrite))
        return false;
    return !grants_durability_alone(kObservedMetadata);
}

// V17: an observed write before the sync is covered, while a conflicting
// mutation completing after the sync's initiation is not covered. Supersession
// is derived from the two facts SEM-06 names — the earlier covered state and a
// conflicting mutation ordered after the sync's initiation — which is why
// coverage is distinguishable from exact-state preservation.
bool v17_coverage_is_not_a_snapshot() {
    if (!covers(kDataSync, kObservedWrite))
        return false;
    constexpr MutationRecord kConflicting{MutationKind::write, CompletionState::observed, 12};
    if (covers(kDataSync, kConflicting))
        return false;
    if (!ordered_after_sync(kDataSync, kConflicting))
        return false;
    if (ordered_after_sync(kDataSync, kObservedWrite))
        return false;
    return !preserves_exact_state(kDataSync, kConflicting);
}

// Coverage is ordered per mutation and the order is strict. A mutation whose
// sequence equals the sync's initiation is unordered in the model, so no coverage
// is claimed for it: an unordered pair supports neither a positive nor a negative
// durability fact.
bool coverage_requires_a_strict_order() {
    constexpr MutationRecord kBefore{MutationKind::write, CompletionState::observed, 7};
    constexpr MutationRecord kEqual{MutationKind::write, CompletionState::observed, 8};
    constexpr MutationRecord kAfter{MutationKind::write, CompletionState::observed, 9};
    return covers(kDataSync, kBefore) && !covers(kDataSync, kEqual) && !covers(kDataSync, kAfter);
}

// The unordered half stated for the ordering rule itself: with equal sequence
// numbers the model can order neither event against the other, so the pair is
// neither covered nor ordered after, and no supersession follows from it.
bool unordered_pair_supports_no_durability_fact() {
    constexpr MutationRecord kEqual{MutationKind::write, CompletionState::observed, 8};
    return !covers(kDataSync, kEqual) && !ordered_after_sync(kDataSync, kEqual);
}

// Cancellation grants no positive or negative durability fact, and metadata needs
// sync_all.
bool failed_sync_covers_nothing_and_metadata_needs_sync_all() {
    constexpr SyncRecord kFailedDataSync{SyncKind::data, false, 8};
    constexpr SyncRecord kFailedAllSync{SyncKind::all, false, 8};
    if (covers(kFailedDataSync, kObservedWrite) || covers(kFailedAllSync, kObservedWrite))
        return false;
    if (covers(kFailedDataSync, kObservedShrink))
        return false;
    if (covers(kDataSync, kObservedMetadata))
        return false;
    return covers(kAllSync, kObservedMetadata);
}

// ── Real file: the observable half of V27 on the direct path ───────────────

std::string make_temp_file() {
    char path[] = "/tmp/sluice_durability_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    ::close(fd);
    return path;
}

sluice::FileOpen read_write_mode() {
    sluice::FileOpen mode;
    mode.access = sluice::FileAccess::read_write;
    return mode;
}

// Direct V27: grow, shrink, then sync_data and sync_all without any covered
// write. Both syncs must succeed on a regular file after a completed size
// change, and the size change must be observable.
bool direct_resize_then_sync_sequence_succeeds() {
    const std::string path = make_temp_file();
    if (path.empty())
        return false;
    auto opened = sluice::File::open(path, read_write_mode());
    if (!opened.has_value()) {
        ::unlink(path.c_str());
        return false;
    }
    sluice::File file = std::move(opened.value());
    ::unlink(path.c_str());

    if (!sluice::blocking::resize(file, 4096).has_value())
        return false;
    if (!sluice::blocking::sync_data(file).has_value())
        return false;
    if (!sluice::blocking::sync_all(file).has_value())
        return false;

    if (!sluice::blocking::resize(file, 1024).has_value())
        return false;
    if (!sluice::blocking::sync_data(file).has_value())
        return false;
    if (!sluice::blocking::sync_all(file).has_value())
        return false;

    auto size = sluice::blocking::size(file);
    return size.has_value() && size.value() == 1024;
}

// A sync that fails reports failure rather than fabricating success. The
// descriptor is valid but is not a synchronizable regular file.
bool failed_sync_is_reported(int* attempted) {
    *attempted = 0;
    if (::access("/dev/null", W_OK) != 0)
        return true;
    *attempted = 1;
    sluice::FileOpen mode;
    mode.access = sluice::FileAccess::write_only;
    mode.existence = sluice::FileExistence::open_existing;
    auto opened = sluice::File::open("/dev/null", mode);
    if (!opened.has_value())
        return false;
    sluice::File file = std::move(opened.value());

    auto synced = sluice::blocking::sync_all(file);
    if (synced.has_value())
        return false;
    // The failure carries native detail and is not a fabricated success.
    return synced.error().os_errno != 0;
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"v16_submitted_write_is_not_covered", v16_submitted_write_is_not_covered},
        {"v27_completed_resize_is_covered", v27_completed_resize_is_covered},
        {"v27_resize_alone_grants_no_durability", v27_resize_alone_grants_no_durability},
        {"v17_coverage_is_not_a_snapshot", v17_coverage_is_not_a_snapshot},
        {"coverage_requires_a_strict_order", coverage_requires_a_strict_order},
        {"unordered_pair_supports_no_durability_fact", unordered_pair_supports_no_durability_fact},
        {"failed_sync_covers_nothing_and_metadata_needs_sync_all",
         failed_sync_covers_nothing_and_metadata_needs_sync_all},
        {"direct_resize_then_sync_sequence_succeeds", direct_resize_then_sync_sequence_succeeds},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    int attempted = 0;
    if (!failed_sync_is_reported(&attempted)) {
        std::fprintf(stderr, "FAIL: failed_sync_is_reported\n");
        return 1;
    }
    if (attempted == 0) {
        std::printf("NOT RUN: failed_sync_is_reported (/dev/null unavailable)\n");
    }

    std::printf("all %zu durability reference tests passed; %d filesystem sync case ran\n",
                sizeof(tests) / sizeof(tests[0]), attempted);
    return 0;
}
