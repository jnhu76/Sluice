#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace m1 {

inline std::uint64_t xorshift64(std::uint64_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

struct Clock {
    static double now_ns() noexcept {
        const auto t = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count());
    }
};

struct Sample {
    double median_ns = 0.0;
    double min_ns = 0.0;
    double max_ns = 0.0;
    double spread = 0.0;
};

class Collector {
  public:
    void add(double ns_per_op) { samples_.push_back(ns_per_op); }
    Sample summarize() const {
        std::vector<double> s = samples_;
        std::sort(s.begin(), s.end());
        Sample out;
        if (s.empty()) return out;
        out.median_ns = s[s.size() / 2];
        out.min_ns = s.front();
        out.max_ns = s.back();
        out.spread = out.min_ns > 0 ? (out.max_ns - out.min_ns) / out.median_ns : 0.0;
        return out;
    }
    const std::vector<double>& all() const { return samples_; }

  private:
    std::vector<double> samples_;
};

struct ArmResult {
    const char* arm = "";
    bool ok = false;
    const char* failure = "";
    Collector collector;
    std::uint64_t oracle_ops = 0;
};

inline void emit_jsonl(const char* hypothesis, const char* arm, std::size_t capacity,
                       const char* mix, const Sample& s, std::uint64_t oracle_ops, bool ok) {
    const char* env = std::getenv("SLUICE_M1_JSONL");
    if (env == nullptr) return;
    if (FILE* f = std::fopen(env, "a")) {
        std::fprintf(f,
                     "{\"study\":\"m0m1\",\"hypothesis\":\"%s\",\"arm\":\"%s\","
                     "\"capacity\":%zu,\"mix\":\"%s\",\"median_ns_per_op\":%.3f,"
                     "\"min_ns_per_op\":%.3f,\"max_ns_per_op\":%.3f,\"spread\":%.4f,"
                     "\"oracle_ops\":%llu,\"ok\":%s}\n",
                     hypothesis, arm, capacity, mix, s.median_ns, s.min_ns, s.max_ns, s.spread,
                     static_cast<unsigned long long>(oracle_ops), ok ? "true" : "false");
        std::fclose(f);
    }
}

inline int report(const char* hypothesis, const std::vector<ArmResult>& results) {
    int failed = 0;
    std::printf("== %s ==\n", hypothesis);
    for (const ArmResult& r : results) {
        const Sample s = r.collector.summarize();
        std::printf("%-28s ok=%d median_ns/op=%9.3f min=%9.3f max=%9.3f spread=%.3f oracle_ops=%llu\n",
                    r.arm, r.ok ? 1 : 0, s.median_ns, s.min_ns, s.max_ns, s.spread,
                    static_cast<unsigned long long>(r.oracle_ops));
        if (!r.ok) {
            std::printf("%-28s FAILURE: %s\n", "", r.failure);
            ++failed;
        }
    }
    return failed;
}

struct RunConfig {
    std::size_t capacity = 64;
    std::size_t ops = 200000;
    unsigned repetitions = 9;
    unsigned warmup = 2;
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
};

inline RunConfig env_overrides(RunConfig cfg) {
    if (const char* v = std::getenv("SLUICE_M1_OPS")) cfg.ops = std::strtoull(v, nullptr, 10);
    if (const char* v = std::getenv("SLUICE_M1_REPS")) cfg.repetitions = std::strtoul(v, nullptr, 10);
    if (const char* v = std::getenv("SLUICE_M1_WARMUP")) cfg.warmup = std::strtoul(v, nullptr, 10);
    return cfg;
}

template <class Fn>
void run_arm(RunConfig cfg, ArmResult& out, Fn&& arm_once) {
    for (unsigned rep = 0; rep < cfg.warmup + cfg.repetitions; ++rep) {
        const bool measured = rep >= cfg.warmup;
        arm_once(rep, measured);
    }
    out.ok = out.failure[0] == '\0';
}

inline std::vector<std::size_t> capacity_sweep() {
    if (const char* v = std::getenv("SLUICE_M1_CAP")) {
        return {std::strtoull(v, nullptr, 10)};
    }
    return {8, 16, 32, 64, 128, 256, 1024};
}

inline bool solo_selected(const char* arm) {
    const char* s = std::getenv("SLUICE_M1_SOLO");
    return s == nullptr || std::strcmp(s, arm) == 0;
}

}  // namespace m1
