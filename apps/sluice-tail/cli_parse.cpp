// sluice-tail CLI argument parsing implementation.
#include "cli_parse.hpp"

#include <cstdio>
#include <limits>

namespace sluice_tail::cli {

namespace {

bool parse_digits(const char* s, std::size_t& out) {
    if (!s || *s == '\0') return false;
    std::size_t v = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned d = static_cast<unsigned>(*p - '0');
        if (v > (std::numeric_limits<std::size_t>::max() - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;  // zero allowed (-n 0)
}

}  // namespace

int usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s [options] <file>\n"
        "  -n <count>              last N lines (default 10; 0 = none)\n"
        "  -f                      follow: keep reading after EOF until Ctrl-C\n"
        "  --poll-interval <ms>    follow poll cadence (%u..%u, default %u)\n"
        "  --buffer-size <bytes>   scan/read buffer (default 64 KiB; %zu..%zu)\n"
        "  --max-line-bytes <n>    retained-line cap; longer lines are reported\n"
        "                          and skipped (default %zu; <= %zu)\n"
        "  --workers <count>       runtime workers (default 1; <= %u)\n"
        "  --help                  show this help\n"
        "\n"
        "Exit codes: 0 = success (follow ended by signal counts as success),\n"
        "1 = usage error, 2 = I/O error.\n",
        prog, static_cast<unsigned>(kMinPollMs),
        static_cast<unsigned>(kMaxPollMs), 200u,
        static_cast<std::size_t>(kMinBufferSize),
        static_cast<std::size_t>(kMaxBufferSize),
        static_cast<std::size_t>(kDefaultMaxLineBytes),
        static_cast<std::size_t>(kMaxMaxLineBytes),
        static_cast<unsigned>(kMaxWorkers));
    return 1;
}

bool parse_count(const char* s, std::size_t& out) {
    return parse_digits(s, out);
}

bool parse_workers(const char* s, unsigned& out) {
    std::size_t v = 0;
    if (!parse_digits(s, v) || v == 0) return false;
    if (v > std::numeric_limits<unsigned>::max()) return false;
    if (v > static_cast<std::size_t>(kMaxWorkers)) return false;
    out = static_cast<unsigned>(v);
    return true;
}

bool parse_poll_ms(const char* s, unsigned& out) {
    std::size_t v = 0;
    if (!parse_digits(s, v) || v == 0) return false;
    if (v < kMinPollMs || v > kMaxPollMs) return false;
    out = static_cast<unsigned>(v);
    return true;
}

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
        } else if (a == "-n") {
            const char* v = next("-n");
            if (!v || !parse_count(v, args.lines) || args.lines > kMaxLines)
                return usage(argv[0]);
        } else if (a == "-f") {
            args.follow = true;
        } else if (a == "--poll-interval") {
            const char* v = next("--poll-interval");
            if (!v || !parse_poll_ms(v, args.poll_interval_ms))
                return usage(argv[0]);
        } else if (a == "--buffer-size") {
            const char* v = next("--buffer-size");
            if (!v || !parse_digits(v, args.buffer_size) ||
                args.buffer_size < kMinBufferSize ||
                args.buffer_size > kMaxBufferSize)
                return usage(argv[0]);
        } else if (a == "--max-line-bytes") {
            const char* v = next("--max-line-bytes");
            if (!v || !parse_digits(v, args.max_line_bytes) ||
                args.max_line_bytes == 0 ||
                args.max_line_bytes > kMaxMaxLineBytes)
                return usage(argv[0]);
        } else if (a == "--workers") {
            const char* v = next("--workers");
            if (!v || !parse_workers(v, args.workers)) return usage(argv[0]);
        } else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], a.c_str());
            return usage(argv[0]);
        } else {
            if (positionals == 0) args.file = a;
            else {
                std::fprintf(stderr, "%s: extra operand %s\n", argv[0],
                             a.c_str());
                return usage(argv[0]);
            }
            ++positionals;
        }
    }
    if (positionals != 1) return usage(argv[0]);
    return 0;
}

}  // namespace sluice_tail::cli
