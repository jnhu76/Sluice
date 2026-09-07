#pragma once

#include "grep_task.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace sluice_grep::cli {

struct CliArgs {
    std::string pattern;
    std::vector<std::string> files;
    bool line_numbers = false;
    std::size_t buffer_size = 1 << 20;
    std::size_t max_line_bytes = kDefaultMaxLineBytes;
    unsigned workers = 1;
    bool help = false;
};

int usage(const char* prog);

bool parse_size(const char* s, std::size_t& out);
bool parse_workers(const char* s, unsigned& out);

int parse_args(int argc, char** argv, CliArgs& args);

} // namespace sluice_grep::cli
