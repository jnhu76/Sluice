// sluice-copy CLI argument parsing tests.
//
// Drives the SAME cli_parse.cpp module the CLI binary uses (compiled into this
// target, same pattern as copy_task.cpp in the other sluice-copy tests).
// Covers the strict integer-parsing and resource-limit contracts:
//   - negative numbers, signs, trailing junk, empty strings, and zero are
//     rejected;
//   - size_t overflow is checked before any conversion (no silent truncation);
//   - the size_t -> unsigned worker conversion is guarded (no narrowing) and
//     the app-level kMaxWorkers cap applies;
//   - parse_args wiring (missing values, unknown options, extra operands).
#include "harness.hpp"

#include "cli_parse.hpp"

#include <climits>
#include <cstddef>
#include <limits>

using namespace sluice_copy;
using sluice_copy::cli::CliArgs;
using sluice_copy::cli::parse_args;
using sluice_copy::cli::parse_size;
using sluice_copy::cli::parse_workers;
using sluice_copy::cli::parse_sync;

namespace {

std::size_t g_out = 0;  // reused scratch for parse_size results

bool parse_size_ok(const char* s) {
    std::size_t v = 0;
    return parse_size(s, v) ? (g_out = v, true) : false;
}

}  // namespace

// ---------------------------------------------------------------------------
// parse_size: strict unsigned decimal parsing
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(cli_parse_size_rejects_empty_and_signs) {
    SLUICE_CHECK(!parse_size_ok(""));
    SLUICE_CHECK(!parse_size_ok("-1"));
    SLUICE_CHECK(!parse_size_ok("-42"));
    SLUICE_CHECK(!parse_size_ok("+1"));      // leading '+' also rejected
    SLUICE_CHECK(!parse_size_ok("--1"));
    SLUICE_CHECK(!parse_size_ok(nullptr));
}

SLUICE_TEST_CASE(cli_parse_size_rejects_zero) {
    SLUICE_CHECK(!parse_size_ok("0"));
    SLUICE_CHECK(!parse_size_ok("00"));
}

SLUICE_TEST_CASE(cli_parse_size_rejects_trailing_junk) {
    SLUICE_CHECK(!parse_size_ok("123abc"));
    SLUICE_CHECK(!parse_size_ok("1MiB"));
    SLUICE_CHECK(!parse_size_ok("12 34"));
    SLUICE_CHECK(!parse_size_ok("12.5"));
    SLUICE_CHECK(!parse_size_ok("0x10"));    // hex not accepted
    SLUICE_CHECK(!parse_size_ok(" 42"));     // leading whitespace not accepted
    SLUICE_CHECK(!parse_size_ok("42 "));     // trailing whitespace not accepted
}

SLUICE_TEST_CASE(cli_parse_size_overflow_rejected) {
    // 2^64 does not fit in any 64-bit size_t; must be rejected, not wrapped.
    SLUICE_CHECK(!parse_size_ok("18446744073709551616"));
    // 2^32 fits only on 64-bit size_t platforms.
    SLUICE_CHECK(parse_size_ok("4294967296") == (sizeof(std::size_t) > 4));
    // Overflow by accumulation (many digits).
    SLUICE_CHECK(!parse_size_ok("99999999999999999999999999"));
}

SLUICE_TEST_CASE(cli_parse_size_accepts_valid_decimal) {
    SLUICE_CHECK(parse_size_ok("1"));
    SLUICE_CHECK(g_out == 1);
    SLUICE_CHECK(parse_size_ok("42"));
    SLUICE_CHECK(g_out == 42);
    SLUICE_CHECK(parse_size_ok("1048576"));  // 1 MiB
    SLUICE_CHECK(g_out == 1048576);
    // Leading zeros are digits and are accepted.
    SLUICE_CHECK(parse_size_ok("007"));
    SLUICE_CHECK(g_out == 7);
}

SLUICE_TEST_CASE(cli_parse_size_accepts_size_max) {
    // The exact SIZE_MAX value fits (on 64-bit targets); the conversion is
    // range-checked, so it is accepted — the caller (run_pipelined_copy)
    // applies the app-level caps afterwards.
    if (sizeof(std::size_t) == 8) {
        SLUICE_CHECK(parse_size_ok("18446744073709551615"));
        SLUICE_CHECK(g_out == std::numeric_limits<std::size_t>::max());
    }
}

// ---------------------------------------------------------------------------
// parse_workers: no silent narrowing + app-level cap
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(cli_parse_workers_rejects_zero_negative) {
    unsigned w = 0;
    SLUICE_CHECK(!parse_workers("0", w));
    SLUICE_CHECK(!parse_workers("-1", w));
    SLUICE_CHECK(!parse_workers("", w));
}

SLUICE_TEST_CASE(cli_parse_workers_rejects_narrowing_overflow) {
    unsigned w = 0;
    // UINT_MAX + 1 does not fit in unsigned: rejected before conversion.
    SLUICE_CHECK(!parse_workers("4294967296", w));
    // Far beyond any platform unsigned: also rejected.
    SLUICE_CHECK(!parse_workers("18446744073709551615", w));
}

SLUICE_TEST_CASE(cli_parse_workers_applies_app_cap) {
    unsigned w = 0;
    // At the app-level cap: accepted.
    SLUICE_CHECK(parse_workers("64", w));
    SLUICE_CHECK(w == kMaxWorkers);
    // One past the cap: rejected (a legal-but-extreme count would exhaust
    // OS resources in ThreadPoolBackend/Runtime worker threads).
    SLUICE_CHECK(!parse_workers("65", w));
    SLUICE_CHECK(!parse_workers("128", w));
    // Junk never parses, even when small.
    SLUICE_CHECK(!parse_workers("1x", w));
}

SLUICE_TEST_CASE(cli_parse_workers_accepts_valid_counts) {
    unsigned w = 0;
    SLUICE_CHECK(parse_workers("1", w));
    SLUICE_CHECK(w == 1);
    SLUICE_CHECK(parse_workers("4", w));
    SLUICE_CHECK(w == 4);
}

// ---------------------------------------------------------------------------
// parse_sync
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(cli_parse_sync_policies) {
    SyncPolicy p = SyncPolicy::none;
    SLUICE_CHECK(parse_sync("none", p));
    SLUICE_CHECK(p == SyncPolicy::none);
    SLUICE_CHECK(parse_sync("data", p));
    SLUICE_CHECK(p == SyncPolicy::data);
    SLUICE_CHECK(parse_sync("all", p));
    SLUICE_CHECK(p == SyncPolicy::all);
    SLUICE_CHECK(!parse_sync("NONE", p));  // case-sensitive
    SLUICE_CHECK(!parse_sync("fsync", p));
    SLUICE_CHECK(!parse_sync("", p));
    SLUICE_CHECK(!parse_sync(nullptr, p));
}

// ---------------------------------------------------------------------------
// parse_args wiring
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(cli_parse_args_accepts_valid_invocation) {
    char a0[] = "sluice-copy";
    char a1[] = "--buffer-size";
    char a2[] = "4096";
    char a3[] = "--pipeline-depth";
    char a4[] = "3";
    char a5[] = "--workers";
    char a6[] = "2";
    char a7[] = "--sync";
    char a8[] = "data";
    char a9[] = "src";
    char a10[] = "dst";
    char* argv[] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10};
    CliArgs args;
    SLUICE_CHECK(parse_args(11, argv, args) == 0);
    SLUICE_CHECK(args.src == "src");
    SLUICE_CHECK(args.dst == "dst");
    SLUICE_CHECK(args.buffer_size == 4096);
    SLUICE_CHECK(args.pipeline_depth == 3);
    SLUICE_CHECK(args.workers == 2);
    SLUICE_CHECK(args.sync == SyncPolicy::data);
    SLUICE_CHECK(!args.help);
}

SLUICE_TEST_CASE(cli_parse_args_defaults) {
    char a0[] = "sluice-copy";
    char a1[] = "src";
    char a2[] = "dst";
    char* argv[] = {a0, a1, a2};
    CliArgs args;
    SLUICE_CHECK(parse_args(3, argv, args) == 0);
    SLUICE_CHECK(args.buffer_size == 1 << 20);
    SLUICE_CHECK(args.pipeline_depth == 1);
    SLUICE_CHECK(args.workers == 1);
    SLUICE_CHECK(args.sync == SyncPolicy::none);
}

SLUICE_TEST_CASE(cli_parse_args_rejects_bad_values) {
    auto run = [](int argc, char** argv) {
        CliArgs args;
        return parse_args(argc, argv, args);
    };
    // Negative values are usage errors.
    {
        char a0[] = "p"; char a1[] = "--workers"; char a2[] = "-1";
        char a3[] = "s"; char a4[] = "d";
        char* argv[] = {a0, a1, a2, a3, a4};
        SLUICE_CHECK(run(5, argv) != 0);
    }
    {
        char a0[] = "p"; char a1[] = "--buffer-size"; char a2[] = "-1";
        char a3[] = "s"; char a4[] = "d";
        char* argv[] = {a0, a1, a2, a3, a4};
        SLUICE_CHECK(run(5, argv) != 0);
    }
    {
        char a0[] = "p"; char a1[] = "--pipeline-depth"; char a2[] = "-1";
        char a3[] = "s"; char a4[] = "d";
        char* argv[] = {a0, a1, a2, a3, a4};
        SLUICE_CHECK(run(5, argv) != 0);
    }
    // Trailing junk.
    {
        char a0[] = "p"; char a1[] = "--buffer-size"; char a2[] = "1MiB";
        char a3[] = "s"; char a4[] = "d";
        char* argv[] = {a0, a1, a2, a3, a4};
        SLUICE_CHECK(run(5, argv) != 0);
    }
    // size_t overflow.
    {
        char a0[] = "p"; char a1[] = "--pipeline-depth";
        char a2[] = "18446744073709551616"; char a3[] = "s"; char a4[] = "d";
        char* argv[] = {a0, a1, a2, a3, a4};
        SLUICE_CHECK(run(5, argv) != 0);
    }
    // Workers beyond the app cap.
    {
        char a0[] = "p"; char a1[] = "--workers"; char a2[] = "65";
        char a3[] = "s"; char a4[] = "d";
        char* argv[] = {a0, a1, a2, a3, a4};
        SLUICE_CHECK(run(5, argv) != 0);
    }
    // Zero is rejected everywhere.
    {
        char a0[] = "p"; char a1[] = "--workers"; char a2[] = "0";
        char a3[] = "s"; char a4[] = "d";
        char* argv[] = {a0, a1, a2, a3, a4};
        SLUICE_CHECK(run(5, argv) != 0);
    }
    {
        char a0[] = "p"; char a1[] = "--buffer-size"; char a2[] = "0";
        char a3[] = "s"; char a4[] = "d";
        char* argv[] = {a0, a1, a2, a3, a4};
        SLUICE_CHECK(run(5, argv) != 0);
    }
}

SLUICE_TEST_CASE(cli_parse_args_rejects_structure_errors) {
    auto run = [](int argc, char** argv) {
        CliArgs args;
        return parse_args(argc, argv, args);
    };
    // Missing value.
    {
        char a0[] = "p"; char a1[] = "--workers";
        char a2[] = "s"; char a3[] = "d";
        char* argv[] = {a0, a1, a2, a3};
        SLUICE_CHECK(run(4, argv) != 0);
    }
    // Unknown option.
    {
        char a0[] = "p"; char a1[] = "--bogus"; char a2[] = "s"; char a3[] = "d";
        char* argv[] = {a0, a1, a2, a3};
        SLUICE_CHECK(run(4, argv) != 0);
    }
    // Extra operand.
    {
        char a0[] = "p"; char a1[] = "s"; char a2[] = "d"; char a3[] = "e";
        char* argv[] = {a0, a1, a2, a3};
        SLUICE_CHECK(run(4, argv) != 0);
    }
    // Missing operands.
    {
        char a0[] = "p"; char a1[] = "s";
        char* argv[] = {a0, a1};
        SLUICE_CHECK(run(2, argv) != 0);
    }
    {
        char a0[] = "p";
        char* argv[] = {a0};
        SLUICE_CHECK(run(1, argv) != 0);
    }
}

SLUICE_TEST_CASE(cli_parse_args_help) {
    char a0[] = "p"; char a1[] = "--help";
    char* argv[] = {a0, a1};
    CliArgs args;
    SLUICE_CHECK(parse_args(2, argv, args) == 0);
    SLUICE_CHECK(args.help);
}

SLUICE_MAIN()
