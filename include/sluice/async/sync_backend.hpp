#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/async/detail/submit_transaction.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace sluice::async {

class SyncBackend : public AsyncBackend {
  public:
    explicit SyncBackend(std::size_t request_capacity = kDefaultCapacity)
        : arena_(detail::ContextIdentity::for_testing(next_backend_id()), request_capacity) {}

    ~SyncBackend() override {}

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        return submit_size(op, c, detail::OperationKind::read);
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        return submit_size(op, c, detail::OperationKind::write);
    }
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) override {
        return submit_void(op, c, detail::OperationKind::sync_data);
    }
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) override {
        return submit_void(op, c, detail::OperationKind::sync_all);
    }

    bool supports_request_identity() const noexcept override { return true; }

  private:
    Result<RequestHandleState> resolve_identity_state(std::uint64_t ctx, std::uint32_t slot,
                                                      std::uint64_t gen) const override {
        return arena_.identity_handle_state(detail::SlotIndex{slot}, detail::Generation{gen},
                                            detail::ContextIdentity{ctx});
    }

  public:
    std::size_t poll() override { return dispatch_and_reap(); }

    Result<std::size_t> wait_one() override { return dispatch_and_reap(); }

    bool wait_one_is_nonblocking() const noexcept override { return true; }

    void cancel(Completion<std::size_t>& c) override {
        auto h = arena_.resolve_completion(&c);
        if (h.has_value()) {
            if (arena_.cancel(*h) == detail::CancelDisposition::terminal_won) {
                tally_canceled();
            }
        }
    }
    void cancel(Completion<void>& c) override {
        auto h = arena_.resolve_completion(&c);
        if (h.has_value()) {
            if (arena_.cancel(*h) == detail::CancelDisposition::terminal_won) {
                tally_canceled();
            }
        }
    }

    Result<void> register_waiter(Completion<std::size_t>& c, detail::WaiterToken token,
                                 detail::RoutingLease lease) override {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        return arena_.register_waiter(*h, token, std::move(lease));
    }
    Result<void> register_waiter(Completion<void>& c, detail::WaiterToken token,
                                 detail::RoutingLease lease) override {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        return arena_.register_waiter(*h, token, std::move(lease));
    }
    Result<detail::RoutingLease> cancel_waiter(Completion<std::size_t>& c) override {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }
    Result<detail::RoutingLease> cancel_waiter(Completion<void>& c) override {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }

    std::size_t outstanding() const noexcept override { return arena_.accepted_outstanding(); }

    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::size_t sink_deliveries() const noexcept { return sink_.deliveries(); }
#endif

  private:
    static constexpr std::size_t kDefaultCapacity = 64;

    static std::uint64_t next_backend_id() noexcept {
        static std::atomic<std::uint64_t> id{0x51590000u};
        return ++id;
    }

    template <class Op>
    Result<void> submit_size(Op op, Completion<std::size_t>& c, detail::OperationKind kind) {
        SubmitPolicy<Op, Completion<std::size_t>> policy{kind};
        auto r = detail::submit_transaction(arena_, c, op, policy);
        if (!r.has_value()) {
            return make_unexpected<void>(r.error());
        }

        (void)arena_.enqueue(r.value());
        return {};
    }

    template <class Op>
    Result<void> submit_void(Op op, Completion<void>& c, detail::OperationKind kind) {
        SubmitPolicy<Op, Completion<void>> policy{kind};
        auto r = detail::submit_transaction(arena_, c, op, policy);
        if (!r.has_value()) {
            return make_unexpected<void>(r.error());
        }
        (void)arena_.enqueue(r.value());
        return {};
    }

    template <class Op, class Comp> struct SubmitPolicy {
        using completion_type = Comp;
        using op_type = Op;

        explicit SubmitPolicy(detail::OperationKind kind) noexcept : kind_(kind) {}

        detail::OperationKind kind() const noexcept { return kind_; }
        static detail::BorrowMetadata borrow(const Op& op) noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return borrow_of(op);
            } else {
                return detail::BorrowMetadata{op.fd, nullptr, 0};
            }
        }
        static std::uint64_t requested_bytes(const Op& op) noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return op.len;
            } else {
                return 0;
            }
        }
        static auto publish_thunk() noexcept {
            if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
                return &SyncBackend::publish_size_ready;
            } else {
                return &SyncBackend::publish_void_ready;
            }
        }

        static bool begin_binding(Comp& c) noexcept { return SyncBackend::begin_binding(c); }
        static void install_binding(Comp& c, detail::RequestArena* arena,
                                    detail::SlotHandle h) noexcept {
            SyncBackend::install_binding(c, arena, h);
        }
        static void commit_binding(Comp& c) noexcept { SyncBackend::commit_binding(c); }
        static void rollback_binding(Comp& c) noexcept {
            SyncBackend::rollback_binding_before_accept(c);
        }

        Result<void> stage0_precheck() const noexcept { return {}; }
        Result<void> validate(const Op&) const noexcept { return {}; }
        void write_scratch(detail::SlotHandle, const Op&) const noexcept {}
        void pause_before_commit_binding() const noexcept {}
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        std::optional<IoError>
        injected_precommit_stage_failure(detail::SubmitStage) const noexcept {
            return std::nullopt;
        }
#endif

      private:
        detail::OperationKind kind_;
    };

    template <class Op> static detail::BorrowMetadata borrow_of(const Op& op) {
        if constexpr (std::is_same_v<Op, ReadOp>) {
            return {op.fd, op.dst, op.len};
        } else {
            return {op.fd, op.src, op.len};
        }
    }

    void dispatch_enqueued() {
        for (std::size_t i = 0; i < arena_.capacity(); ++i) {
            detail::SlotIndex idx{static_cast<std::uint32_t>(i)};
            if (arena_.state_of(idx) != detail::RequestState::enqueued)
                continue;
            detail::SlotHandle h{idx, arena_.generation_of(idx)};
            detail::OperationKind kind = arena_.kind_of(idx);
            detail::TerminalResult res =
                (kind == detail::OperationKind::read || kind == detail::OperationKind::write)
                    ? detail::TerminalResult::ok_bytes(arena_.requested_bytes_of(idx))
                    : detail::TerminalResult::ok_void();
            (void)arena_.record_terminal(h, res);
        }
    }

    std::size_t dispatch_and_reap() {
        dispatch_enqueued();

        return arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
    }

    static void publish_size_ready(void* completion, const detail::TerminalResult& t) noexcept {
        AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion),
                              terminal_to_size(t));
    }
    static void publish_void_ready(void* completion, const detail::TerminalResult& t) noexcept {
        AsyncBackend::publish(*static_cast<Completion<void>*>(completion), terminal_to_void(t));
    }

    static Result<std::size_t> terminal_to_size(const detail::TerminalResult& t) noexcept {
        if (t.stored && t.is_error)
            return make_unexpected<std::size_t>(t.error);
        return Result<std::size_t>{static_cast<std::size_t>(t.bytes)};
    }
    static Result<void> terminal_to_void(const detail::TerminalResult& t) noexcept {
        if (t.stored && t.is_error)
            return make_unexpected<void>(t.error);
        return {};
    }

    void tally_canceled() noexcept {
        if (stats_)
            ++stats_->canceled_ops;
    }

    detail::RequestArena arena_;
    detail::ReferenceReadySink sink_;
};

} // namespace sluice::async
