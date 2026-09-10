#pragma once

#include <sluice/async/application_runtime.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace sluice::async {

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

class File {
  public:
    static Result<File> open(const std::string& path, FileOpen mode = {});

    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

    ~File();

    Result<void> close() noexcept;

    bool is_open() const noexcept { return fd_ >= 0; }

    int native_handle() const noexcept { return fd_; }

  private:
    explicit File(int fd) noexcept : fd_(fd) {}

    int fd_ = -1;
};

Result<std::size_t> read_at(const File& file, RuntimeTaskContext& ctx, std::uint64_t offset,
                            std::span<std::byte> dst, Completion<std::size_t>& c);

} // namespace sluice::async
