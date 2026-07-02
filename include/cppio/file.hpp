// cppio FileReader / FileWriter — blocking POSIX file I/O behind the
// Reader/Writer abstractions. RAII closes the descriptor; move-only.
//
// This is the minimal blocking backend. No durability beyond what is asked
// for: FileWriter::flush() is a documented no-op for user-space state (no
// fsync) in this first phase.
#pragma once

#include <cppio/measurement.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace cppio {

class FileReader final : public Reader {
public:
    FileReader() = default;
    // Open `path` for reading. If `stats` is non-null, syscall counters are
    // recorded there for the lifetime of this reader.
    explicit FileReader(const std::string& path, SyscallStats* stats = nullptr);
    // Adopt an already-open file descriptor. The reader takes ownership and
    // will close it on destruction. Pass -1 for an empty reader.
    explicit FileReader(int fd) : fd_(fd) {}
    ~FileReader() override;
    FileReader(FileReader&& other) noexcept
        : fd_(other.fd_), open_error_(std::move(other.open_error_)), stats_(other.stats_) {
        other.fd_ = -1;
        other.open_error_.reset();
        other.stats_ = nullptr;
    }
    FileReader& operator=(FileReader&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            open_error_ = std::move(other.open_error_);
            stats_ = other.stats_;
            other.fd_ = -1;
            other.open_error_.reset();
            other.stats_ = nullptr;
        }
        return *this;
    }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    bool opened() const { return fd_ >= 0; }
    // Returns an error if !opened(); the error preserves the real errno from a
    // failed open() rather than a synthetic code.
    Result<std::size_t> read_some(std::span<std::byte> dst) override;

private:
    void close();
    int fd_ = -1;
    // Set when the constructor's open() failed; surfaced on first I/O. Empty
    // for a default-constructed or moved-from reader.
    std::optional<IoError> open_error_;
    // Optional measurement; null = no counting. Caller-owned, never dereferenced
    // after destruction — callers must keep it alive for the reader's lifetime.
    SyscallStats* stats_ = nullptr;
};

class FileWriter final : public Writer {
public:
    FileWriter() = default;
    // Creates/truncates the file (O_WRONLY|O_CREAT|O_TRUNC). If `stats` is
    // non-null, syscall counters are recorded there for the writer's lifetime.
    explicit FileWriter(const std::string& path, SyscallStats* stats = nullptr);
    // Adopt an already-open file descriptor (e.g. STDOUT_FILENO). Ownership is
    // taken; the writer will close it on destruction. Pass -1 for an empty writer.
    explicit FileWriter(int fd) : fd_(fd) {}
    ~FileWriter() override;
    FileWriter(FileWriter&& other) noexcept
        : fd_(other.fd_), open_error_(std::move(other.open_error_)), stats_(other.stats_) {
        other.fd_ = -1;
        other.open_error_.reset();
        other.stats_ = nullptr;
    }
    FileWriter& operator=(FileWriter&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            open_error_ = std::move(other.open_error_);
            stats_ = other.stats_;
            other.fd_ = -1;
            other.open_error_.reset();
            other.stats_ = nullptr;
        }
        return *this;
    }
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    bool opened() const { return fd_ >= 0; }
    // Returns an error if !opened(); preserves the real errno from a failed
    // open() rather than a synthetic code.
    Result<std::size_t> write_some(std::span<const std::byte> src) override;
    // No-op flush of user-space state in this phase (no fsync). Durability is
    // intentionally deferred per the task boundaries.
    Result<void> flush() override { return {}; }

private:
    void close();
    int fd_ = -1;
    std::optional<IoError> open_error_;
    SyscallStats* stats_ = nullptr;
};

}  // namespace cppio
