



















































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

class FakeAsyncBackend : public AsyncBackend {
  public:
    explicit FakeAsyncBackend(std::size_t request_capacity = kDefaultCapacity)
        : arena_(detail::ContextIdentity::for_testing(next_backend_id()), request_capacity) {}
    ~FakeAsyncBackend() override = default;

















    void auto_bytes(std::size_t n) {
        auto_mode_ = Auto::bytes;
        auto_bytes_ = n;
    }
    void auto_error(IoError e) {
        auto_mode_ = Auto::err;
        auto_err_ = e;
    }
    void auto_eof() { auto_bytes(0); }
    void auto_disable() { auto_mode_ = Auto::off; }
    void auto_short_then_full(std::size_t first_short) {
        auto_mode_ = Auto::short_then_full;
        auto_bytes_ = first_short;
        auto_short_used_ = false;
    }


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
        return arena_.identity_handle_state(detail::SlotIndex{slot},
                                            detail::Generation{gen},
                                            detail::ContextIdentity{ctx});
    }

  public:
















    void complete_oldest_with_bytes(std::size_t n) {
        resolve_size_terminal(detail::TerminalResult::ok_bytes(n));
    }

    void complete_oldest_with_error(IoError e) {
        resolve_size_terminal(detail::TerminalResult::err(e));
    }

    void complete_oldest_sync_ok() {
        resolve_void_terminal(detail::TerminalResult::ok_void());
    }

    void complete_oldest_sync_error(IoError e) {
        resolve_void_terminal(detail::TerminalResult::err(e));
    }





    std::size_t poll() override { return dispatch_and_reap(); }

    Result<std::size_t> wait_one() override {

        return dispatch_and_reap();
    }







    bool wait_one_is_nonblocking() const noexcept override { return true; }














    Result<void> register_waiter(Completion<std::size_t>& c,
                                 detail::WaiterToken token,
                                 detail::RoutingLease lease) override {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        return arena_.register_waiter(*h, token, std::move(lease));
    }
    Result<void> register_waiter(Completion<void>& c,
                                 detail::WaiterToken token,
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
            return make_unexpected<detail::RoutingLease>(
                IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }
    Result<detail::RoutingLease> cancel_waiter(Completion<void>& c) override {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<detail::RoutingLease>(
                IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }






















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

    std::size_t outstanding() const noexcept override { return arena_.accepted_outstanding(); }
















    void close_admission() {
        std::lock_guard<std::mutex> lk(admission_mtx_);
        arena_.close_admission();
    }


    std::size_t arena_capacity() const noexcept { return arena_.capacity(); }
    std::size_t arena_slot_in_use() const noexcept { return arena_.slot_in_use(); }
    std::size_t arena_capacity_rejections() const noexcept { return arena_.capacity_rejections(); }


#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::size_t sink_deliveries() const noexcept { return sink_.deliveries(); }
#endif
    bool arena_enqueue_pin_live(std::uint32_t slot) const noexcept {
        return arena_.enqueue_pin_live(detail::SlotIndex{slot});
    }
    bool arena_state_is(std::uint32_t slot, detail::RequestState st) const noexcept {
        return arena_.state_of(detail::SlotIndex{slot}) == st;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)






    struct SubmitPauseGate;
    void set_submit_pause_after_commit(SubmitPauseGate* gate) noexcept;





    std::optional<detail::SlotHandle> handle_for_completion_for_test(
        const void* completion) const noexcept {
        return arena_.resolve_completion(completion);
    }











    detail::CancelDisposition cancel_handle_for_test(detail::SlotHandle h) noexcept {
        detail::CancelDisposition disp = arena_.cancel(h);
        if (disp == detail::CancelDisposition::terminal_won) {
            tally_canceled();
        }
        return disp;
    }













    Result<void> register_waiter_for_test(Completion<std::size_t>& c,
                                          detail::WaiterToken token,
                                          detail::RoutingLease lease) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        return arena_.register_waiter(*h, token, std::move(lease));
    }
    Result<void> register_waiter_for_test(Completion<void>& c,
                                          detail::WaiterToken token,
                                          detail::RoutingLease lease) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        return arena_.register_waiter(*h, token, std::move(lease));
    }





    Result<detail::RoutingLease> cancel_waiter_for_test(Completion<std::size_t>& c) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<detail::RoutingLease>(
                IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }
    Result<detail::RoutingLease> cancel_waiter_for_test(Completion<void>& c) {
        auto h = arena_.resolve_completion(&c);
        if (!h.has_value()) {
            return make_unexpected<detail::RoutingLease>(
                IoError{IoError::Code::not_found});
        }
        return arena_.cancel_waiter(*h);
    }








    Result<void> register_waiter_handle_for_test(detail::SlotHandle h,
                                                 detail::WaiterToken token,
                                                 detail::RoutingLease lease) {
        return arena_.register_waiter(h, token, std::move(lease));
    }
    Result<detail::RoutingLease> cancel_waiter_handle_for_test(detail::SlotHandle h) {
        return arena_.cancel_waiter(h);
    }


    std::optional<detail::RequestArena::BorrowSnapshot> borrow_for_test(
        detail::SlotHandle h) const noexcept {
        return arena_.borrow_for_test(h);
    }


    std::optional<detail::RequestArena::WaiterObservation> waiter_for_test(
        detail::SlotHandle h) const noexcept {
        return arena_.waiter_for_test(h);
    }




    bool sink_last_has_waiter() const noexcept { return sink_.last_has_waiter(); }
    detail::WaiterToken sink_last_token() const noexcept { return sink_.last_token(); }
    std::uint64_t sink_last_lease_id() const noexcept { return sink_.last_lease_id(); }
#endif

  private:
    static constexpr std::size_t kDefaultCapacity = 64;

    static std::uint64_t next_backend_id() noexcept {
        static std::atomic<std::uint64_t> id{0x4A410000u};
        return ++id;
    }







    void resolve_size_terminal(detail::TerminalResult res) {
        auto oh = arena_.oldest_enqueued_of(detail::OperationKind::read);




        auto wh = arena_.oldest_enqueued_of(detail::OperationKind::write);
        std::optional<detail::SlotHandle> target;
        if (oh.has_value() && wh.has_value()) {
            target = (arena_.submit_seq_of(oh->slot) <= arena_.submit_seq_of(wh->slot))
                         ? oh : wh;
        } else if (oh.has_value()) {
            target = oh;
        } else {
            target = wh;
        }
        if (!target.has_value()) return;
        bool won = arena_.record_terminal(*target, res);
        tally_terminal_result(won, res);
    }


    void resolve_void_terminal(detail::TerminalResult res) {
        auto dh = arena_.oldest_enqueued_of(detail::OperationKind::sync_data);
        auto ah = arena_.oldest_enqueued_of(detail::OperationKind::sync_all);
        std::optional<detail::SlotHandle> target;
        if (dh.has_value() && ah.has_value()) {
            target = (arena_.submit_seq_of(dh->slot) <= arena_.submit_seq_of(ah->slot))
                         ? dh : ah;
        } else if (dh.has_value()) {
            target = dh;
        } else {
            target = ah;
        }
        if (!target.has_value()) return;
        bool won = arena_.record_terminal(*target, res);
        tally_terminal_result(won, res);
    }













    template <class Op>
    Result<void> submit_size(Op op, Completion<std::size_t>& c, detail::OperationKind kind) {
        detail::SlotHandle h{};
        {
            std::lock_guard<std::mutex> admission_lk(admission_mtx_);
            SubmitPolicy<Op, Completion<std::size_t>> policy{*this, kind};
            auto r = detail::submit_transaction(arena_, c, op, policy);
            if (!r.has_value()) {
                return make_unexpected<void>(r.error());
            }
            h = r.value();
        }


        (void)arena_.enqueue(h);
        return {};
    }

    template <class Op>
    Result<void> submit_void(Op op, Completion<void>& c, detail::OperationKind kind) {
        detail::SlotHandle h{};
        {
            std::lock_guard<std::mutex> admission_lk(admission_mtx_);
            SubmitPolicy<Op, Completion<void>> policy{*this, kind};
            auto r = detail::submit_transaction(arena_, c, op, policy);
            if (!r.has_value()) {
                return make_unexpected<void>(r.error());
            }
            h = r.value();
        }
        (void)arena_.enqueue(h);
        return {};
    }










    template <class Op, class Comp>
    struct SubmitPolicy {
        using completion_type = Comp;
        using op_type = Op;

        SubmitPolicy(FakeAsyncBackend& self, detail::OperationKind kind) noexcept
            : self_(self), kind_(kind) {}


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
                return &FakeAsyncBackend::publish_size_ready;
            } else {
                return &FakeAsyncBackend::publish_void_ready;
            }
        }


        static bool begin_binding(Comp& c) noexcept {
            return FakeAsyncBackend::begin_binding(c);
        }
        static void install_binding(Comp& c, detail::RequestArena* arena,
                                    detail::SlotHandle h) noexcept {
            FakeAsyncBackend::install_binding(c, arena, h);
        }
        static void commit_binding(Comp& c) noexcept {
            FakeAsyncBackend::commit_binding(c);
        }
        static void rollback_binding(Comp& c) noexcept {
            FakeAsyncBackend::rollback_binding_before_accept(c);
        }

        Result<void> stage0_precheck() const noexcept { return {}; }
        Result<void> validate(const Op&) const noexcept { return {}; }
        void write_scratch(detail::SlotHandle, const Op&) const noexcept {}
        void pause_before_commit_binding() noexcept {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)







            self_.wait_submit_pause_();
#endif
        }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)


        std::optional<IoError> injected_precommit_stage_failure(
            detail::SubmitStage) const noexcept {
            return std::nullopt;
        }
#endif

      private:
        FakeAsyncBackend& self_;
        detail::OperationKind kind_;
    };


    template <class Op>
    static detail::BorrowMetadata borrow_of(const Op& op) {
        if constexpr (std::is_same_v<Op, ReadOp>) {
            return {op.fd, op.dst, op.len};
        } else {
            return {op.fd, op.src, op.len};
        }
    }







    std::size_t dispatch_and_reap() {
        if (auto_mode_ != Auto::off) {
            drain_auto_size();
            drain_auto_void();
        }




        return arena_.reap(routing_sink_ ? *routing_sink_ : sink_);
    }

    void drain_auto_size() {



        for (;;) {
            auto oh = arena_.oldest_enqueued_of(detail::OperationKind::read);
            auto wh = arena_.oldest_enqueued_of(detail::OperationKind::write);
            std::optional<detail::SlotHandle> target;
            if (oh.has_value() && wh.has_value()) {
                target = (arena_.submit_seq_of(oh->slot) <= arena_.submit_seq_of(wh->slot))
                             ? oh : wh;
            } else if (oh.has_value()) {
                target = oh;
            } else {
                target = wh;
            }
            if (!target.has_value()) break;
            std::size_t requested =
                static_cast<std::size_t>(arena_.requested_bytes_of(target->slot));
            detail::TerminalResult res = auto_size_result(requested);
            bool won = arena_.record_terminal(*target, res);
            tally_terminal_result(won, res);
        }
    }
    void drain_auto_void() {
        for (;;) {
            auto dh = arena_.oldest_enqueued_of(detail::OperationKind::sync_data);
            auto ah = arena_.oldest_enqueued_of(detail::OperationKind::sync_all);
            std::optional<detail::SlotHandle> target;
            if (dh.has_value() && ah.has_value()) {
                target = (arena_.submit_seq_of(dh->slot) <= arena_.submit_seq_of(ah->slot))
                             ? dh : ah;
            } else if (dh.has_value()) {
                target = dh;
            } else {
                target = ah;
            }
            if (!target.has_value()) break;
            detail::TerminalResult res = (auto_mode_ == Auto::err)
                                             ? detail::TerminalResult::err(auto_err_)
                                             : detail::TerminalResult::ok_void();



            bool won = arena_.record_terminal(*target, res);
            tally_terminal_result(won, res);
        }
    }



    detail::TerminalResult auto_size_result(std::size_t requested) {
        switch (auto_mode_) {
        case Auto::bytes:
            return detail::TerminalResult::ok_bytes(auto_bytes_);
        case Auto::err:
            return detail::TerminalResult::err(auto_err_);
        case Auto::short_then_full:
            if (!auto_short_used_) {
                auto_short_used_ = true;
                return detail::TerminalResult::ok_bytes(auto_bytes_);
            }
            return detail::TerminalResult::ok_bytes(requested);
        default:
            return detail::TerminalResult::ok_bytes(requested);
        }
    }








    static void publish_size_ready(void* completion,
                                   const detail::TerminalResult& t) noexcept {
        AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion),
                              terminal_to_size(t));
    }
    static void publish_void_ready(void* completion,
                                   const detail::TerminalResult& t) noexcept {
        AsyncBackend::publish(*static_cast<Completion<void>*>(completion),
                              terminal_to_void(t));
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
        if (stats_) ++stats_->canceled_ops;
    }
    void tally_terminal_result(bool won, const detail::TerminalResult& t) noexcept {
        if (!stats_ || !won || !t.stored || !t.is_error) return;
        if (t.error.code == IoError::Code::canceled) {
            ++stats_->canceled_ops;
        } else {
            ++stats_->completion_errors;
        }
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    void wait_submit_pause_() noexcept;
    std::atomic<SubmitPauseGate*> submit_pause_gate_{nullptr};
#endif

    detail::RequestArena arena_;
    detail::ReferenceReadySink sink_;







    mutable std::mutex admission_mtx_;









    enum class Auto : std::uint8_t { off, bytes, err, short_then_full };
    Auto auto_mode_ = Auto::off;
    std::size_t auto_bytes_ = 0;
    IoError auto_err_{IoError::Code::backend_error};
    bool auto_short_used_ = false;
};

}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)




#include "fake_test_seams.hpp"
#endif
