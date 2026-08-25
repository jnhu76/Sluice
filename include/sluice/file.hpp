// sluice FileReader / FileWriter — blocking POSIX file I/O behind the
// Reader/Writer abstractions. RAII closes the descriptor (best-effort,
// unreportable); move-only. Callers that need the close(2) result call
// close() explicitly before destruction.
//
// This is the minimal blocking backend. No durability beyond what is asked
// for: FileWriter::flush() is a documented no-op for user-space state (no
// fsync); durability is requested explicitly via sync_data()/sync_all().
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
            // Old descriptor closed best-effort; the result is discarded —
            // operator= is noexcept and has no error channel (call close()
            // first when the result matters).
            (void)close();
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
    // Close the descriptor and REPORT the close(2) result. For a reader this
    // is diagnostic (a read-only close has no
    // writeback semantics); see FileWriter::close for the data-integrity
    // case. Contract (both classes):
    //  - idempotent: closing an already-closed / never-opened / moved-from
    //    object is a no-op returning success. close() reports ONLY the close
    //    syscall — a preserved open() failure stays on open_error() and the
    //    first-I/O path, never here;
    //  - the descriptor is consumed on EVERY return path (success or error):
    //    on Linux close(2) releases the fd even when it reports EINTR/EIO.
    //    close is deliberately NOT retried through detail::retry_on_eintr
    //    (unlike read/write/fsync) — after an error return the fd number may
    //    already have been reused by another open, and retrying could close
    //    an unrelated descriptor. The raw errno is reported verbatim instead
    //    (EINTR -> interrupted, EIO -> backend_error, os_errno preserved);
    //  - the destructor and move-assignment close best-effort with the result
    //    DISCARDED (a destructor must not throw and has no channel); callers
    //    that need the close result must call close() explicitly first.
    // close() is noexcept by design: it performs one C syscall and returns a
    // value-type Result, so the destructor's best-effort use is structurally
    // throw-free, not incidentally so.
    Result<void> close() noexcept;
    // Returns an error if !opened(); preserves the real errno from a failed
    // open() rather than a synthetic code.
    Result<std::size_t> read_some(std::span<std::byte> dst) override;
    // POSIX readv override: gathers into all non-empty slices in one (chunked)
    // syscall. See src/file.cpp and docs/reference/sync-io-model.md
    // (Vector I/O semantics).
    Result<std::size_t> read_vec(std::span<IoSlice> dsts) override;

    // Positional read: pread at an explicit byte offset.
    // Does NOT move the shared file cursor. Returns bytes read (0 == EOF at
    // that offset). See docs/reference/sync-io-model.md (Positional I/O semantics).
    Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> dst);
    // Positional vector read: preadv at an explicit byte
    // offset. Same stop-on-short + skip-empty semantics as read_vec. Does NOT
    // move the shared file cursor.
    Result<std::size_t> read_vec_at(std::uint64_t offset, std::span<IoSlice> dsts);
    // Derived: read exactly dst.size() bytes from `offset`
    // (looping read_at across short reads), or fail on EOF/error. dst.size()==0
    // is immediate success. EOF before/within -> IoError::eof.
    Result<void> read_at_exact(std::uint64_t offset, std::span<std::byte> dst);

  private:
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
    // sync_all counters are recorded there.
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
            // Old descriptor closed best-effort; the result is discarded —
            // operator= is noexcept and has no error channel (call close()
            // first when the result matters).
            (void)close();
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
    // Close the descriptor and REPORT the close(2) result. close(2) on a file
    // with delayed writeback may fail with
    // EIO/ENOSPC — the kernel's LAST data-integrity report for this file;
    // discarding it (as the RAII destructor must) hides a real write
    // failure. The full contract is on FileReader::close; the writeback
    // consequence specific to writers: sync_all()/sync_data() success and a
    // later close() failure are INDEPENDENT observations on separate
    // channels — durability asked for is not durability achieved if the
    // final close reports an error, and only the caller can judge.
    Result<void> close() noexcept;
    // Returns an error if !opened(); preserves the real errno from a failed
    // open() rather than a synthetic code.
    Result<std::size_t> write_some(std::span<const std::byte> src) override;
    // POSIX writev override: scatters from all non-empty slices in (chunked)
    // syscalls. See src/file.cpp and docs/reference/sync-io-model.md
    // (Vector I/O semantics).
    Result<std::size_t> write_vec(std::span<const ConstIoSlice> srcs) override;
    // Positional write: pwrite at an explicit byte offset.
    // Does NOT move the shared file cursor. Returns bytes written (0 on
    // non-empty input is invalid_state/backend failure — surfaced by callers).
    // See docs/reference/sync-io-model.md (Positional I/O semantics).
    Result<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> src);
    // Positional vector write: pwritev at an explicit byte
    // offset. Same stop-on-short + skip-empty semantics as write_vec. Does NOT
    // move the shared file cursor.
    Result<std::size_t> write_vec_at(std::uint64_t offset, std::span<const ConstIoSlice> srcs);
    // Derived: write all of src at `offset`, looping
    // write_at across short writes and advancing offset by bytes written. Zero
    // progress on non-empty remaining input -> IoError::invalid_state.
    Result<void> write_at_all(std::uint64_t offset, std::span<const std::byte> src);
    // No-op flush of user-space state (no fsync). Durability is INTENTIONALLY
    // separate — see sync_data/sync_all below and
    // docs/architecture/sync-durability-model.md. flush() must never call
    // fsync/fdatasync.
    Result<void> flush() override { return {}; }
    // Request persistence of file data (fdatasync). See
    // docs/architecture/sync-durability-model.md.
    Result<void> sync_data() override;
    // Request persistence of file data + metadata (fsync).
    Result<void> sync_all() override;

  private:
    int fd_ = -1;
    std::optional<IoError> open_error_;
    SyscallStats* stats_ = nullptr;
    VectorStats* vec_stats_ = nullptr;
    SyncStats* sync_stats_ = nullptr;
};

} // namespace sluice
