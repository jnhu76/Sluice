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

// ─── Minimal metadata and same-file identity (SEM-07) ──────────────────────

// The v1 file kinds. `regular` is the PROD-02 ordinary-file data-I/O domain;
// `other` is a kind `file_info` can report without promising that ordinary-file
// data operations work on it. This is deliberately not a stat-mode mirror: a
// kind only enters this enum through a v1 requirement.
enum class FileKind : std::uint8_t {
    regular,
    other,
};

// A bounded same-file identity value. Linux device/inode is the permitted
// representation (SEM-07); identity is a comparison value, never a registry
// key. It is valid for concurrently live resources within the platform domain:
// a retained value does not prove identity after every handle closes and the
// identifiers can be reused.
struct FileIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;

    friend bool operator==(const FileIdentity&, const FileIdentity&) noexcept = default;
};

struct FileInfo {
    FileKind kind = FileKind::other;
    // Size observed by this call: the regular-file length in bytes when kind is
    // `regular`. Not a reservation and not an immutable snapshot.
    std::uint64_t size = 0;
    // `nullopt` is the explicit identity-unavailable outcome. A provider that
    // cannot supply identity reports it here instead of fabricating one.
    std::optional<FileIdentity> identity;
};

enum class IdentityMatch : std::uint8_t {
    same,
    different,
    // At least one side has no identity. Unknown is never reported as `different`:
    // an unavailable identity supports no same/different answer.
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
    // unobservable (RAII boundary). A caller that needs the old close error must
    // call `close()` explicitly first.
    File& operator=(File&& other) noexcept;

    // Deterministic resource cleanup: noexcept, best effort, and unable to
    // report a close failure. Use `close()` when the error matters.
    ~File();

    // The observable close-error channel. The first native close attempt
    // consumes ownership even when it fails, so a failed close leaves the File
    // closed and the same native descriptor is never retried (SEM-02). Closing
    // an already closed or moved-from File succeeds as a no-op.
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
