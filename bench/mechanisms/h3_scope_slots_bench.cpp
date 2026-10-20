// H3: RequestScope slot management — first-free linear reserve scan (the
// production shape) vs an LIFO free-slot vector (the mechanism RequestCore
// already uses). Resolve stays the production ticket-id scan in both arms;
// the arms differ in which physical slot a live request lands in, which
// feeds back into resolve scan length under each consumption order.
// Oracle: an independent live-id model must agree on every accept/reject
// and every take result; both arms must end with all slots free.

#include "mechanism_common.hpp"

using namespace m1;

#include <deque>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::uint8_t kFree = 0;
constexpr std::uint8_t kOwning = 1;
constexpr std::uint64_t kCtx = 0xA11CE;

struct Slot {
    std::uint8_t state = kFree;
    std::uint8_t variant_index = 0;
    std::uint8_t pad[6] = {};
    std::uint64_t id_ctx = 0;
    std::uint32_t id_slot = 0;
    std::uint32_t pad2 = 0;
    std::uint64_t id_gen = 0;
    std::uint64_t cold[4] = {};
};
static_assert(sizeof(Slot) == 64, "mirror production RequestScope::Slot width");

struct BaselineScanScope {
    std::vector<Slot> slots;
    explicit BaselineScanScope(std::size_t c) : slots(c) {}
    std::size_t reserve() {
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].state == kFree) {
                slots[i].state = kOwning;
                return i;
            }
        }
        return slots.size();
    }
    void release(std::size_t i) { slots[i] = Slot{}; }
};

struct FreelistScope {
    std::vector<Slot> slots;
    std::vector<std::uint32_t> free_slots;
    explicit FreelistScope(std::size_t c) : slots(c) {
        for (std::size_t i = c; i > 0; --i) free_slots.push_back(static_cast<std::uint32_t>(i - 1));
    }
    std::size_t reserve() {
        if (free_slots.empty()) return slots.size();
        const std::uint32_t i = free_slots.back();
        free_slots.pop_back();
        slots[i].state = kOwning;
        return i;
    }
    void release(std::size_t i) {
        slots[i] = Slot{};
        free_slots.push_back(static_cast<std::uint32_t>(i));
    }
};

struct FreelistNoPushScope {
    std::vector<Slot> slots;
    std::vector<std::uint32_t> free_slots;
    explicit FreelistNoPushScope(std::size_t c) : slots(c) {
        for (std::size_t i = c; i > 0; --i) free_slots.push_back(static_cast<std::uint32_t>(i - 1));
    }
    std::size_t reserve() {
        if (free_slots.empty()) return slots.size();
        const std::uint32_t i = free_slots.back();
        free_slots.pop_back();
        slots[i].state = kOwning;
        return i;
    }
    void release(std::size_t i) { slots[i] = Slot{}; }
};

struct Ticket {
    std::uint64_t gen = 0;
};

template <class Scope>
bool all_free(const Scope& scope) {
    for (const Slot& s : scope.slots) {
        if (s.state != kFree) return false;
    }
    return true;
}

template <class Scope>
std::size_t resolve(const Scope& scope, std::uint64_t gen) {
    for (std::size_t i = 0; i < scope.slots.size(); ++i) {
        const Slot& s = scope.slots[i];
        if (s.state != kOwning) continue;
        if (s.variant_index != 1) continue;
        if (s.id_ctx == kCtx && s.id_gen == gen) return i;
    }
    return scope.slots.size();
}

template <class Scope>
bool run_ops(Scope& scope, std::uint64_t& seed, std::size_t ops, const char* pattern,
             std::deque<std::uint64_t>& live_fifo, std::vector<std::uint64_t>& live_random,
             std::uint64_t& next_gen) {
    const std::string p(pattern);
    const bool saturated = p == "saturated";
    for (std::size_t k = 0; k < ops; ++k) {
        const std::uint64_t r = xorshift64(seed);
        const bool do_take = saturated
                                 ? live_random.size() >= scope.slots.size()
                                 : !live_random.empty() && (r % 100) < 50;
        if (!do_take) {
            const std::size_t slot = scope.reserve();
            if (slot == scope.slots.size()) {
                if (live_random.size() != scope.slots.size()) return false;
                continue;
            }
            if (live_random.size() >= scope.slots.size()) return false;
            Slot& s = scope.slots[slot];
            s.id_ctx = kCtx;
            s.id_slot = static_cast<std::uint32_t>(slot);
            s.id_gen = next_gen;
            s.variant_index = 1;
            live_fifo.push_back(next_gen);
            live_random.push_back(next_gen);
            ++next_gen;
        } else {
            std::uint64_t gen = 0;
            if (p == "fifo") {
                gen = live_fifo.front();
            } else if (p == "lifo") {
                gen = live_random.back();
            } else {
                gen = live_random[r % live_random.size()];
            }
            const std::size_t found = resolve(scope, gen);
            if (found == scope.slots.size()) return false;
            scope.release(found);
            if (p == "fifo") live_fifo.pop_front();
            auto it = std::find(live_random.begin(), live_random.end(), gen);
            live_random.erase(it);
            if (saturated) {
                const std::size_t refill = scope.reserve();
                if (refill == scope.slots.size()) return false;
                Slot& rs = scope.slots[refill];
                rs.id_ctx = kCtx;
                rs.id_slot = static_cast<std::uint32_t>(refill);
                rs.id_gen = next_gen;
                rs.variant_index = 1;
                live_fifo.push_back(next_gen);
                live_random.push_back(next_gen);
                ++next_gen;
            }
        }
    }
    while (!live_random.empty()) {
        const std::uint64_t gen = live_random.front();
        const std::size_t found = resolve(scope, gen);
        if (found == scope.slots.size()) return false;
        scope.release(found);
        live_fifo.pop_front();
        auto it = std::find(live_random.begin(), live_random.end(), gen);
        live_random.erase(it);
    }
    return true;
}

struct Model {
    std::unordered_set<std::uint64_t> live;
    std::size_t capacity = 0;
    std::uint64_t next_gen = 1;

    bool can_accept() const { return live.size() < capacity; }
};

template <class Scope>
bool run_oracle_once(Scope& scope, std::uint64_t& seed, std::size_t ops, const char* pattern) {
    Model model;
    model.capacity = scope.slots.size();
    std::deque<std::uint64_t> live_fifo;
    std::vector<std::uint64_t> live_random;
    const std::string p(pattern);
    const bool saturated = p == "saturated";
    for (std::size_t k = 0; k < ops; ++k) {
        const std::uint64_t r = xorshift64(seed);
        const bool model_has_live = !model.live.empty();
        const bool do_take = saturated
                                 ? model_has_live && model.live.size() >= model.capacity
                                 : model_has_live && (r % 100) < 50;
        if (!do_take) {
            const std::size_t slot = scope.reserve();
            if (model.can_accept()) {
                if (slot == scope.slots.size()) return false;
            } else if (slot != scope.slots.size()) {
                return false;
            }
            if (slot == scope.slots.size()) continue;
            Slot& s = scope.slots[slot];
            s.id_ctx = kCtx;
            s.id_slot = static_cast<std::uint32_t>(slot);
            s.id_gen = model.next_gen;
            s.variant_index = 1;
            model.live.insert(model.next_gen);
            live_fifo.push_back(model.next_gen);
            live_random.push_back(model.next_gen);
            ++model.next_gen;
        } else {
            std::uint64_t gen = 0;
            if (p == "fifo") gen = live_fifo.front();
            else if (p == "lifo") gen = live_random.back();
            else gen = live_random[r % live_random.size()];
            if (model.live.count(gen) == 0) return false;
            const std::size_t found = resolve(scope, gen);
            if (found == scope.slots.size()) return false;
            scope.release(found);
            model.live.erase(gen);
            if (p == "fifo") live_fifo.pop_front();
            auto it = std::find(live_random.begin(), live_random.end(), gen);
            live_random.erase(it);
            if (saturated) {
                const std::size_t refill = scope.reserve();
                if (model.can_accept()) {
                    if (refill == scope.slots.size()) return false;
                } else if (refill != scope.slots.size()) {
                    return false;
                }
                if (refill == scope.slots.size()) continue;
                Slot& rs = scope.slots[refill];
                rs.id_ctx = kCtx;
                rs.id_slot = static_cast<std::uint32_t>(refill);
                rs.id_gen = model.next_gen;
                rs.variant_index = 1;
                model.live.insert(model.next_gen);
                live_fifo.push_back(model.next_gen);
                live_random.push_back(model.next_gen);
                ++model.next_gen;
            }
        }
    }
    while (!live_random.empty()) {
        const std::uint64_t gen = live_random.front();
        if (model.live.count(gen) == 0) return false;
        const std::size_t found = resolve(scope, gen);
        if (found == scope.slots.size()) return false;
        scope.release(found);
        model.live.erase(gen);
        live_fifo.pop_front();
        auto it = std::find(live_random.begin(), live_random.end(), gen);
        live_random.erase(it);
    }
    return model.live.empty() && all_free(scope);
}

void run_timed_arms(const char* arm_name, const std::vector<std::size_t>& caps) {
    const char* kPatterns[] = {"fifo", "random", "lifo", "saturated"};
    if (!solo_selected(arm_name)) return;
    std::printf("== H3 / %s ==\n", arm_name);
    for (std::size_t cap : caps) {
        for (const char* pattern : kPatterns) {
            RunConfig cfg;
            cfg.capacity = cap;
            cfg.ops = 100000;
            cfg = env_overrides(cfg);
            Collector c;
            bool ok = true;
            for (unsigned rep = 0; rep < cfg.warmup + cfg.repetitions; ++rep) {
                std::uint64_t seed = cfg.seed + rep;
                if (std::string_view(arm_name) == "baseline_first_free_scan") {
                    BaselineScanScope scope(cap);
                    std::deque<std::uint64_t> f;
                    std::vector<std::uint64_t> r;
                    std::uint64_t gen = 1;
                    const double t0 = Clock::now_ns();
                    const bool step_ok = run_ops(scope, seed, cfg.ops, pattern, f, r, gen);
                    const double t1 = Clock::now_ns();
                    if (!step_ok || !all_free(scope)) {
                        ok = false;
                        break;
                    }
                    if (rep < cfg.warmup) continue;
                    c.add((t1 - t0) / static_cast<double>(cfg.ops));
                } else {
                    FreelistScope scope(cap);
                    std::deque<std::uint64_t> f;
                    std::vector<std::uint64_t> r;
                    std::uint64_t gen = 1;
                    const double t0 = Clock::now_ns();
                    const bool step_ok = run_ops(scope, seed, cfg.ops, pattern, f, r, gen);
                    const double t1 = Clock::now_ns();
                    if (!step_ok || !all_free(scope) ||
                        scope.free_slots.size() != cap) {
                        ok = false;
                        break;
                    }
                    if (rep < cfg.warmup) continue;
                    c.add((t1 - t0) / static_cast<double>(cfg.ops));
                }
            }
            const Sample s = c.summarize();
            std::printf("cap=%4zu pattern=%-6s %9.2f ns/op spread=%.3f %s\n", cap, pattern,
                        s.median_ns, s.spread, ok ? "selfcheck=PASS" : "SELFCHECK FAIL");
            emit_jsonl("H3", arm_name, cap, pattern, s, cfg.ops, ok);
        }
    }
}

void run_oracle(const char* arm_name, const std::vector<std::size_t>& caps) {
    for (std::size_t cap : caps) {
        RunConfig cfg;
        cfg.capacity = cap;
        cfg.ops = 20000;
        cfg = env_overrides(cfg);
        bool ok = true;
        for (const char* pattern : {"fifo", "random", "lifo", "saturated"}) {
            for (unsigned rep = 0; rep < 3 && ok; ++rep) {
                std::uint64_t seed = cfg.seed + rep;
                if (std::string_view(arm_name) == "baseline_first_free_scan") {
                    BaselineScanScope scope(cap);
                    ok = run_oracle_once(scope, seed, cfg.ops, pattern);
                } else {
                    FreelistScope scope(cap);
                    ok = run_oracle_once(scope, seed, cfg.ops, pattern);
                }
            }
        }
        std::printf("H3 oracle %s cap=%zu: %s\n", arm_name, cap, ok ? "PASS" : "FAIL");
        emit_jsonl("H3", arm_name, cap, "oracle", Sample{}, cfg.ops, ok);
    }
}

struct Inflight {
    Ticket ticket;
    std::size_t slot = 0;
    std::uint64_t offset = 0;
    double submitted_ns = 0.0;
};

void harness_only_control() {
    std::printf("== H3 companion: W-02 harness-only window bookkeeping ==\n");
    for (std::size_t depth : {std::size_t{1}, std::size_t{4}, std::size_t{16}, std::size_t{64}, std::size_t{256}, std::size_t{1024}}) {
        RunConfig cfg;
        cfg.capacity = depth;
        cfg.ops = 200000;
        cfg = env_overrides(cfg);
        Collector c;
        for (unsigned rep = 0; rep < cfg.warmup + cfg.repetitions; ++rep) {
            std::uint64_t seed = cfg.seed + rep;
            std::deque<Inflight> window;
            std::vector<std::size_t> free_slots(depth);
            for (std::size_t i = 0; i < depth; ++i) free_slots[i] = i;
            std::uint64_t checksum = 0;
            const double t0 = Clock::now_ns();
            std::size_t completed = 0;
            while (completed < cfg.ops) {
                while (window.size() < depth) {
                    const std::size_t slot = free_slots.back();
                    free_slots.pop_back();
                    window.push_back(Inflight{Ticket{seed++}, slot, seed, 0.0});
                }
                Inflight entry = window.front();
                window.erase(window.begin());
                checksum += entry.slot + entry.offset;
                free_slots.push_back(entry.slot);
                ++completed;
            }
            const double t1 = Clock::now_ns();
            asm volatile("" ::"r"(checksum) : "memory");
            if (rep < cfg.warmup) continue;
            c.add((t1 - t0) / static_cast<double>(cfg.ops));
        }
        const Sample s = c.summarize();
        std::printf("depth=%4zu harness-only bookkeeping %7.2f ns/op spread=%.3f\n", depth,
                    s.median_ns, s.spread);
        emit_jsonl("H3", "harness_only_window", depth, "fifo", s, cfg.ops, true);
    }
}

}  // namespace

int main() {
    std::printf("H3 negative control (freelist without release push-back): ");
    {
        RunConfig cfg;
        cfg.capacity = 16;
        cfg.ops = 2000;
        FreelistNoPushScope scope(cfg.capacity);
        std::uint64_t seed = cfg.seed;
        std::deque<std::uint64_t> f;
        std::vector<std::uint64_t> r;
        std::uint64_t gen = 1;
        const bool caught = !run_ops(scope, seed, cfg.ops, "fifo", f, r, gen);
        std::printf("%s\n", caught ? "CAUGHT (capacity exhausted while model expects accepts)"
                                   : "NOT CAUGHT — oracle too weak");
        emit_jsonl("H3", "negative_control_no_push", cfg.capacity, "fifo", Sample{}, cfg.ops,
                   caught);
        if (!caught) return 1;
    }
    const std::vector<std::size_t> caps = capacity_sweep();
    run_oracle("baseline_first_free_scan", caps);
    run_oracle("freelist_lifo", caps);
    run_timed_arms("baseline_first_free_scan", caps);
    run_timed_arms("freelist_lifo", caps);
    harness_only_control();
    return 0;
}
