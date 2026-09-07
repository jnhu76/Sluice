#pragma once

#include "tail_task.hpp"

#include <cstddef>
#include <string>

namespace sluice_tail::cli {

struct CliArgs {
    std::string file;
    std::size_t lines = 10;
    bool follow = false;
    unsigned poll_interval_ms = 200;
    std::size_t buffer_size = 64 * 1024;
    std::size_t max_line_bytes = kDefaultMaxLineBytes;
    unsigned workers = 1;
    bool help = false;
};

int usage(const char* prog);

bool parse_count(const char* s, std::size_t& out);

bool parse_workers(const char* s, unsigned& out);
bool parse_poll_ms(const char* s, unsigned& out);

int parse_args(int argc, char** argv, CliArgs& args);

} // namespace sluice_tail::cli
