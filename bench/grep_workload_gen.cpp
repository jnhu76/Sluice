// Write a grep-attribution workload to a file (runner helper). Generates the
// exact same bytes as grep_attribution_bench's in-memory stages, so CLI/rg/
// GNU grep comparisons scan identical data.
//
//   grep_workload_gen <name> <out-path> [--bytes N] [--pattern S]
//
// `name` is a workload from the matrix; --pattern OVERRIDES the embedded
// pattern (density stays the workload's). Exit 2 on unknown name.
#include "grep_workloads.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <workload-name> <out-path> [--bytes N] "
                     "[--pattern S]\n",
                     argv[0]);
        return 2;
    }
    std::string name = argv[1];
    const char* out_path = argv[2];
    std::size_t bytes = 64ULL << 20;
    bool override_pat = false;
    std::string pattern;
    for (int i = 3; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--bytes" && i + 1 < argc) {
            bytes = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::string_view(argv[i]) == "--pattern" && i + 1 < argc) {
            pattern = argv[++i];
            override_pat = true;
        }
    }
    auto wls = sluice::bench::grep_wl::matrix(bytes);
    for (auto& w : wls) {
        if (w.name != name) continue;
        if (override_pat) w.pattern = pattern;
        std::string data = sluice::bench::grep_wl::generate(w);
        std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", out_path);
            return 2;
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) {
            std::fprintf(stderr, "write failed: %s\n", out_path);
            return 2;
        }
        return 0;
    }
    std::fprintf(stderr, "unknown workload: %s\n", name.c_str());
    return 2;
}
