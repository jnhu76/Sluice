// sluice-copy Version C — safe atomic output (temp file + rename).
//
// Implements the Version C destination lifecycle from the file-tools track
// (docs/applications/file-tools-plan.md §3.1):
//
//   source -> mkstemp in the destination directory -> pipelined copy into the
//   temp fd (sync policy applies to the TEMP fd) -> close -> atomic rename
//   over the destination -> fsync of the parent directory when the sync
//   policy requires durability.
//
// Guarantees (see README "Atomicity and durability" for the full scope):
//   * an existing destination is NEVER visible as partial content — a
//     copy/sync failure leaves it untouched and unlinks the temp file;
//   * the rename is atomic within one filesystem (temp lives in the
//     destination's own directory);
//   * crash durability of the data + the rename requires --sync data|all
//     (none of it is claimed with --sync none).
//
// App-local module; not an installed public header. The app target and its
// tests are the only consumers.
#pragma once

#include "copy_task.hpp"  // SyncPolicy

#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <string>

namespace sluice_copy {

// Why open_atomic_copy() failed. Distinct tags let the CLI print an exact
// message and let tests assert the exact rejection without parsing strings.
enum class SafeOpenFailure : std::uint8_t {
    none,            // success; src_fd/temp_fd/temp_path/dst_dir valid
    src_open,        // cannot open source (error.os_errno set)
    src_stat,        // fstat on source failed (error.os_errno set)
    src_not_regular, // source exists but is not a regular file
    dst_stat,        // destination exists but stat failed (os_errno set)
    dst_not_regular, // destination exists but is not a regular file
    same_file,       // destination is the same inode as the source (hard link
                     // or identical path); replacing it would destroy the
                     // source mid-copy
    temp_dir,        // destination path has no usable parent directory
    temp_create,     // mkstemp in the destination directory failed (os_errno)
    temp_chmod,      // fchmod of the temp file to the source mode failed
};

struct SafeOpenOutcome {
    int src_fd = -1;          // valid when failure == none; caller closes
    int temp_fd = -1;         // valid when failure == none; caller passes to
                              // the copy, then to commit/discard
    std::string temp_path;    // absolute or relative temp pathname (mkstemp)
    std::string dst_dir;      // parent directory of the destination (used by
                              // commit for the directory fsync)
    SafeOpenFailure failure = SafeOpenFailure::none;
    sluice::IoError error{};  // underlying OS error (os_errno) where relevant
};

// Open + validate the source (same regular-file domain as file_domain.hpp),
// validate the destination if it exists (must be a regular file; must not be
// the same inode as the source), then create a uniquely-named temp file IN
// THE DESTINATION'S DIRECTORY and give it the source's permission bits
// (mode & 0777 — setuid/setgid/sticky are deliberately NOT preserved).
//
// The destination is never opened for writing here and never truncated; an
// existing destination is only read (stat) for validation. On any failure no
// temp file remains (a created-but-rejected temp is unlinked before
// returning).
//
// A destination that does not exist is fine (fresh creation); its parent
// directory must exist and be writable.
SafeOpenOutcome open_atomic_copy(const std::string& src_path,
                                 const std::string& dst_path);

// Human-readable message for a failed outcome (CLI diagnostics).
const char* safe_open_failure_message(SafeOpenFailure f);

// Commit the atomic replacement. Takes ownership of `o.temp_fd` and
// `o.temp_path`: on EVERY return path (success or error) the temp fd is
// closed and, unless the rename succeeded, the temp file is unlinked. The
// caller must have finished writing (and applying the sync policy) to the
// temp fd beforehand.
//
//   1. close(temp_fd)                       — flush the descriptor
//   2. rename(temp_path, dst_path)          — the atomic replacement
//   3. sync != none: fsync(dst_dir)         — make the rename durable
//
// Failure semantics:
//   * close/rename failure: destination untouched, temp unlinked, error
//     returned (os_errno set);
//   * directory-open/fsync failure (step 3): the rename ALREADY HAPPENED —
//     the destination now holds the new content. The error is returned so the
//     caller can report the missing durability; the temp no longer exists.
sluice::Result<void> commit_atomic_copy(SafeOpenOutcome& o,
                                        const std::string& dst_path,
                                        SyncPolicy sync);

// Best-effort cleanup for a copy that will not be committed: close the temp
// fd and unlink the temp file. Never touches the destination. Idempotent
// (safe to call on an already-consumed outcome).
void discard_atomic_copy(SafeOpenOutcome& o);

}  // namespace sluice_copy
