#pragma once

#include <sluice/fault.hpp>
#include <sluice/io_context.hpp>
#include <sluice/result.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sluice {

class MemoryIoContext final : public IoContext {
  public:
    MemoryIoContext() = default;

    void seed(std::string_view path, std::vector<std::byte> bytes) {
        store_[std::string(path)] = std::move(bytes);
    }

    [[nodiscard]] Result<std::unique_ptr<Reader>> open_reader(std::string_view path,
                                                              OpenReaderOptions = {}) override {
        auto it = store_.find(std::string(path));
        if (it == store_.end()) {
            return make_unexpected<std::unique_ptr<Reader>>(
                IoError{IoError::Code::permission_denied});
        }

        return std::unique_ptr<Reader>(std::make_unique<MemoryReader>(it->second));
    }

    [[nodiscard]] Result<std::unique_ptr<Writer>> open_writer(std::string_view,
                                                              OpenWriterOptions = {}) override {
        return std::unique_ptr<Writer>(std::make_unique<MemoryWriter>());
    }

  private:
    std::unordered_map<std::string, std::vector<std::byte>> store_;
};

} // namespace sluice
