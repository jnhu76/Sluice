// sluice FileReader / FileWriter — blocking POSIX file I/O behind the
// Reader/Writer abstractions. RAII closes the descriptor; move-only.
//
// This is the minimal blocking backend. No durability beyond what is asked
// for: FileWriter::flush() is a documented no-op for user-space state (no
// fsync) in this first phase.
#pragma once

#include <sluice/measurement.hpp>
#include <sluice/reader.hpp>
#include <sluice/sync.hpp>
#include <sluice/writer.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace sluice {

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
        : fd_(std::exchange(other.fd_, -1)), open_error_(std::exchange(other.open_error_, {})),
          stats_(std::exchange(other.stats_, nullptr)),
          vec_stats_(std::exchange(other.vec_stats_, nullptr)) {}
    FileReader& operator=(FileReader&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
            open_error_ = std::exchange(other.open_error_, {});
            stats_ = std::exchange(other.stats_, nullptr);
            vec_stats_ = std::exchange(other.vec_stats_, nullptr);
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

    // Positional read (sluice-CORE-018S): pread at an explicit byte offset.
    // Does NOT move the shared file cursor. Returns bytes read (0 == EOF at
    // that offset). See docs/sync-io-model.md (Positional I/O semantics).
    Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> dst);
    // Positional vector read (sluice-CORE-018S): preadv at an explicit byte
    // offset. Same stop-on-short + skip-empty semantics as read_vec. Does NOT
    // move the shared file cursor.
    Result<std::size_t> read_vec_at(std::uint64_t offset, std::span<IoSlice> dsts);
    // Derived (sluice-CORE-019S): read exactly dst.size() bytes from `offset`
    // (looping read_at across short reads), or fail on EOF/error. dst.size()==0
    // is immediate success. EOF before/within -> IoError::eof.
    Result<void> read_at_exact(std::uint64_t offset, std::span<std::byte> dst);

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
        : fd_(std::exchange(other.fd_, -1)), open_error_(std::exchange(other.open_error_, {})),
          stats_(std::exchange(other.stats_, nullptr)),
          vec_stats_(std::exchange(other.vec_stats_, nullptr)),
          sync_stats_(std::exchange(other.sync_stats_, nullptr)) {}
    FileWriter& operator=(FileWriter&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
            open_error_ = std::exchange(other.open_error_, {});
            stats_ = std::exchange(other.stats_, nullptr);
            vec_stats_ = std::exchange(other.vec_stats_, nullptr);
            sync_stats_ = std::exchange(other.sync_stats_, nullptr);
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
    // Positional write (sluice-CORE-018S): pwrite at an explicit byte offset.
    // Does NOT move the shared file cursor. Returns bytes written (0 on
    // non-empty input is invalid_state/backend failure — surfaced by callers).
    // See docs/sync-io-model.md (Positional I/O semantics).
    Result<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> src);
    // Positional vector write (sluice-CORE-018S): pwritev at an explicit byte
    // offset. Same stop-on-short + skip-empty semantics as write_vec. Does NOT
    // move the shared file cursor.
    Result<std::size_t> write_vec_at(std::uint64_t offset, std::span<const ConstIoSlice> srcs);
    // Derived (sluice-CORE-019S): write all of src at `offset`, looping
    // write_at across short writes and advancing offset by bytes written. Zero
    // progress on non-empty remaining input -> IoError::invalid_state.
    Result<void> write_at_all(std::uint64_t offset, std::span<const std::byte> src);
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

} // namespace sluice
