// sluice-grep CLI argument parsing tests.
#include "harness.hpp"

#include "cli_parse.hpp"

#include <cstddef>

using namespace sluice_grep;
using sluice_grep::cli::CliArgs;
using sluice_grep::cli::parse_args;
using sluice_grep::cli::parse_size;
using sluice_grep::cli::parse_workers;

namespace {

std::size_t g_out = 0;

bool parse_size_ok(const char* s) {
    std::size_t v = 0;
    return parse_size(s, v) ? (g_out = v, true) : false;
}

int run_parse(std::vector<const char*> argv_in, CliArgs& a) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("prog"));
    for (auto* s : argv_in) argv.push_back(const_cast<char*>(s));
    return parse_args(static_cast<int>(argv.size()), argv.data(), a);
}

}  // namespace

SLUICE_TEST_CASE(grep_cli_size_strictness) {
    SLUICE_CHECK(!parse_size_ok(""));
    SLUICE_CHECK(!parse_size_ok("-4"));
    SLUICE_CHECK(!parse_size_ok("+4"));
    SLUICE_CHECK(!parse_size_ok("4k"));
    SLUICE_CHECK(!parse_size_ok("0"));
    SLUICE_CHECK(!parse_size_ok("99999999999999999999999999"));
    SLUICE_CHECK(parse_size_ok("4096"));
    SLUICE_CHECK(g_out == 4096);
}

SLUICE_TEST_CASE(grep_cli_workers_cap) {
    unsigned w = 0;
    SLUICE_CHECK(parse_workers("4", w));
    SLUICE_CHECK(!parse_workers("0", w));
    SLUICE_CHECK(!parse_workers("65", w));
    SLUICE_CHECK(!parse_workers("4294967296", w));
}

SLUICE_TEST_CASE(grep_cli_layout_basic) {
    CliArgs a;
    SLUICE_CHECK(run_parse({"hello", "f.txt"}, a) == 0);
    SLUICE_CHECK(a.pattern == "hello");
    SLUICE_CHECK(a.files.size() == 1 && a.files[0] == "f.txt");
    SLUICE_CHECK(!a.line_numbers);
    SLUICE_CHECK(a.max_line_bytes == kDefaultMaxLineBytes);
}

SLUICE_TEST_CASE(grep_cli_flag_n_and_options) {
    CliArgs a;
    SLUICE_CHECK(run_parse({"-n", "--workers", "2", "--buffer-size", "8192",
                            "--max-line-bytes", "4096", "p", "a", "b"},
                           a) == 0);
    SLUICE_CHECK(a.line_numbers);
    SLUICE_CHECK(a.workers == 2);
    SLUICE_CHECK(a.buffer_size == 8192);
    SLUICE_CHECK(a.max_line_bytes == 4096);
    SLUICE_CHECK(a.files.size() == 2);
}

SLUICE_TEST_CASE(grep_cli_usage_errors_exit_two) {
    // Missing operands / unknown flags / bad values: exit code 2 (grep
    // tradition — usage errors are errors).
    {
        CliArgs a;
        SLUICE_CHECK(run_parse({}, a) == 2);
    }
    {
        CliArgs a;
        SLUICE_CHECK(run_parse({"only-pattern"}, a) == 2);
    }
    {
        CliArgs a;
        SLUICE_CHECK(run_parse({"-x", "p", "f"}, a) == 2);  // unknown short
    }
    {
        CliArgs a;
        SLUICE_CHECK(run_parse({"--bogus", "p", "f"}, a) == 2);
    }
    {
        CliArgs a;
        SLUICE_CHECK(run_parse({"--buffer-size", "0", "p", "f"}, a) == 2);
    }
    {
        CliArgs a;
        SLUICE_CHECK(run_parse({"--workers"}, a) == 2);  // missing value
    }
}

SLUICE_TEST_CASE(grep_cli_newline_pattern_rejected) {
    CliArgs a;
    SLUICE_CHECK(run_parse({"a\nb", "f"}, a) == 2);
}

SLUICE_TEST_CASE(grep_cli_empty_pattern_allowed) {
    // Empty pattern is a documented match-every-line policy, not an error.
    CliArgs a;
    SLUICE_CHECK(run_parse({"", "f"}, a) == 0);
    SLUICE_CHECK(a.pattern.empty());
}

SLUICE_TEST_CASE(grep_cli_dash_is_operand_not_option) {
    // A single "-" is an operand (grep reads it as stdin in GNU grep; here it
    // is a (failing) file operand), not an unknown option.
    CliArgs a;
    SLUICE_CHECK(run_parse({"p", "-"}, a) == 0);
    SLUICE_CHECK(a.files.size() == 1 && a.files[0] == "-");
}

SLUICE_MAIN()
