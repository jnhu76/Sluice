// H1: io_uring router lookup — reverse linear cookie scan vs direct
// user_data-encoded slot index. Semantics under test: identity re-check,
// stale-cookie miss, control-tag routing, capacity bound, never-reuse
// cookies. A slot-only variant without the cookie re-check is the
// negative control the oracle must catch after slot reuse.

#include "mechanism_common.hpp"

using namespace m1;

#include <cstdint>
#include <vector>

namespace {

constexpr std::uint64_t kControlTag = 1ull << 63;
constexpr std::uint64_t kSlotShift = 48;
constexpr std::uint64_t kCookieMask = (1ull << kSlotShift) - 1;
constexpr std::uint32_t kNotFound = 0xFFFFFFFFu;

struct RouterEntry {
    std::uint64_t cookie = 0;
    std::uint32_t handle_slot = 0;
    std::uint64_t handle_gen = 0;
    std::uint8_t control_state = 0;
    std::uint8_t terminal_delivered = 0;
    std::uint8_t control_pending = 0;
    std::uint8_t in_use = 0;
};
static_assert(sizeof(RouterEntry) == 32, "mirror production RouterEntry width");

struct LiveRecord {
    std::uint64_t user_data = 0;
    std::uint32_t slot = 0;
};

struct RetiredRecord {
    std::uint64_t user_data = 0;
    std::uint32_t slot = 0;
};

struct BaselineLookup {
    const std::vector<RouterEntry>& router;
    std::size_t capacity = 0;
    std::size_t operator()(std::uint64_t user_data) const noexcept {
        const std::uint64_t cookie = user_data & ~kControlTag;
        for (std::size_t i = router.size(); i-- > 0;) {
            if (router[i].in_use && router[i].cookie == cookie) return i;
        }
        return kNotFound;
    }
};

struct DirectLookup {
    const std::vector<RouterEntry>& router;
    std::size_t capacity = 0;
    std::size_t operator()(std::uint64_t user_data) const noexcept {
        const std::size_t slot = static_cast<std::size_t>((user_data >> kSlotShift) & 0x7FFFu);
        const std::uint64_t cookie = user_data & kCookieMask;
        if (slot >= capacity) return kNotFound;
        if (router[slot].in_use == 0 || router[slot].cookie != cookie) return kNotFound;
        return slot;
    }
};

struct DirectLookupNoVerify {
    const std::vector<RouterEntry>& router;
    std::size_t capacity = 0;
    std::size_t operator()(std::uint64_t user_data) const noexcept {
        const std::size_t slot = static_cast<std::size_t>((user_data >> kSlotShift) & 0x7FFFu);
        if (slot >= capacity) return kNotFound;
        return router[slot].in_use ? slot : kNotFound;
    }
};

template <class L>
struct LookupIsNaive {
    static constexpr bool value = false;
};
template <>
struct LookupIsNaive<DirectLookupNoVerify> {
    static constexpr bool value = true;
};

enum class Op { dispatch, complete, stale, control };

struct Sequence {
    std::vector<Op> ops;
    std::vector<std::uint32_t> targets;
};

Sequence make_sequence(std::uint64_t seed, std::size_t ops, double p_dispatch, double p_complete,
                       double p_stale) {
    Sequence seq;
    seq.ops.reserve(ops);
    seq.targets.reserve(ops);
    for (std::size_t i = 0; i < ops; ++i) {
        const double u = static_cast<double>(xorshift64(seed) % 10000u) / 10000.0;
        const std::uint32_t t = static_cast<std::uint32_t>(xorshift64(seed));
        Op op = Op::control;
        if (u < p_dispatch) op = Op::dispatch;
        else if (u < p_dispatch + p_complete) op = Op::complete;
        else if (u < p_dispatch + p_complete + p_stale) op = Op::stale;
        seq.ops.push_back(op);
        seq.targets.push_back(t);
    }
    return seq;
}

struct ArmState {
    std::vector<RouterEntry> router;
    std::vector<std::uint32_t> free_slots;
    std::vector<LiveRecord> live;
    std::vector<RetiredRecord> retired;
    std::uint64_t next_cookie = 1;
    std::uint64_t checksum = 0;
};

std::uint64_t encode(std::uint64_t cookie, std::uint32_t slot, bool direct, bool control) {
    const std::uint64_t tag = control ? kControlTag : 0ull;
    if (!direct) return tag | cookie;
    return tag | (static_cast<std::uint64_t>(slot) << kSlotShift) | (cookie & kCookieMask);
}

template <class Lookup>
bool apply_op(ArmState& st, const Lookup& lookup, Op op, std::uint32_t target, bool direct_arm,
              bool oracle_mode) {
    switch (op) {
    case Op::dispatch: {
        if (st.free_slots.empty()) return true;
        const std::uint32_t slot = st.free_slots.back();
        st.free_slots.pop_back();
        const std::uint64_t cookie = st.next_cookie++;
        if (cookie > kCookieMask) return false;
        st.router[slot] = RouterEntry{};
        st.router[slot].cookie = cookie;
        st.router[slot].handle_slot = slot;
        st.router[slot].handle_gen = cookie * 31;
        st.router[slot].in_use = 1;
        st.live.push_back(LiveRecord{encode(cookie, slot, direct_arm, false), slot});
        return true;
    }
    case Op::complete: {
        if (st.live.empty()) return true;
        const std::size_t idx = target % st.live.size();
        const LiveRecord r = st.live[idx];
        st.live[idx] = st.live.back();
        st.live.pop_back();
        if (oracle_mode && lookup(r.user_data) != r.slot) return false;
        const std::size_t found = lookup(r.user_data);
        if (found != r.slot) return false;
        if (st.router[found].handle_slot != r.slot) return false;
        st.checksum += st.router[found].cookie;
        st.router[found] = RouterEntry{};
        st.free_slots.push_back(r.slot);
        st.retired.push_back(RetiredRecord{r.user_data, r.slot});
        return true;
    }
    case Op::stale: {
        if (st.retired.empty()) return true;
        const RetiredRecord& r = st.retired[target % st.retired.size()];
        if (oracle_mode) {
            const std::size_t found = lookup(r.user_data);
            if (found != kNotFound) return false;
        }
        const std::size_t found = lookup(r.user_data);
        st.checksum += found == kNotFound ? 1u : st.router[found].cookie;
        return true;
    }
    case Op::control: {
        if (st.live.empty()) return true;
        const LiveRecord& r = st.live[target % st.live.size()];
        if (oracle_mode && lookup(r.user_data | kControlTag) != r.slot) return false;
        const std::size_t found = lookup(r.user_data | kControlTag);
        if (found != r.slot) return false;
        st.checksum += st.router[found].control_state;
        return true;
    }
    }
    return true;
}

template <class Lookup, bool Oracle>
double replay_once(const RunConfig& cfg, const Sequence& seq, bool direct_arm) {
    ArmState st;
    st.router.resize(cfg.capacity);
    for (std::size_t i = cfg.capacity; i > 0; --i)
        st.free_slots.push_back(static_cast<std::uint32_t>(i - 1));
    Lookup lookup{st.router, cfg.capacity};
    const double t0 = Clock::now_ns();
    for (std::size_t i = 0; i < seq.ops.size(); ++i) {
        if (!apply_op(st, lookup, seq.ops[i], seq.targets[i], direct_arm, Oracle)) return -1.0;
    }
    const double t1 = Clock::now_ns();
    asm volatile("" ::"r"(st.checksum) : "memory");
    return (t1 - t0) / static_cast<double>(seq.ops.size());
}

struct Mix {
    const char* name;
    double p_dispatch;
    double p_complete;
    double p_stale;
};

int run_hypothesis(const std::vector<std::size_t>& capacities) {
    constexpr Mix kMixes[] = {
        {"lookup", 0.15, 0.70, 0.05},
        {"churn", 0.40, 0.40, 0.10},
        {"miss", 0.15, 0.35, 0.45},
    };
    int failed = 0;
    std::printf("== H1 router lookup: scan vs direct index ==\n");
    for (std::size_t cap : capacities) {
        for (const Mix& mix : kMixes) {
            RunConfig cfg;
            cfg.capacity = cap;
            cfg = env_overrides(cfg);
            Collector base_c, direct_c;
            bool oracle_ok = true;
            const char* failure = "oracle divergence";
            const bool run_base = solo_selected("baseline_scan");
            const bool run_direct = solo_selected("direct_index");
            if (!run_base && !run_direct) continue;
            for (unsigned rep = 0; rep < cfg.warmup + cfg.repetitions; ++rep) {
                const std::uint64_t seed = cfg.seed + rep * 7919;
                const Sequence seq = make_sequence(seed, cfg.ops, mix.p_dispatch, mix.p_complete,
                                                   mix.p_stale);
                if (run_base && replay_once<BaselineLookup, true>(cfg, seq, false) < 0) {
                    oracle_ok = false;
                    break;
                }
                if (run_direct && replay_once<DirectLookup, true>(cfg, seq, true) < 0) {
                    oracle_ok = false;
                    break;
                }
                if (rep < cfg.warmup) continue;
                double b = 0.0;
                double d = 0.0;
                if (run_base) b = replay_once<BaselineLookup, false>(cfg, seq, false);
                if (run_direct) d = replay_once<DirectLookup, false>(cfg, seq, true);
                if (b < 0 || d < 0) {
                    oracle_ok = false;
                    failure = "timed replay failed";
                    break;
                }
                if (run_base) base_c.add(b);
                if (run_direct) direct_c.add(d);
            }
            const Sample bs = base_c.summarize();
            const Sample ds = direct_c.summarize();
            std::printf(
                "cap=%4zu mix=%-6s baseline=%9.2f direct=%9.2f ns/op ratio=%.2fx spread=[%.2f/%.2f] %s\n",
                cap, mix.name, bs.median_ns, ds.median_ns,
                bs.median_ns > 0 ? ds.median_ns / bs.median_ns : 0, bs.spread, ds.spread,
                oracle_ok ? "oracle=PASS" : failure);
            if (run_base) emit_jsonl("H1", "baseline_scan", cap, mix.name, bs, cfg.ops, oracle_ok);
            if (run_direct) emit_jsonl("H1", "direct_index", cap, mix.name, ds, cfg.ops, oracle_ok);
            if (!oracle_ok) ++failed;
        }
    }
    return failed;
}

}  // namespace

int main() {
    int failed = 0;
    std::printf("H1 negative control (no-verify direct lookup, churn mix): ");
    {
        RunConfig cfg;
        cfg.capacity = 64;
        cfg.ops = 50000;
        const Sequence seq = make_sequence(cfg.seed, cfg.ops, 0.40, 0.40, 0.10);
        const bool caught = replay_once<DirectLookupNoVerify, true>(cfg, seq, true) < 0;
        std::printf("%s\n", caught ? "CAUGHT (oracle rejects stale hit after slot reuse)"
                                   : "NOT CAUGHT — oracle too weak");
        emit_jsonl("H1", "negative_control_no_verify", cfg.capacity, "churn", Sample{}, cfg.ops,
                   caught);
        std::printf("H1 positive control (verified direct lookup, same sequence): ");
        const bool passed = replay_once<DirectLookup, true>(cfg, seq, true) >= 0;
        std::printf("%s\n", passed ? "PASS" : "FAIL");
        emit_jsonl("H1", "positive_control_direct", cfg.capacity, "churn", Sample{}, cfg.ops,
                   passed);
        if (!caught || !passed) ++failed;
    }
    failed += run_hypothesis(capacity_sweep());
    std::printf("H1 failed checks: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
