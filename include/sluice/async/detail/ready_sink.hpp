#pragma once

#include <sluice/async/detail/request_key.hpp>

#include <utility>

namespace sluice::async::detail {

enum class OperationKind : std::uint8_t {
    read,
    write,
    sync_data,
    sync_all,
};

struct ReadyEvent {
    RequestKey key{};
    OperationKind kind = OperationKind::read;
};

class SynchronousReadySink {
  public:
    virtual ~SynchronousReadySink() = default;
    virtual void on_ready(ReadyEvent event) noexcept = 0;
};

}
