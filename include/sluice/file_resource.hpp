#pragma once

#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace sluice {

enum class FileAccess : std::uint8_t {
    read_only,
    write_only,
    read_write,
};

enum class FileExistence : std::uint8_t {
    open_existing,
    create_if_missing,
    create_new,
};

enum class FileInitialContents : std::uint8_t {
    preserve,
    truncate,
};

struct FileOpen {
    FileAccess access = FileAccess::read_only;
    FileExistence existence = FileExistence::open_existing;
    FileInitialContents contents = FileInitialContents::preserve;
};

// ─── Minimal metadata and same-file identity ───────────────────────────────

// `regular` is the ordinary-file data-I/O domain; `other` covers kinds
// `file_info` can report without promising data operations on them. Not a
// stat-mode mirror: a kind enters only when a requirement names it.
enum class FileKind : std::uint8_t {
    regular,
    other,
};

// A bounded same-file identity value: a comparison value, not a registry key.
// A retained value proves nothing once the handles close and the identifiers
// can be reused.
struct FileIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;

    friend bool operator==(const FileIdentity&, const FileIdentity&) noexcept = default;
};

struct FileInfo {
    FileKind kind = FileKind::other;
    // Size observed by this call when kind is `regular`; not a snapshot
    // across calls.
    std::uint64_t size = 0;
    // `nullopt` reports identity-unavailable instead of fabricating one.
    std::optional<FileIdentity> identity;
};

enum class IdentityMatch : std::uint8_t {
    same,
    different,
    // At least one side has no identity: unknown is never reported as
    // `different`.
    unknown,
};

constexpr IdentityMatch identity_match(const FileInfo& lhs, const FileInfo& rhs) noexcept {
    if (!lhs.identity.has_value() || !rhs.identity.has_value())
        return IdentityMatch::unknown;
    return *lhs.identity == *rhs.identity ? IdentityMatch::same : IdentityMatch::different;
}

class File {
  public:
    static Result<File> open(const std::string& path, FileOpen mode = {});

    File(File&& other) noexcept;
    // Move assignment is a noexcept resource transfer: the destination's old
    // resource gets exactly one best-effort close attempt whose error is
    // unobservable. A caller that needs the old close error must call `close()`
    // explicitly first.
    File& operator=(File&& other) noexcept;

    // Deterministic resource cleanup: noexcept, best effort, and unable to
    // report a close failure. Use `close()` when the error matters.
    ~File();

    // The observable close-error channel. The first native close attempt
    // consumes ownership even when it fails: a failed close leaves the File
    // closed and the descriptor is never retried (an EINTR retry is unsafe on
    // Linux). Closing an already closed or moved-from File is a no-op.
    Result<void> close() noexcept;

    bool is_open() const noexcept { return fd_ >= 0; }

    int native_handle() const noexcept { return fd_; }

    FileAccess access() const noexcept { return access_; }

  private:
    explicit File(int fd, FileAccess access) noexcept : fd_(fd), access_(access) {}

    int fd_ = -1;
    FileAccess access_ = FileAccess::read_only;
};

}
