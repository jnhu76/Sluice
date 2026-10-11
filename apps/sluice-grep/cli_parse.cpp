#include "cli_parse.hpp"

#include <cstdio>
#include <cstring>
#include <limits>

namespace sluice_grep::cli {

namespace {

bool parse_unsigned_decimal(const char* s, std::size_t& out) {
    if (!s || *s == '\0')
        return false;
    std::size_t v = 0;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        unsigned d = static_cast<unsigned>(*p - '0');
        if (v > (std::numeric_limits<std::size_t>::max() - d) / 10)
            return false;
        v = v * 10 + d;
    }
    if (v == 0)
        return false;
    out = v;
    return true;
}

}

int usage(const char* prog) {
    std::fprintf(
        stderr,
        "usage: %s [options] <pattern> <file>...\n"
        "  -n                      prefix each match with its 1-based line number\n"
        "  --buffer-size <bytes>   read buffer (default 1 MiB; %zu..%zu)\n"
        "  --max-line-bytes <n>    cap for a retained line; longer lines are\n"
        "                          reported and skipped, not matched (default\n"
        "                          %zu; <= %zu)\n"
        "  --help                  show this help\n"
        "\n"
        "Literal byte-oriented substring search (no regex). Exit codes follow\n"
        "grep tradition: 0 = match found, 1 = no match, 2 = error.\n",
        prog, static_cast<std::size_t>(kMinBufferSize), static_cast<std::size_t>(kMaxBufferSize),
        static_cast<std::size_t>(kDefaultMaxLineBytes), static_cast<std::size_t>(kMaxMaxLineBytes));
    return 2;
}

bool parse_size(const char* s, std::size_t& out) {
    return parse_unsigned_decimal(s, out);
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
            args.line_numbers = true;
        } else if (a == "--buffer-size") {
            const char* v = next("--buffer-size");
            if (!v || !parse_size(v, args.buffer_size))
                return usage(argv[0]);
        } else if (a == "--max-line-bytes") {
            const char* v = next("--max-line-bytes");
            if (!v || !parse_size(v, args.max_line_bytes))
                return usage(argv[0]);
        } else if (a == "--workers" || a.starts_with("--workers=")) {
            std::fprintf(stderr,
                         "%s: --workers has been removed; omit it to use direct execution\n",
                         argv[0]);
            return usage(argv[0]);
        } else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], a.c_str());
            return usage(argv[0]);
        } else {
            if (positionals == 0)
                args.pattern = a;
            else
                args.files.push_back(a);
            ++positionals;
        }
    }
    if (positionals < 2) {
        std::fprintf(stderr, "%s: missing %s\n", argv[0],
                     positionals == 0 ? "pattern and file operands" : "file operand");
        return usage(argv[0]);
    }

    if (args.pattern.find('\n') != std::string::npos) {
        std::fprintf(stderr,
                     "%s: pattern contains a newline; multi-line patterns are "
                     "not supported (byte-oriented line matching)\n",
                     argv[0]);
        return 2;
    }
    if (!valid_config(args.buffer_size, args.max_line_bytes))
        return usage(argv[0]);
    return 0;
}

}
