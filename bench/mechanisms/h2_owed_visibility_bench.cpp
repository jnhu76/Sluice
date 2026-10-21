// H2: owed-event visibility — full delivery_ sweeps (per-record flags, the
// production shape) vs a maintained owed count. Arms mirror both production
// sweep shapes (single-lock snapshot; per-slot lock/unlock) plus the
// counter candidate. The pass verdict (any owed?) is measured too. Oracle:
// an independent owed-slot model must agree with every delivered set (sum
// and xor accumulators), every residue verdict, and never observe a stale
// verdict after an empty pass.

#include "mechanism_common.hpp"

using namespace m1;

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

struct RequestKey {
    std::uint64_t ctx = 0;
    std::uint32_t slot = 0;
    std::uint64_t gen = 0;
};

struct DeliveryRecord {
    std::uint8_t kind = 0;
    bool event_owed = false;
    RequestKey owed_key{};
};
static_assert(sizeof(DeliveryRecord) == 32, "mirror production DeliveryRecord width");

struct BaselineSnapshotSweep {
    std::mutex mtx;
    std::vector<DeliveryRecord> delivery;
    std::vector<DeliveryRecord> scratch;

    void set(std::uint32_t slot, const RequestKey& key) {
        std::lock_guard<std::mutex> lk(mtx);
        delivery[slot].event_owed = true;
        delivery[slot].owed_key = key;
        delivery[slot].kind = 1;
    }
    std::size_t pass(std::uint64_t& sum, std::uint64_t& x) {
        std::size_t bound = 0;
        {
            std::lock_guard<std::mutex> lk(mtx);
            for (std::uint32_t i = 0; i < delivery.size(); ++i) {
                DeliveryRecord& r = delivery[i];
                if (!r.event_owed) continue;
                r.event_owed = false;
                scratch[bound++] = r;
                r.owed_key = RequestKey{};
            }
        }
        for (std::size_t n = 0; n < bound; ++n) {
            const std::uint64_t v = scratch[n].owed_key.slot + 1;
            sum += v;
            x ^= v;
        }
        return bound;
    }
    bool any_owed() {
        std::lock_guard<std::mutex> lk(mtx);
        for (const DeliveryRecord& r : delivery) {
            if (r.event_owed) return true;
        }
        return false;
    }
};

struct BaselinePerSlotLockSweep {
    std::mutex mtx;
    std::vector<DeliveryRecord> delivery;

    void set(std::uint32_t slot, const RequestKey& key) {
        std::lock_guard<std::mutex> lk(mtx);
        delivery[slot].event_owed = true;
        delivery[slot].owed_key = key;
        delivery[slot].kind = 1;
    }
    std::size_t pass(std::uint64_t& sum, std::uint64_t& x) {
        std::size_t delivered = 0;
        for (std::uint32_t i = 0; i < delivery.size(); ++i) {
            RequestKey key{};
            {
                std::lock_guard<std::mutex> lk(mtx);
                DeliveryRecord& r = delivery[i];
                if (!r.event_owed) continue;
                r.event_owed = false;
                key = r.owed_key;
                r.owed_key = RequestKey{};
            }
            const std::uint64_t v = key.slot + 1;
            sum += v;
            x ^= v;
            ++delivered;
        }
        return delivered;
    }
    bool any_owed() {
        std::lock_guard<std::mutex> lk(mtx);
        for (const DeliveryRecord& r : delivery) {
            if (r.event_owed) return true;
        }
        return false;
    }
};

struct CounterSnapshotSweep {
    std::mutex mtx;
    std::vector<DeliveryRecord> delivery;
    std::vector<DeliveryRecord> scratch;
    std::size_t owed = 0;

    void set(std::uint32_t slot, const RequestKey& key) {
        std::lock_guard<std::mutex> lk(mtx);
        if (delivery[slot].event_owed) return;
        delivery[slot].event_owed = true;
        delivery[slot].owed_key = key;
        delivery[slot].kind = 1;
        ++owed;
    }
    std::size_t pass(std::uint64_t& sum, std::uint64_t& x) {
        std::size_t bound = 0;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (owed != 0) {
                for (std::uint32_t i = 0; i < delivery.size(); ++i) {
                    DeliveryRecord& r = delivery[i];
                    if (!r.event_owed) continue;
                    r.event_owed = false;
                    scratch[bound++] = r;
                    r.owed_key = RequestKey{};
                    --owed;
                }
            }
        }
        for (std::size_t n = 0; n < bound; ++n) {
            const std::uint64_t v = scratch[n].owed_key.slot + 1;
            sum += v;
            x ^= v;
        }
        return bound;
    }
    bool any_owed() {
        std::lock_guard<std::mutex> lk(mtx);
        return owed != 0;
    }
};

struct CounterNoClearOnDeliver {
    std::mutex mtx;
    std::vector<DeliveryRecord> delivery;
    std::vector<DeliveryRecord> scratch;
    std::size_t owed = 0;

    void set(std::uint32_t slot, const RequestKey& key) {
        std::lock_guard<std::mutex> lk(mtx);
        if (delivery[slot].event_owed) return;
        delivery[slot].event_owed = true;
        delivery[slot].owed_key = key;
        delivery[slot].kind = 1;
        ++owed;
    }
    std::size_t pass(std::uint64_t& sum, std::uint64_t& x) {
        std::size_t bound = 0;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (owed != 0) {
                for (std::uint32_t i = 0; i < delivery.size(); ++i) {
                    DeliveryRecord& r = delivery[i];
                    if (!r.event_owed) continue;
                    r.event_owed = false;
                    scratch[bound++] = r;
                    r.owed_key = RequestKey{};
                }
            }
        }
        for (std::size_t n = 0; n < bound; ++n) {
            const std::uint64_t v = scratch[n].owed_key.slot + 1;
            sum += v;
            x ^= v;
        }
        return bound;
    }
    bool any_owed() {
        std::lock_guard<std::mutex> lk(mtx);
        return owed != 0;
    }
};

struct Model {
    std::vector<bool> owed;
    std::size_t count = 0;

    explicit Model(std::size_t c) : owed(c, false) {}
    bool set(std::uint32_t slot) {
        if (owed[slot]) return false;
        owed[slot] = true;
        ++count;
        return true;
    }
    std::pair<std::uint64_t, std::uint64_t> expected_checksums() const {
        std::uint64_t sum = 0;
        std::uint64_t x = 0;
        for (std::uint32_t i = 0; i < owed.size(); ++i) {
            if (owed[i]) {
                const std::uint64_t v = i + 1;
                sum += v;
                x ^= v;
            }
        }
        return {sum, x};
    }
    bool expect_after_pass(std::size_t delivered, std::uint64_t sum, std::uint64_t x) {
        const auto [esum, ex] = expected_checksums();
        const bool delivery_ok = delivered == count && sum == esum && x == ex;
        owed.assign(owed.size(), false);
        count = 0;
        return delivery_ok;
    }
};

template <class Arm>
bool round(Arm& arm, Model& model, std::uint64_t& seed, std::size_t sets) {
    for (std::size_t k = 0; k < sets; ++k) {
        const std::uint32_t slot = static_cast<std::uint32_t>(xorshift64(seed) % model.owed.size());
        if (model.set(slot)) {
            arm.set(slot, RequestKey{7, slot, slot * 31 + 5});
        }
    }
    std::uint64_t sum = 0;
    std::uint64_t x = 0;
    const std::size_t delivered = arm.pass(sum, x);
    if (!model.expect_after_pass(delivered, sum, x)) return false;
    const bool fresh_set = (xorshift64(seed) % 2) == 0;
    bool fresh_model = false;
    if (fresh_set) {
        const std::uint32_t slot = static_cast<std::uint32_t>(xorshift64(seed) % model.owed.size());
        fresh_model = model.set(slot);
        if (fresh_model) arm.set(slot, RequestKey{7, slot, slot * 31 + 5});
    }
    const bool verdict = arm.any_owed();
    return verdict == fresh_model;
}

template <class Arm>
void run_arm(const char* arm_name, const std::vector<std::size_t>& caps) {
    const char* kMixNames[] = {"idle", "sparse", "half", "full"};
    if (!solo_selected(arm_name)) return;
    std::printf("== H2 / %s ==\n", arm_name);
    for (std::size_t cap : caps) {
        for (const char* mix_name : kMixNames) {
            RunConfig cfg;
            cfg.capacity = cap;
            cfg.ops = 20000;
            cfg = env_overrides(cfg);
            const std::string name(mix_name);
            const std::size_t sets_per_round =
                name == "idle" ? 0 : name == "sparse" ? 1 : name == "half" ? cap / 2 : cap;
            Collector c;
            bool ok = true;
            for (unsigned rep = 0; rep < cfg.warmup + cfg.repetitions; ++rep) {
                Arm arm;
                arm.delivery.resize(cap);
                if constexpr (requires { arm.scratch.resize(cap); }) arm.scratch.resize(cap);
                Model model(cap);
                std::uint64_t seed = cfg.seed + rep;
                const double t0 = Clock::now_ns();
                for (std::size_t r = 0; r < cfg.ops; ++r) {
                    if (!round(arm, model, seed, sets_per_round)) {
                        ok = false;
                        break;
                    }
                }
                const double t1 = Clock::now_ns();
                if (!ok) break;
                if (rep < cfg.warmup) continue;
                c.add((t1 - t0) / static_cast<double>(cfg.ops));
            }
            const Sample s = c.summarize();
            std::printf("cap=%4zu mix=%-6s %9.2f ns/pass spread=%.3f %s\n", cap, mix_name,
                        s.median_ns, s.spread, ok ? "oracle=PASS" : "ORACLE FAIL");
            emit_jsonl("H2", arm_name, cap, mix_name, s, cfg.ops, ok);
        }
    }
}

}  // namespace

int main() {
    std::printf("H2 negative control (counter without deliver-side decrement): ");
    bool caught = false;
    {
        RunConfig cfg;
        cfg.capacity = 64;
        CounterNoClearOnDeliver arm;
        arm.delivery.resize(cfg.capacity);
        arm.scratch.resize(cfg.capacity);
        Model model(cfg.capacity);
        std::uint64_t seed = cfg.seed;
        for (std::size_t r = 0; r < 200 && !caught; ++r) {
            if (!round(arm, model, seed, 4)) caught = true;
        }
        std::printf("%s\n", caught ? "CAUGHT (stale residue verdict diverged from model)"
                                   : "NOT CAUGHT — oracle too weak");
        emit_jsonl("H2", "negative_control_no_clear", cfg.capacity, "sparse", Sample{}, 200,
                   caught);
    }
    std::printf("H2 positive control (counter arm, same driver): ");
    {
        CounterSnapshotSweep arm;
        arm.delivery.resize(64);
        arm.scratch.resize(64);
        Model model(64);
        std::uint64_t seed = 12345;
        bool ok = true;
        for (std::size_t r = 0; r < 5000 && ok; ++r) ok = round(arm, model, seed, 3);
        std::printf("%s\n", ok ? "PASS" : "FAIL");
        emit_jsonl("H2", "positive_control_counter", 64, "sparse", Sample{}, 5000, ok);
        if (!ok) return 1;
    }
    if (!caught) return 1;
    const std::vector<std::size_t> caps = capacity_sweep();
    run_arm<BaselineSnapshotSweep>("baseline_single_lock_sweep", caps);
    run_arm<BaselinePerSlotLockSweep>("baseline_per_slot_lock_sweep", caps);
    run_arm<CounterSnapshotSweep>("counter_snapshot_sweep", caps);
    return 0;
}
