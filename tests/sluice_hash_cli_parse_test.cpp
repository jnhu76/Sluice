// sluice-hash CLI argument parsing tests (same pattern as sluice-copy's).
#include "harness.hpp"

#include "cli_parse.hpp"

#include <cstddef>
#include <limits>

using namespace sluice_hash;
using sluice_hash::cli::CliArgs;
using sluice_hash::cli::parse_args;
using sluice_hash::cli::parse_size;
using sluice_hash::cli::parse_workers;

namespace {

std::size_t g_out = 0;

bool parse_size_ok(const char* s) {
    std::size_t v = 0;
    return parse_size(s, v) ? (g_out = v, true) : false;
}

}  // namespace

SLUICE_TEST_CASE(hash_cli_size_rejects_empty_signs_junk_zero) {
    SLUICE_CHECK(!parse_size_ok(""));
    SLUICE_CHECK(!parse_size_ok("-1"));
    SLUICE_CHECK(!parse_size_ok("+8"));
    SLUICE_CHECK(!parse_size_ok("1024abc"));
    SLUICE_CHECK(!parse_size_ok("0x400"));
    SLUICE_CHECK(!parse_size_ok("0"));
    SLUICE_CHECK(!parse_size_ok(nullptr));
}

SLUICE_TEST_CASE(hash_cli_size_overflow_rejected) {
    SLUICE_CHECK(!parse_size_ok("18446744073709551616"));
    SLUICE_CHECK(!parse_size_ok("99999999999999999999999999"));
}

SLUICE_TEST_CASE(hash_cli_size_accepts_valid) {
    SLUICE_CHECK(parse_size_ok("1"));
    SLUICE_CHECK(g_out == 1);
    SLUICE_CHECK(parse_size_ok("1048576"));
    SLUICE_CHECK(g_out == 1048576);
}

SLUICE_TEST_CASE(hash_cli_workers_narrowing_and_cap) {
    unsigned w = 0;
    SLUICE_CHECK(parse_workers("2", w));
    SLUICE_CHECK(w == 2);
    // Above kMaxWorkers (64) rejected.
    SLUICE_CHECK(!parse_workers("65", w));
    // Would overflow unsigned after narrowing: reject, don't wrap.
    SLUICE_CHECK(!parse_workers("4294967296", w));
    SLUICE_CHECK(!parse_workers("18446744073709551616", w));
    SLUICE_CHECK(!parse_workers("0", w));
}

// parse_args wiring through a fake argv.
SLUICE_TEST_CASE(hash_cli_parse_args_layout) {
    {
        const char* argv[] = {"prog", "a.bin", "b.bin"};
        CliArgs a;
        SLUICE_CHECK(parse_args(3, const_cast<char**>(argv), a) == 0);
        SLUICE_CHECK(!a.help);
        SLUICE_CHECK(a.files.size() == 2);
        SLUICE_CHECK(a.files[0] == "a.bin" && a.files[1] == "b.bin");
        SLUICE_CHECK(a.buffer_size == 1 << 20);
        SLUICE_CHECK(a.workers == 1);
    }
    {
        const char* argv[] = {"prog", "--buffer-size", "8192",
                              "--workers", "3", "f"};
        CliArgs a;
        SLUICE_CHECK(parse_args(6, const_cast<char**>(argv), a) == 0);
        SLUICE_CHECK(a.buffer_size == 8192);
        SLUICE_CHECK(a.workers == 3);
        SLUICE_CHECK(a.files.size() == 1 && a.files[0] == "f");
    }
    {
        // No operands.
        const char* argv[] = {"prog"};
        CliArgs a;
        SLUICE_CHECK(parse_args(1, const_cast<char**>(argv), a) != 0);
    }
    {
        // Unknown option.
        const char* argv[] = {"prog", "--bogus", "f"};
        CliArgs a;
        SLUICE_CHECK(parse_args(3, const_cast<char**>(argv), a) != 0);
    }
    {
        // Missing value.
        const char* argv[] = {"prog", "--buffer-size"};
        CliArgs a;
        SLUICE_CHECK(parse_args(2, const_cast<char**>(argv), a) != 0);
    }
    {
        // Help short-circuits (no operand requirement).
        const char* argv[] = {"prog", "--help"};
        CliArgs a;
        SLUICE_CHECK(parse_args(2, const_cast<char**>(argv), a) == 0);
        SLUICE_CHECK(a.help);
    }
}

SLUICE_MAIN()
