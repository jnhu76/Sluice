// cppio FileReader / FileWriter — blocking POSIX file I/O behind the
// Reader/Writer abstractions. RAII closes the descriptor; move-only.
//
// This is the minimal blocking backend. No durability beyond what is asked
// for: FileWriter::flush() is a documented no-op for user-space state (no
// fsync) in this first phase.
#pragma once

#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <utility>

namespace cppio {

class FileReader final : public Reader {
public:
    FileReader() = default;
    explicit FileReader(const std::string& path);
    // Adopt an already-open file descriptor. The reader takes ownership and
    // will close it on destruction. Pass -1 for an empty reader.
    explicit FileReader(int fd) : fd_(fd) {}
    ~FileReader() override;
    FileReader(FileReader&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FileReader& operator=(FileReader&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    bool opened() const { return fd_ >= 0; }
    Result<std::size_t> read_some(std::span<std::byte> dst) override;

private:
    void close();
    int fd_ = -1;
};

class FileWriter final : public Writer {
public:
    FileWriter() = default;
    // Creates/truncates the file (O_WRONLY|O_CREAT|O_TRUNC).
    explicit FileWriter(const std::string& path);
    // Adopt an already-open file descriptor (e.g. STDOUT_FILENO). Ownership is
    // taken; the writer will close it on destruction. Pass -1 for an empty writer.
    explicit FileWriter(int fd) : fd_(fd) {}
    ~FileWriter() override;
    FileWriter(FileWriter&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FileWriter& operator=(FileWriter&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    bool opened() const { return fd_ >= 0; }
    Result<std::size_t> write_some(std::span<const std::byte> src) override;
    // No-op flush of user-space state in this phase (no fsync). Durability is
    // intentionally deferred per the task boundaries.
    Result<void> flush() override { return {}; }

private:
    void close();
    int fd_ = -1;
};

}  // namespace cppio
