// sluice-tail CLI argument parsing tests.
#include "harness.hpp"

#include "cli_parse.hpp"

#include <cstddef>

using namespace sluice_tail;
using sluice_tail::cli::CliArgs;
using sluice_tail::cli::parse_args;
using sluice_tail::cli::parse_count;
using sluice_tail::cli::parse_poll_ms;
using sluice_tail::cli::parse_workers;

namespace {

std::size_t g_out = 0;

int run_parse(std::vector<const char*> argv_in, CliArgs& a) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("prog"));
    for (auto* s : argv_in) argv.push_back(const_cast<char*>(s));
    return parse_args(static_cast<int>(argv.size()), argv.data(), a);
}

}  // namespace

SLUICE_TEST_CASE(tail_cli_count_allows_zero_rejects_junk) {
    SLUICE_CHECK(parse_count("0", g_out));
    SLUICE_CHECK(g_out == 0);
    SLUICE_CHECK(parse_count("42", g_out));
    SLUICE_CHECK(g_out == 42);
    SLUICE_CHECK(!parse_count("", g_out));
    SLUICE_CHECK(!parse_count("-1", g_out));
    SLUICE_CHECK(!parse_count("10x", g_out));
    SLUICE_CHECK(!parse_count("99999999999999999999999999", g_out));
    SLUICE_CHECK(!parse_count(nullptr, g_out));
}

SLUICE_TEST_CASE(tail_cli_workers_and_poll_bounds) {
    unsigned w = 0, p = 0;
    SLUICE_CHECK(parse_workers("2", w));
    SLUICE_CHECK(!parse_workers("0", w));
    SLUICE_CHECK(!parse_workers("65", w));
    SLUICE_CHECK(parse_poll_ms("50", p) && p == 50);
    SLUICE_CHECK(parse_poll_ms("5000", p) && p == 5000);
    SLUICE_CHECK(!parse_poll_ms("49", p));
    SLUICE_CHECK(!parse_poll_ms("5001", p));
    SLUICE_CHECK(!parse_poll_ms("0", p));
}

SLUICE_TEST_CASE(tail_cli_layout_default_ten_lines) {
    CliArgs a;
    SLUICE_CHECK(run_parse({"f.log"}, a) == 0);
    SLUICE_CHECK(a.file == "f.log");
    SLUICE_CHECK(a.lines == 10);
    SLUICE_CHECK(!a.follow);
    SLUICE_CHECK(a.poll_interval_ms == 200);
    SLUICE_CHECK(a.buffer_size == 64 * 1024);
    SLUICE_CHECK(a.max_line_bytes == kDefaultMaxLineBytes);
}

SLUICE_TEST_CASE(tail_cli_flags_and_values) {
    CliArgs a;
    SLUICE_CHECK(run_parse({"-f", "-n", "3", "--poll-interval", "100",
                            "--buffer-size", "8192", "--max-line-bytes",
                            "2048", "--workers", "2", "x"},
                           a) == 0);
    SLUICE_CHECK(a.follow);
    SLUICE_CHECK(a.lines == 3);
    SLUICE_CHECK(a.poll_interval_ms == 100);
    SLUICE_CHECK(a.buffer_size == 8192);
    SLUICE_CHECK(a.max_line_bytes == 2048);
    SLUICE_CHECK(a.workers == 2);
    SLUICE_CHECK(a.file == "x");
}

SLUICE_TEST_CASE(tail_cli_usage_errors) {
    // No operands / extra operands / unknown flags / bad values / -n cap.
    CliArgs a;
    SLUICE_CHECK(run_parse({}, a) == 1);
    SLUICE_CHECK(run_parse({"a", "b"}, a) == 1);  // extra operand
    SLUICE_CHECK(run_parse({"-z", "f"}, a) == 1);
    SLUICE_CHECK(run_parse({"-n"}, a) == 1);  // missing value
    SLUICE_CHECK(run_parse({"-n", "-3", "f"}, a) == 1);
    SLUICE_CHECK(run_parse({"--poll-interval", "10", "f"}, a) == 1);
    SLUICE_CHECK(run_parse({"--buffer-size", "1", "f"}, a) == 1);  // < min
    SLUICE_CHECK(run_parse({"--max-line-bytes", "0", "f"}, a) == 1);
    {
        CliArgs big;
        // kMaxLines + 1 must be rejected, not clamped.
        SLUICE_CHECK(run_parse({"-n", "1000000001", "f"}, big) == 1);
    }
    SLUICE_CHECK(run_parse({"--help"}, a) == 0);
    SLUICE_CHECK(a.help);
}

SLUICE_MAIN()
