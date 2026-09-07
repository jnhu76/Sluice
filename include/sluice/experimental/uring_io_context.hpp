



#pragma once

#include <sluice/experimental/uring_write_batch.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace sluice::experimental {

class UringIoContext {
  public:
    explicit UringIoContext(unsigned queue_depth = 64);


    Result<UringWriteResult> write_file_all(std::string_view path,
                                            std::span<const std::byte> bytes);


    void set_stats(UringStats* stats) { batch_.set_stats(stats); }

  private:
    UringWriteBatch batch_;
};

}
