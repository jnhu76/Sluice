// cppio BufferedReader / BufferedWriter — interface-level buffering wrappers
// inspired by Zig std.Io's interface-owned buffer model. The caller supplies
// (and owns) the backing storage; the wrapper tracks fill/dirty positions.
#pragma once

#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cppio/measurement.hpp>

#include <cassert>
#include <cstddef>
#include <span>

namespace cppio {

// Reads from an inner Reader through a caller-owned buffer, collapsing many
// small reads into fewer inner reads while preserving byte order and EOF.
class BufferedReader final : public Reader {
public:
    // The wrapper holds the span; the caller must keep the backing storage
    // alive for the wrapper's lifetime. Buffer must be non-empty. If `stats`
    // is non-null, buffer hit/miss/refill counters are recorded there.
    BufferedReader(Reader& inner, std::span<std::byte> buffer, BufferStats* stats = nullptr)
        : inner_(inner), buf_(buffer), stats_(stats) {
        assert(!buffer.empty() && "BufferedReader requires a non-empty backing buffer");
    }

    // Wrappers hold a reference to the inner reader and mutable cursor state;
    // copying or moving them would alias that state and corrupt accounting.
    BufferedReader(const BufferedReader&) = delete;
    BufferedReader& operator=(const BufferedReader&) = delete;
    BufferedReader(BufferedReader&&) = delete;
    BufferedReader& operator=(BufferedReader&&) = delete;

    Result<std::size_t> read_some(std::span<std::byte> dst) override;

private:
    Reader& inner_;
    std::span<std::byte> buf_;
    std::size_t seek_ = 0;  // consumed position within buf_
    std::size_t end_ = 0;   // one past last valid byte within buf_
    BufferStats* stats_ = nullptr;
};

// Writes to an inner Writer through a caller-owned buffer, coalescing small
// writes. Dirty bytes are flushed on overflow and on explicit flush(). The
// destructor does NOT flush — callers must flush() before destruction to
// avoid silent data loss, matching the Zig model.
class BufferedWriter final : public Writer {
public:
    BufferedWriter(Writer& inner, std::span<std::byte> buffer, BufferStats* stats = nullptr)
        : inner_(inner), buf_(buffer), stats_(stats) {
        assert(!buffer.empty() && "BufferedWriter requires a non-empty backing buffer");
    }

    // Not copyable or movable: see BufferedReader for rationale.
    BufferedWriter(const BufferedWriter&) = delete;
    BufferedWriter& operator=(const BufferedWriter&) = delete;
    BufferedWriter(BufferedWriter&&) = delete;
    BufferedWriter& operator=(BufferedWriter&&) = delete;

    Result<std::size_t> write_some(std::span<const std::byte> src) override;
    Result<void> flush() override;

    // No silent flush in destructor: a flush that fails cannot be reported
    // from a destructor, so we require explicit flush() before destruction.
    // Debug-only assert catches the common "forgot to flush" misuse loudly;
    // suppressed when flush() already reported an error (that path is known
    // to leave dirty bytes — the caller has been notified). Compiled out under
    // NDEBUG (the release-safe guard in write_some handles the empty-buffer
    // precondition, not this).
    ~BufferedWriter() override {
        assert((end_ == 0 || flush_ever_failed_) &&
               "BufferedWriter destroyed with unflushed dirty bytes (did you forget flush()?)");
    }

private:
    // Push buf_[0..end_) to the inner writer, retrying short writes.
    Result<void> flush_dirty();

    Writer& inner_;
    std::span<std::byte> buf_;
    std::size_t end_ = 0;  // one past last dirty byte within buf_
    bool flush_ever_failed_ = false;  // suppresses the destructor assert
    BufferStats* stats_ = nullptr;
};

}  // namespace cppio
