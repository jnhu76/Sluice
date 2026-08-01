// sluice-copy file open + input-domain validation (small internal module).
//
// Extracted from main.cpp so the Version B regular-file input domain can be
// tested directly (invalid source must never create or truncate the
// destination). Not an installed public header; the app target and its tests
// are the only consumers.
#pragma once

#include <sluice/error.hpp>

#include <string>

namespace sluice_copy {

// Why open_copy_files() failed. Distinct tags let the CLI print an exact
// message and let tests assert the exact rejection without parsing strings.
enum class OpenCopyFailure : std::uint8_t {
    none,           // success; src_fd/dst_fd valid
    src_open,       // cannot open source (error.os_errno set)
    src_stat,       // fstat on source failed (error.os_errno set)
    src_not_regular,   // source exists but is not a regular file
    dst_open,       // cannot open destination (error.os_errno set)
    dst_stat,       // fstat on destination failed (error.os_errno set)
    dst_not_regular,   // destination exists but is not a regular file
    same_file,      // source and destination are the same inode
};

struct OpenCopyOutcome {
    int src_fd = -1;  // valid when failure == none
    int dst_fd = -1;  // valid when failure == none
    OpenCopyFailure failure = OpenCopyFailure::none;
    sluice::IoError error{};  // underlying OS error (os_errno) where relevant
};

// Open source O_RDONLY and destination O_WRONLY|O_CREAT (mode 0644) and
// validate the Version B input domain:
//
//   * source must be a regular file (positional reads, finite length, eventual
//     EOF). This is checked via fstat IMMEDIATELY after opening the source and
//     BEFORE the destination is created, so an invalid source never creates a
//     new destination file and never touches an existing one.
//   * destination must be a regular file (truncatable, positional).
//   * source and destination must not be the same file (device + inode
//     identity, covering hard links and the same pathname).
//
// The destination is opened but NOT truncated here; the caller truncates only
// after this returns success. FIFO/socket/char-device sources are rejected;
// note that open(src, O_RDONLY) itself may already block for FIFOs with no
// writer — that happens before this function can fstat.
OpenCopyOutcome open_copy_files(const std::string& src_path,
                                const std::string& dst_path);

// Human-readable message for a failed outcome (CLI diagnostics).
const char* open_copy_failure_message(OpenCopyFailure f);

}  // namespace sluice_copy
