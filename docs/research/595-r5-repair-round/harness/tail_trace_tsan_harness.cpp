// #595 R5-repair F1 validation: synthetic writer/reader harness for the v2
// tail-trace instrument, built with -fsanitize=thread against the exact
// production translation unit.
//
// Threads mirror the W03 writers:
//  - submitter pattern: note_built/note_admitted/note_enter_batch/
//    note_cookie/note_stop_kind/note_taken/next_presentation
//  - driver pattern: note_wake/note_cqe/note_published
//  - controller: begin_phase/end_phase cycles with a dump dir set
//
// Slots are reused at high frequency (64 slots x many generations, full
// 64-bit generation space probed with a 2^33 offset so any 32-bit
// truncation would alias), including duplicate (slot,gen) admissions to
// exercise the analyzer's duplicate classification. Compile the production
// TU with a small event cap to exercise overflow accounting.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>

#include <sluice/async/detail/r5_tail_trace.hpp>
#include <sluice/async/detail/request_key.hpp>

using sluice::async::detail::tailtrace::now_ns_i;
using sluice::async::detail::RequestKey;
using SlotIndex = sluice::async::detail::SlotIndex;
using Generation = sluice::async::detail::Generation;
using ContextIdentity = sluice::async::detail::ContextIdentity;
namespace tailtrace = sluice::async::detail::tailtrace;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<std::uint64_t> g_ops{0};
std::atomic<std::uint64_t> g_admitted_seq{0};

constexpr std::uint64_t kGenBase = 1ull << 33; // would alias under 32-bit truncation

void submitter_loop(int id) {
    std::uint64_t gen = kGenBase + static_cast<std::uint64_t>(id);
    std::uint32_t seq = 0;
    std::vector<RequestKey> keys;
    keys.reserve(16);
    while (!g_stop.load(std::memory_order_relaxed)) {
        const std::uint32_t presentation = tailtrace::next_presentation();
        keys.clear();
        const std::size_t batch = 1 + (seq % 16);
        for (std::size_t k = 0; k < batch; ++k) {
            const std::uint32_t slot = (seq + k) % 64;
            tailtrace::note_built(seq + static_cast<std::uint32_t>(k),
                                  static_cast<std::uint32_t>(k), now_ns_i());
            keys.push_back(RequestKey{ContextIdentity{1}, SlotIndex{slot},
                                      Generation{gen}});
            gen += 2; // odd stride: reuse of the same slot separates gens widely
        }
        for (std::size_t k = 0; k < batch; ++k) {
            tailtrace::note_admitted(keys[k].slot.value, keys[k].generation.value,
                                     presentation);
        }
        tailtrace::note_enter_batch(keys.data(), keys.size());
        for (std::size_t k = 0; k < batch; ++k) {
            tailtrace::note_cookie(keys[k].slot.value, keys[k].generation.value,
                                   0x1000ull + (g_admitted_seq.load(std::memory_order_relaxed)));
        }
        tailtrace::note_stop_kind(keys.data(), keys.size(), 1 + (seq % 7));
        for (std::size_t k = 0; k < batch; ++k) {
            tailtrace::note_taken(seq + static_cast<std::uint32_t>(k), now_ns_i());
            g_admitted_seq.fetch_add(1, std::memory_order_relaxed);
        }
        seq += static_cast<std::uint32_t>(batch);
        g_ops.fetch_add(batch, std::memory_order_relaxed);
    }
}

void driver_loop() {
    std::uint64_t gen = kGenBase;
    std::uint32_t slot = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        tailtrace::note_wake();
        for (int i = 0; i < 8; ++i) {
            tailtrace::note_cqe(slot, gen);
            tailtrace::note_published(slot, gen);
            slot = (slot + 7) % 64;
            gen += 2;
        }
    }
}

void duplicate_admitter_loop() {
    // Independent writer hammering the SAME (slot,gen) pairs the driver uses,
    // so duplicate admissions and cross-writer key reuse are exercised.
    std::uint64_t gen = kGenBase;
    std::uint32_t slot = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 4; ++i) {
            tailtrace::note_admitted(slot, gen, 999);
            slot = (slot + 7) % 64;
            gen += 2;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* dump_dir = argc > 1 ? argv[1] : "/tmp/r5rm-tsan-dumps";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 3;
    assert(tailtrace::enabled());
    std::printf("harness: dump_dir=%s seconds=%d\n", dump_dir, seconds);

    std::thread controller([&] {
        int phase = 0;
        while (!g_stop.load(std::memory_order_relaxed)) {
            tailtrace::begin_phase();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            char tag[32];
            std::snprintf(tag, sizeof tag, "hphase-%03d", phase++);
            tailtrace::end_phase(tag);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::vector<std::thread> workers;
    workers.emplace_back(submitter_loop, 0);
    workers.emplace_back(submitter_loop, 1);
    workers.emplace_back(driver_loop);
    workers.emplace_back(duplicate_admitter_loop);

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    g_stop.store(true, std::memory_order_relaxed);
    for (auto& t : workers) {
        t.join();
    }
    controller.join();

    std::printf("harness: ops=%llu\n", static_cast<unsigned long long>(
                                           g_ops.load(std::memory_order_relaxed)));
    return 0;
}
