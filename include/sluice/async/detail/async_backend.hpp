#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/progress_source.hpp>
#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/detail/request_key.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace sluice::async {

// Uninstalled seam: the shipped backends, the library TUs and the isolated
// seam tests are the only consumers.
class AsyncBackend {
  public:
    virtual ~AsyncBackend() = default;
    AsyncBackend(const AsyncBackend&) = delete;
    AsyncBackend& operator=(const AsyncBackend&) = delete;

    void attach_stats(AsyncStats* s) { stats_ = s; }

    void attach_ready_sink(detail::SynchronousReadySink* sink) noexcept { routing_sink_ = sink; }

    // Installed exactly once by the AsyncIoContext constructor before the
    // backend can produce any work. There is no rebind path. A backend that
    // cannot install its required progress-notification binding fails the
    // context construction.
    void attach_progress_port(detail::BackendProgressPort port) {
        progress_port_ = port;
        if (!progress_port_attached()) {
            throw std::runtime_error(
                "sluice::async::AsyncBackend: progress notification binding install failed");
        }
    }

    virtual std::size_t poll() = 0;

    virtual ProgressPass poll_progress() {
        ProgressPass pass;
        pass.completed = poll();
        pass.accepted_work_remains = outstanding() != 0;
        return pass;
    }

    virtual std::size_t outstanding() const noexcept = 0;

    // retire_execution_resources is valid only after internal_work_retired
    // holds; internal_work_retired must report a real retirement witness, so
    // no default is provided.
    virtual bool internal_work_retired() const noexcept = 0;
    virtual void stop_execution() noexcept {}
    virtual void retire_execution_resources() noexcept {}

    virtual bool signals_physical_progress() const noexcept { return false; }

  private:
    friend class AsyncIoContext;
    friend CancelDisposition detail::RequestBridge::cancel(AsyncBackend*, const RequestId&) noexcept;

    // Nonblocking probe of immediately actionable backend-physical work
    // (kernel CQ state for io_uring). Kernel producers bypass the userspace
    // progress epochs, so the park handshake needs this physical answer as
    // its final actionable-work check. Producers that signal through the
    // context source are covered by token revalidation and answer false.
    virtual bool has_immediate_physical_work() const noexcept { return false; }

    // Accepts and linearizes one operation; the returned key is the accepted
    // request identity in every accepting case.
    virtual Result<detail::RequestKey> submit_read(ReadOp op) = 0;
    virtual Result<detail::RequestKey> submit_write(WriteOp op) = 0;
    virtual Result<detail::RequestKey> submit_sync_data(SyncDataOp op) = 0;
    virtual Result<detail::RequestKey> submit_sync_all(SyncAllOp op) = 0;
    virtual Result<detail::RequestKey> submit_file_info(FileInfoOp op) = 0;
    virtual Result<detail::RequestKey> submit_size(SizeOp op) = 0;

    // The capacity of the request slot table this backend serves; the
    // context-owned core is sized by it.
    virtual std::size_t slot_capacity() const noexcept = 0;

    virtual detail::PublicCancel cancel_identity(detail::RequestKey key) = 0;

  protected:
    AsyncBackend() = default;
    AsyncStats* stats_ = nullptr;

    detail::SynchronousReadySink* routing_sink_ = nullptr;

    detail::BackendProgressPort progress_port_{};

    // Runs once after the context installs the progress port, before any work
    // can exist. Backends install their kernel progress-notification binding
    // here; false fails the context construction. They cannot replace, query,
    // or unbind the port.
    virtual bool progress_port_attached() noexcept { return true; }

    // Installed only by the AsyncIoContext constructor; backends never set
    // or clear it, and the backend retires before the core storage does.
    detail::RequestCore* core_ = nullptr;
};

namespace detail {

// Internal construction pathway shared by the named backend factories and
// the isolated seam tests; befriended by AsyncIoContext.
struct ContextFactory {
    static AsyncIoContext make(std::unique_ptr<AsyncBackend> backend,
                               AsyncStats* stats = nullptr);
};

AsyncIoContext make_context(std::unique_ptr<AsyncBackend> backend, AsyncStats* stats = nullptr);

}

}
