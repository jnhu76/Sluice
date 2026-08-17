// sluice-copy CLI argument parsing implementation.
#include "cli_parse.hpp"

#include <cstdio>
#include <cstring>
#include <limits>

namespace sluice_copy::cli {

namespace {

// Strict unsigned decimal scanner: digits only, explicit overflow check.
// See parse_size() in cli_parse.hpp for the full rejection contract.
bool parse_unsigned_decimal(const char* s, std::size_t& out) {
    if (!s || *s == '\0') return false;
    std::size_t v = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        // Any non-digit (signs, whitespace, unit suffixes) is rejected: no
        // strtoull-style "accept a prefix, ignore the tail" behavior.
        if (*p < '0' || *p > '9') return false;
        unsigned d = static_cast<unsigned>(*p - '0');
        if (v > (std::numeric_limits<std::size_t>::max() - d) / 10) return false;
        v = v * 10 + d;
    }
    if (v == 0) return false;  // zero is not a valid size/worker count
    out = v;
    return true;
}

}  // namespace

int usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s [options] <source> <destination>\n"
        "  --buffer-size <bytes>   per-chunk buffer (default 1 MiB;\n"
        "                          <= %zu bytes)\n"
        "  --pipeline-depth <n>    read-ahead slots (default 1; >1 enables the\n"
        "                          bounded reusable-buffer pipeline, Version B;\n"
        "                          <= %zu, buffer_size*depth <= %zu bytes)\n"
        "  --workers <count>       runtime workers (default 1; <= %u)\n"
        "  --sync none|data|all    durability after copy (default none)\n"
        "  --no-atomic             write the destination directly (old Version A/B\n"
        "                          behavior; a failure may leave a partial file).\n"
        "                          The default is Version C: copy to a temp file in\n"
        "                          the destination directory, then rename atomically\n"
        "  --help                  show this help\n",
        prog, static_cast<std::size_t>(kMaxBufferSize),
        static_cast<std::size_t>(kMaxPipelineDepth),
        static_cast<std::size_t>(kMaxPipelineBytes),
        static_cast<unsigned>(kMaxWorkers));
    return 1;
}

bool parse_size(const char* s, std::size_t& out) {
    return parse_unsigned_decimal(s, out);
}

bool parse_workers(const char* s, unsigned& out) {
    std::size_t v = 0;
    if (!parse_unsigned_decimal(s, v)) return false;
    // Explicit narrowing check BEFORE the size_t -> unsigned conversion; the
    // kMaxWorkers app-level cap is checked afterwards (it is the meaningful
    // bound for a copy tool — an extreme-but-in-range count would still
    // exhaust OS resources).
    if (v > std::numeric_limits<unsigned>::max()) return false;
    if (v > static_cast<std::size_t>(kMaxWorkers)) return false;
    out = static_cast<unsigned>(v);
    return true;
}

bool parse_sync(const char* s, SyncPolicy& out) {
    if (!s) return false;
    if (std::strcmp(s, "none") == 0) { out = SyncPolicy::none; return true; }
    if (std::strcmp(s, "data") == 0) { out = SyncPolicy::data; return true; }
    if (std::strcmp(s, "all") == 0) { out = SyncPolicy::all; return true; }
    return false;
}

// Returns 0 on success (fills args), or a non-zero exit code on usage error.
int parse_args(int argc, char** argv, CliArgs& args) {
    int positionals = 0;
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
        } else if (a == "--pipeline-depth") {
            const char* v = next("--pipeline-depth");
            if (!v || !parse_size(v, args.pipeline_depth)) return usage(argv[0]);
        } else if (a == "--workers") {
            const char* v = next("--workers");
            if (!v || !parse_workers(v, args.workers)) return usage(argv[0]);
        } else if (a == "--sync") {
            const char* v = next("--sync");
            if (!v || !parse_sync(v, args.sync)) return usage(argv[0]);
        } else if (a == "--no-atomic") {
            args.atomic = false;
        } else if (a == "--atomic") {
            args.atomic = true;  // explicit no-op: atomic is the default
        } else if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], a.c_str());
            return usage(argv[0]);
        } else {
            if (positionals == 0) args.src = a;
            else if (positionals == 1) args.dst = a;
            else { std::fprintf(stderr, "%s: extra operand %s\n", argv[0], a.c_str()); return usage(argv[0]); }
            ++positionals;
        }
    }
    if (positionals != 2) return usage(argv[0]);
    return 0;
}

const char* code_name(sluice::IoError::Code c) {
    switch (c) {
    case sluice::IoError::Code::eof: return "eof";
    case sluice::IoError::Code::canceled: return "canceled";
    case sluice::IoError::Code::no_space: return "no_space";
    case sluice::IoError::Code::permission_denied: return "permission_denied";
    case sluice::IoError::Code::invalid_state: return "invalid_state";
    case sluice::IoError::Code::backend_error: return "backend_error";
    default: return "io_error";
    }
}

}  // namespace sluice_copy::cli
