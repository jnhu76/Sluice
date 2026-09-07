












#pragma once

#include <sluice/limit.hpp>

#include <cstdint>
#include <string_view>

namespace sluice {


enum class CopyStrategy {
    Auto,
    Scratch,
    BufferedFirst,
};



struct CopyOptions {
    CopyLimit limit = CopyLimit::unlimited();
    CopyStrategy strategy = CopyStrategy::Auto;
};




struct CopyDecision {
    CopyStrategy requested = CopyStrategy::Auto;
    CopyStrategy selected = CopyStrategy::Auto;
    std::string_view reason = "auto";
    bool used_buffered_fast_path = false;
    bool used_scratch_path = false;
};


std::string_view to_string(CopyStrategy strategy);

}
