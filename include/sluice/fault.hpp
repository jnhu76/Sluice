// sluice fault-injection wrappers + minimal in-memory Reader/Writer sinks.
//
// MemoryReader/MemoryWriter are zero-dependency in-process sources/sinks used
// both by tests and by the fault wrappers' default backing store. FaultReader/
// FaultWriter wrap any Reader/Writer and deterministically inject short I/O
// and failures per a FaultPlan. They never mutate data.
#pragma once

#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace sluice {

// ---------- In-memory sinks ----------

class MemoryWriter final : public Writer {
public:
    MemoryWriter() = default;
    explicit MemoryWriter(std::vector<std::byte> initial) : buf_(std::move(initial)) {}

    static MemoryWriter from_string(std::string_view s) {
        auto* p = reinterpret_cast<const std::byte*>(s.data());
        return MemoryWriter({p, p + s.size()});
    }

    Result<std::size_t> write_some(std::span<const std::byte> src) override {
        buf_.insert(buf_.end(), src.begin(), src.end());
        return src.size();
    }
    Result<void> flush() override { return {}; }

    const std::vector<std::byte>& bytes() const { return buf_; }
    std::vector<std::byte> take() { return std::move(buf_); }

private:
    std::vector<std::byte> buf_;
};

class MemoryReader final : public Reader {
public:
    MemoryReader() = default;
    explicit MemoryReader(std::vector<std::byte> data) : buf_(std::move(data)) {}

    static MemoryReader from_string(std::string_view s) {
        auto* p = reinterpret_cast<const std::byte*>(s.data());
        return MemoryReader({p, p + s.size()});
    }

    // Convenience factory from a byte span (CPPIO-CORE-015B). Mirrors
    // from_string; COPIES the span into owned storage so the caller's buffer
    // need not outlive the reader (no dangling reference). Empty span is fine.
    static MemoryReader from_bytes(std::span<const std::byte> bytes) {
        return MemoryReader({bytes.begin(), bytes.end()});
    }

    Result<std::size_t> read_some(std::span<std::byte> dst) override {
        std::size_t n = std::min(dst.size(), buf_.size() - pos_);
        // Guard the memcpy: with n==0 either buf_.data() (empty source) or
        // dst.data() (empty request) may be null, and passing null to memcpy
        // is UB even for a zero-length copy.
        if (n != 0) {
            std::memcpy(dst.data(), buf_.data() + pos_, n);
        }
        pos_ += n;
        return n;
    }

    std::size_t remaining() const { return buf_.size() - pos_; }

private:
    std::vector<std::byte> buf_;
    std::size_t pos_ = 0;
};

// ---------- Fault injection ----------

struct FaultPlan {
    std::optional<std::uint64_t> fail_after_read_calls;
    std::optional<std::uint64_t> fail_after_write_calls;
    std::optional<std::uint64_t> fail_after_bytes;
    std::optional<std::size_t> max_read_size;
    std::optional<std::size_t> max_write_size;
    bool fail_flush = false;
    IoError error = IoError{IoError::Code::backend_error, 0};
};

class FaultReader final : public Reader {
public:
    FaultReader(Reader& inner, const FaultPlan& plan)
        : inner_(inner), plan_(plan) {}

    // Not copyable or movable: holds a reference + mutable counters.
    FaultReader(const FaultReader&) = delete;
    FaultReader& operator=(const FaultReader&) = delete;
    FaultReader(FaultReader&&) = delete;
    FaultReader& operator=(FaultReader&&) = delete;

    Result<std::size_t> read_some(std::span<std::byte> dst) override;

private:
    Reader& inner_;
    FaultPlan plan_;
    std::uint64_t read_calls_ = 0;
    std::uint64_t bytes_seen_ = 0;
};

class FaultWriter final : public Writer {
public:
    FaultWriter(Writer& inner, const FaultPlan& plan)
        : inner_(inner), plan_(plan) {}

    // Not copyable or movable: holds a reference + mutable counters.
    FaultWriter(const FaultWriter&) = delete;
    FaultWriter& operator=(const FaultWriter&) = delete;
    FaultWriter(FaultWriter&&) = delete;
    FaultWriter& operator=(FaultWriter&&) = delete;

    Result<std::size_t> write_some(std::span<const std::byte> src) override;
    Result<void> flush() override;

private:
    Writer& inner_;
    FaultPlan plan_;
    std::uint64_t write_calls_ = 0;
    std::uint64_t bytes_seen_ = 0;
};

}  // namespace sluice
