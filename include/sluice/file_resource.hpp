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

enum class FileKind : std::uint8_t {
    regular,
    other,
};

struct FileIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;

    friend bool operator==(const FileIdentity&, const FileIdentity&) noexcept = default;
};

struct FileInfo {
    FileKind kind = FileKind::other;
    std::uint64_t size = 0;
    std::optional<FileIdentity> identity;
};

enum class IdentityMatch : std::uint8_t {
    same,
    different,
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
    File& operator=(File&& other) noexcept;

    ~File();

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
