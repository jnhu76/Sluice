





#pragma once

#include <sluice/error.hpp>

#include <string>

namespace sluice_copy {



enum class OpenCopyFailure : std::uint8_t {
    none,
    src_open,
    src_stat,
    src_not_regular,
    dst_open,
    dst_stat,
    dst_not_regular,
    same_file,
};

struct OpenCopyOutcome {
    int src_fd = -1;
    int dst_fd = -1;
    OpenCopyFailure failure = OpenCopyFailure::none;
    sluice::IoError error{};
};






















OpenCopyOutcome open_copy_files(const std::string& src_path,
                                const std::string& dst_path);


const char* open_copy_failure_message(OpenCopyFailure f);

}
