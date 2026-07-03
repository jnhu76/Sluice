// cppio::MemoryIoContext — deterministic in-memory IoContext for tests/examples
// (CPPIO-CORE-015C). Seeded paths open as MemoryReader (independent copy of the
// stored bytes); open_writer returns a fresh independent MemoryWriter. Does NOT
// change BlockingIoContext or default backend selection. There is no persistent
// backing store: writes go to the returned MemoryWriter's own sink, not back
// into the context's seed map.
#pragma once

#include <cppio/fault.hpp>
#include <cppio/io_context.hpp>
#include <cppio/result.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cppio {

class MemoryIoContext final : public IoContext {
public:
    MemoryIoContext() = default;

    // Seed a readable path with the given bytes. Overwrites any prior seed.
    void seed(std::string_view path, std::vector<std::byte> bytes) {
        store_[std::string(path)] = std::move(bytes);
    }

    [[nodiscard]] Result<std::unique_ptr<Reader>>
    open_reader(std::string_view path, OpenReaderOptions /*options*/ = {}) override {
        auto it = store_.find(std::string(path));
        if (it == store_.end()) {
            // No dedicated not_found code; use permission_denied, the codebase's
            // established mapping for "cannot open this path" (BlockingIoContext
            // maps ENOENT the same way).
            return make_unexpected<std::unique_ptr<Reader>>(
                IoError{IoError::Code::permission_denied});
        }
        // Return an independent copy so the caller's reads don't mutate the seed.
        return std::unique_ptr<Reader>(std::make_unique<MemoryReader>(it->second));
    }

    [[nodiscard]] Result<std::unique_ptr<Writer>>
    open_writer(std::string_view /*path*/, OpenWriterOptions /*options*/ = {}) override {
        // A fresh, independent in-memory writer. The path is accepted (so callers
        // can use a consistent name) but writes are NOT stored back into the seed
        // map; this is test/demo ergonomics, not a filesystem.
        return std::unique_ptr<Writer>(std::make_unique<MemoryWriter>());
    }

private:
    std::unordered_map<std::string, std::vector<std::byte>> store_;
};

}  // namespace cppio
