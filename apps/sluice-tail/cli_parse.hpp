// sluice-tail CLI argument parsing (small internal module). Not installed.
#pragma once

#include "tail_task.hpp"  // TailOptions + caps

#include <cstddef>
#include <string>

namespace sluice_tail::cli {

struct CliArgs {
    std::string file;
    std::size_t lines = 10;      // -n N (0 allowed: no initial tail)
    bool follow = false;         // -f
    unsigned poll_interval_ms = 200;
    std::size_t buffer_size = 64 * 1024;
    std::size_t max_line_bytes = kDefaultMaxLineBytes;
    unsigned workers = 1;
    bool help = false;
};

// Print usage to stderr. Returns the usage-error exit code (1).
int usage(const char* prog);

// Strict unsigned parser that ALLOWS zero (for -n 0).
bool parse_count(const char* s, std::size_t& out);

bool parse_workers(const char* s, unsigned& out);
bool parse_poll_ms(const char* s, unsigned& out);

// Parse argv into `args`. Returns 0 on success, or a non-zero exit code on a
// usage error (message already printed).
int parse_args(int argc, char** argv, CliArgs& args);

}  // namespace sluice_tail::cli
