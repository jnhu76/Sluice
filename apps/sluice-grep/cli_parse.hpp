// sluice-grep CLI argument parsing (small internal module). Not installed.
#pragma once

#include "grep_task.hpp"  // kMaxWorkers, line-byte caps

#include <cstddef>
#include <string>
#include <vector>

namespace sluice_grep::cli {

struct CliArgs {
    std::string pattern;
    std::vector<std::string> files;
    bool line_numbers = false;         // -n
    std::size_t buffer_size = 1 << 20; // 1 MiB default
    std::size_t max_line_bytes = kDefaultMaxLineBytes;
    unsigned workers = 1;
    bool help = false;
};

// Print usage to stderr. Returns the usage-error exit code (2 for grep: see
// the traditional exit-code contract in the README).
int usage(const char* prog);

bool parse_size(const char* s, std::size_t& out);
bool parse_workers(const char* s, unsigned& out);

// Parse argv into `args`. Returns 0 on success, or a non-zero exit code on a
// usage error (message already printed).
int parse_args(int argc, char** argv, CliArgs& args);

}  // namespace sluice_grep::cli
