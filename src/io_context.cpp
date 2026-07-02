// BlockingIoContext implementation (CPPIO-CORE-009C). Wraps FileReader/
// FileWriter construction and surfaces open errors at open time.
#include <cppio/io_context.hpp>
#include <cppio/file.hpp>

#include <memory>
#include <string>
#include <utility>

namespace cppio {

Result<std::unique_ptr<Reader>>
BlockingIoContext::open_reader(std::string_view path, OpenReaderOptions options) {
    // FileReader takes const std::string&; copy the view into owned storage.
    auto reader = std::make_unique<FileReader>(
        std::string(path), options.syscall_stats, options.vector_stats);
    if (!reader->opened()) {
        // Surface the real open error immediately instead of deferring to first
        // read (the direct-constructor behavior).
        return make_unexpected<std::unique_ptr<Reader>>(
            reader->open_error().value_or(IoError{IoError::Code::permission_denied}));
    }
    return std::unique_ptr<Reader>(std::move(reader));
}

Result<std::unique_ptr<Writer>>
BlockingIoContext::open_writer(std::string_view path, OpenWriterOptions options) {
    auto writer = std::make_unique<FileWriter>(
        std::string(path), options.syscall_stats, options.vector_stats, options.sync_stats);
    if (!writer->opened()) {
        return make_unexpected<std::unique_ptr<Writer>>(
            writer->open_error().value_or(IoError{IoError::Code::permission_denied}));
    }
    return std::unique_ptr<Writer>(std::move(writer));
}

}  // namespace cppio
