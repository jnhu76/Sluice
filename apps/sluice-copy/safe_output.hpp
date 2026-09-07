#pragma once

#include "copy_task.hpp"

#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <string>

namespace sluice_copy {

enum class SafeOpenFailure : std::uint8_t {
    none,
    src_open,
    src_stat,
    src_not_regular,
    dst_stat,
    dst_not_regular,
    same_file,

    temp_dir,
    temp_create,
    temp_chmod,
};

struct SafeOpenOutcome {
    int src_fd = -1;
    int temp_fd = -1;

    std::string temp_path;
    std::string dst_dir;

    SafeOpenFailure failure = SafeOpenFailure::none;
    sluice::IoError error{};
};

SafeOpenOutcome open_atomic_copy(const std::string& src_path, const std::string& dst_path);

const char* safe_open_failure_message(SafeOpenFailure f);

enum class SafeCommitStage : std::uint8_t {
    none,
    close,
    rename,
    dir_sync,

};

sluice::Result<void> commit_atomic_copy(SafeOpenOutcome& o, const std::string& dst_path,
                                        SyncPolicy sync, SafeCommitStage* stage = nullptr);

void discard_atomic_copy(SafeOpenOutcome& o);

} // namespace sluice_copy
