









#pragma once

#include <sluice/measurement.hpp>
#include <sluice/reader.hpp>
#include <sluice/result.hpp>
#include <sluice/writer.hpp>

#include <memory>
#include <string_view>

namespace sluice {



struct OpenReaderOptions {
    SyscallStats* syscall_stats = nullptr;
    VectorStats* vector_stats = nullptr;
};


struct OpenWriterOptions {
    SyscallStats* syscall_stats = nullptr;
    VectorStats* vector_stats = nullptr;
    SyncStats* sync_stats = nullptr;
};





class IoContext {
  public:
    virtual ~IoContext() = default;




    [[nodiscard]] virtual Result<std::unique_ptr<Reader>>
    open_reader(std::string_view path, OpenReaderOptions options = {}) = 0;




    [[nodiscard]] virtual Result<std::unique_ptr<Writer>>
    open_writer(std::string_view path, OpenWriterOptions options = {}) = 0;
};




class BlockingIoContext final : public IoContext {
  public:
    Result<std::unique_ptr<Reader>> open_reader(std::string_view path,
                                                OpenReaderOptions options = {}) override;

    Result<std::unique_ptr<Writer>> open_writer(std::string_view path,
                                                OpenWriterOptions options = {}) override;
};

}
