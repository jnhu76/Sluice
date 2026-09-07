#pragma once

#include "hash_task.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace sluice_hash::cli {

struct CliArgs {
    std::vector<std::string> files;
    std::size_t buffer_size = 1 << 20;
    unsigned workers = 1;
    bool help = false;
};

int usage(const char* prog);

bool parse_size(const char* s, std::size_t& out);

bool parse_workers(const char* s, unsigned& out);

int parse_args(int argc, char** argv, CliArgs& args);

} // namespace sluice_hash::cli
