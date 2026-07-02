// cppio FileReader / FileWriter — blocking POSIX file I/O behind the
// Reader/Writer abstractions. RAII closes the descriptor; move-only.
//
// This is the minimal blocking backend. No durability beyond what is asked
// for: FileWriter::flush() is a documented no-op for user-space state (no
// fsync) in this first phase.
#pragma once

#include <cppio/measurement.hpp>
#include <cppio/reader.hpp>
#include <cppio/sync.hpp>
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
    // recorded there for the lifetime of this reader. If `vec_stats` is
    // non-null, read_vec counters (non-fallback — the real readv path) are
    // recorded there.
    explicit FileReader(const std::string& path, SyscallStats* stats = nullptr,
                        VectorStats* vec_stats = nullptr);
    // Adopt an already-open file descriptor. The reader takes ownership and
    // will close it on destruction. Pass -1 for an empty reader.
    explicit FileReader(int fd) : fd_(fd) {}
    ~FileReader() override;
    FileReader(FileReader&& other) noexcept
        : fd_(other.fd_), open_error_(std::move(other.open_error_)),
          stats_(other.stats_), vec_stats_(other.vec_stats_) {
        other.fd_ = -1;
        other.open_error_.reset();
        other.stats_ = nullptr;
        other.vec_stats_ = nullptr;
    }
    FileReader& operator=(FileReader&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            open_error_ = std::move(other.open_error_);
            stats_ = other.stats_;
            vec_stats_ = other.vec_stats_;
            other.fd_ = -1;
            other.open_error_.reset();
            other.stats_ = nullptr;
            other.vec_stats_ = nullptr;
        }
        return *this;
    }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    bool opened() const { return fd_ >= 0; }
    // The preserved errno from a failed open(), or empty if open succeeded / this
    // reader is default/moved-from. Exposed so IoContext backends can surface the
    // real open error at open time rather than deferring to first read.
    const std::optional<IoError>& open_error() const { return open_error_; }
    // Returns an error if !opened(); preserves the real errno from a failed
    // open() rather than a synthetic code.
    Result<std::size_t> read_some(std::span<std::byte> dst) override;
    // POSIX readv override: gathers into all non-empty slices in one (chunked)
    // syscall. See src/file.cpp and docs/readv-writev-design-note.md.
    Result<std::size_t> read_vec(std::span<IoSlice> dsts) override;

private:
    void close();
    int fd_ = -1;
    // Set when the constructor's open() failed; surfaced on first I/O. Empty
    // for a default-constructed or moved-from reader.
    std::optional<IoError> open_error_;
    // Optional measurement; null = no counting. Caller-owned, never dereferenced
    // after destruction — callers must keep it alive for the reader's lifetime.
    SyscallStats* stats_ = nullptr;
    VectorStats* vec_stats_ = nullptr;
};

class FileWriter final : public Writer, public SyncableWriter {
public:
    FileWriter() = default;
    // Creates/truncates the file (O_WRONLY|O_CREAT|O_TRUNC). If `stats` is
    // non-null, syscall counters are recorded there for the writer's lifetime.
    // If `vec_stats` is non-null, write_vec counters (non-fallback — the real
    // writev path) are recorded there. If `sync_stats` is non-null, sync_data/
    // sync_all counters are recorded there (CPPIO-CORE-008D).
    explicit FileWriter(const std::string& path, SyscallStats* stats = nullptr,
                        VectorStats* vec_stats = nullptr, SyncStats* sync_stats = nullptr);
    // Adopt an already-open file descriptor (e.g. STDOUT_FILENO). Ownership is
    // taken; the writer will close it on destruction. Pass -1 for an empty writer.
    explicit FileWriter(int fd) : fd_(fd) {}
    ~FileWriter() override;
    FileWriter(FileWriter&& other) noexcept
        : fd_(other.fd_), open_error_(std::move(other.open_error_)),
          stats_(other.stats_), vec_stats_(other.vec_stats_), sync_stats_(other.sync_stats_) {
        other.fd_ = -1;
        other.open_error_.reset();
        other.stats_ = nullptr;
        other.vec_stats_ = nullptr;
        other.sync_stats_ = nullptr;
    }
    FileWriter& operator=(FileWriter&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            open_error_ = std::move(other.open_error_);
            stats_ = other.stats_;
            vec_stats_ = other.vec_stats_;
            sync_stats_ = other.sync_stats_;
            other.fd_ = -1;
            other.open_error_.reset();
            other.stats_ = nullptr;
            other.vec_stats_ = nullptr;
            other.sync_stats_ = nullptr;
        }
        return *this;
    }
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    bool opened() const { return fd_ >= 0; }
    // The preserved errno from a failed open(), or empty if open succeeded / this
    // writer is default/moved-from. Exposed so IoContext backends can surface the
    // real open error at open time rather than deferring to first write.
    const std::optional<IoError>& open_error() const { return open_error_; }
    // Returns an error if !opened(); preserves the real errno from a failed
    // open() rather than a synthetic code.
    Result<std::size_t> write_some(std::span<const std::byte> src) override;
    // POSIX writev override: scatters from all non-empty slices in (chunked)
    // syscalls. See src/file.cpp and docs/readv-writev-design-note.md.
    Result<std::size_t> write_vec(std::span<const ConstIoSlice> srcs) override;
    // No-op flush of user-space state in this phase (no fsync). Durability is
    // INTENTIONALLY separate — see sync_data/sync_all below and
    // docs/flush-sync-durability.md. flush() must never call fsync/fdatasync.
    Result<void> flush() override { return {}; }
    // Request persistence of file data (fdatasync). See docs/flush-sync-durability.md.
    Result<void> sync_data() override;
    // Request persistence of file data + metadata (fsync).
    Result<void> sync_all() override;

private:
    void close();
    int fd_ = -1;
    std::optional<IoError> open_error_;
    SyscallStats* stats_ = nullptr;
    VectorStats* vec_stats_ = nullptr;
    SyncStats* sync_stats_ = nullptr;
};

}  // namespace cppio
