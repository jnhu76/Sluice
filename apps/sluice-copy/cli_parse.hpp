





#pragma once

#include "copy_task.hpp"

#include <sluice/error.hpp>

#include <cstddef>
#include <string>

namespace sluice_copy::cli {

struct CliArgs {
    std::string src;
    std::string dst;
    std::size_t buffer_size = 1 << 20;
    std::size_t pipeline_depth = 1;
    unsigned workers = 1;
    SyncPolicy sync = SyncPolicy::none;
    bool atomic = true;
    bool help = false;
};


int usage(const char* prog);








bool parse_size(const char* s, std::size_t& out);




bool parse_workers(const char* s, unsigned& out);

bool parse_sync(const char* s, SyncPolicy& out);



int parse_args(int argc, char** argv, CliArgs& args);


const char* code_name(sluice::IoError::Code c);

}
