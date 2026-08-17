// sluice-hash CLI argument parsing implementation.
#include "cli_parse.hpp"

#include <cstdio>
#include <limits>

namespace sluice_hash::cli {

namespace {

// Strict unsigned decimal scanner (digits only, explicit overflow check).
bool parse_unsigned_decimal(const char* s, std::size_t& out) {
    if (!s || *s == '\0') return false;
    std::size_t v = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned d = static_cast<unsigned>(*p - '0');
        if (v > (std::numeric_limits<std::size_t>::max() - d) / 10) return false;
        v = v * 10 + d;
    }
    if (v == 0) return false;
    out = v;
    return true;
}

}  // namespace

int usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s [options] <file>...\n"
        "  --buffer-size <bytes>   read buffer (default 1 MiB; %zu..%zu)\n"
        "  --workers <count>       runtime workers (default 1; <= %u)\n"
        "  --help                  show this help\n",
        prog, static_cast<std::size_t>(kMinBufferSize),
        static_cast<std::size_t>(kMaxBufferSize),
        static_cast<unsigned>(kMaxWorkers));
    return 1;
}

bool parse_size(const char* s, std::size_t& out) {
    return parse_unsigned_decimal(s, out);
}

bool parse_workers(const char* s, unsigned& out) {
    std::size_t v = 0;
    if (!parse_unsigned_decimal(s, v)) return false;
    if (v > std::numeric_limits<unsigned>::max()) return false;
    if (v > static_cast<std::size_t>(kMaxWorkers)) return false;
    out = static_cast<unsigned>(v);
    return true;
}

int parse_args(int argc, char** argv, CliArgs& args) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s: missing value for %s\n", argv[0], opt);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--help") {
            args.help = true;
            return 0;
        } else if (a == "--buffer-size") {
            const char* v = next("--buffer-size");
            if (!v || !parse_size(v, args.buffer_size)) return usage(argv[0]);
        } else if (a == "--workers") {
            const char* v = next("--workers");
            if (!v || !parse_workers(v, args.workers)) return usage(argv[0]);
        } else if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], a.c_str());
            return usage(argv[0]);
        } else {
            args.files.push_back(a);
        }
    }
    if (args.files.empty()) return usage(argv[0]);
    return 0;
}

}  // namespace sluice_hash::cli
