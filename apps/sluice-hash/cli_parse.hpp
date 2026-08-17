// sluice-hash CLI argument parsing (small internal module; same shape as
// sluice-copy's cli_parse). Not an installed public header.
#pragma once

#include "hash_task.hpp"  // kMaxWorkers

#include <cstddef>
#include <string>
#include <vector>

namespace sluice_hash::cli {

struct CliArgs {
    std::vector<std::string> files;
    std::size_t buffer_size = 1 << 20;  // 1 MiB default
    unsigned workers = 1;
    bool help = false;
};

// Print usage to stderr. Returns the usage-error exit code (1).
int usage(const char* prog);

// Strict unsigned decimal parser: ASCII digits only; rejects empty strings,
// signs, trailing junk, overflow, and zero.
bool parse_size(const char* s, std::size_t& out);

// Worker-count parser with the size_t -> unsigned narrowing check and the
// kMaxWorkers cap.
bool parse_workers(const char* s, unsigned& out);

// Parse argv into `args`. Returns 0 on success, or a non-zero exit code on a
// usage error (message already printed).
int parse_args(int argc, char** argv, CliArgs& args);

}  // namespace sluice_hash::cli
