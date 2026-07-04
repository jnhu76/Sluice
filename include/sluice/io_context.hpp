// sluice::IoContext — abstract backend capability boundary (CPPIO-CORE-009).
//
// IoContext is an abstract factory for Reader/Writer handles, so future backends
// (async, io_uring, test fakes) can plug in without callers changing how they
// obtain handles. The direct FileReader/FileWriter constructors remain valid;
// IoContext is an additional construction boundary for code that wants backend
// indirection. See docs/io-context.md.
//
// IoContext is NOT async. IoContext is NOT io_uring. It is the backend
// capability boundary.
#pragma once

#include <sluice/measurement.hpp>
#include <sluice/reader.hpp>
#include <sluice/result.hpp>
#include <sluice/writer.hpp>

#include <memory>
#include <string_view>

namespace sluice {

// Options for opening a Reader through an IoContext. All stats pointers are
// optional (null = no counting) and caller-owned.
struct OpenReaderOptions {
    SyscallStats* syscall_stats = nullptr;
    VectorStats* vector_stats = nullptr;
};

// Options for opening a Writer through an IoContext.
struct OpenWriterOptions {
    SyscallStats* syscall_stats = nullptr;
    VectorStats* vector_stats = nullptr;
    SyncStats* sync_stats = nullptr;
};

// Abstract I/O capability boundary. Concrete contexts (e.g. BlockingIoContext)
// implement open_reader/open_writer to construct backend-owned handles. Errors
// are returned at open time where feasible (unlike the direct FileReader/
// FileWriter constructors, which defer open errors to first I/O).
class IoContext {
  public:
    virtual ~IoContext() = default;

    // Open `path` for reading. On success returns a Reader handle the caller
    // owns; on failure returns the open error immediately. [[nodiscard]]: an
    // open error must never be silently dropped.
    [[nodiscard]] virtual Result<std::unique_ptr<Reader>>
    open_reader(std::string_view path, OpenReaderOptions options = {}) = 0;

    // Create/truncate `path` for writing. On success returns a Writer handle the
    // caller owns; on failure returns the open error immediately. [[nodiscard]]:
    // an open error must never be silently dropped.
    [[nodiscard]] virtual Result<std::unique_ptr<Writer>>
    open_writer(std::string_view path, OpenWriterOptions options = {}) = 0;
};

// Concrete blocking-POSIX context (CPPIO-CORE-009C). Constructs FileReader/
// FileWriter under the hood and surfaces open errors at open time (rather than
// deferring to first I/O). No async, no thread pool, no io_uring.
class BlockingIoContext final : public IoContext {
  public:
    Result<std::unique_ptr<Reader>> open_reader(std::string_view path,
                                                OpenReaderOptions options = {}) override;

    Result<std::unique_ptr<Writer>> open_writer(std::string_view path,
                                                OpenWriterOptions options = {}) override;
};

} // namespace sluice
