// CQ-side machinery of UringAsyncBackend: bounded reap of completion entries,
// control-CQE resolution, and the terminal/publication handoff that retires
// the kernel borrow. The SQ side lives in uring_transport.cpp; profile setup,
// admission and the public surface live in uring_backend.cpp.

#include "uring_internal.hpp"

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/error.hpp>

#include <cstdio>
#include <cstdint>

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

namespace sluice::async {

#if defined(SLUICE_HAS_LIBURING)

void UringAsyncBackend::finalize_operation_terminal_(
    RouterEntry& route, std::size_t router_index, const detail::TerminalResult& terminal,
    const sluice::FileInfo* metadata) noexcept {
    if (router_index >= router_.size() || !route.in_use || route.terminal_delivered) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: invalid operation terminal "
                             "finalization (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    const detail::RequestKey key{core_->context(), route.handle.slot, route.handle.generation};
    const PreparedUringOp& prep = prepared_ops_[route.handle.slot.value];
    const bool is_byte_op =
        prep.kind == detail::OperationKind::read || prep.kind == detail::OperationKind::write;
    detail::TerminalCandidate candidate;
    candidate.kind = detail::TerminalCandidateKind::physical_outcome;
    if (terminal.stored && terminal.is_error) {
        // A dispatched attempt that reached the kernel and reports failure
        if (!is_byte_op) {
            if (terminal_is_kernel_cancel(terminal.error)) {
                candidate.outcome = sluice::detail::IoOutcome::failure(
                    IoError{.code = IoError::Code::canceled, .os_errno = terminal.error.os_errno});
            } else {
                candidate.outcome = sluice::detail::IoOutcome::failure(terminal.error);
            }
        } else if (terminal_is_kernel_cancel(terminal.error)) {
            candidate.outcome = sluice::detail::canceled_racing_in_flight_attempt(0);
        } else {
#if defined(SLUICE_E1_MUTANT_BYTE_FAILURE_ACCOUNTED)
            candidate.outcome = sluice::detail::IoOutcome::failure(terminal.error);
#else
            candidate.outcome = sluice::detail::failed_dispatched_attempt(terminal.error);
#endif
        }
    } else if (is_byte_op) {
        candidate.outcome = sluice::detail::IoOutcome::success(terminal.bytes);
    } else {
        candidate.outcome = sluice::detail::IoOutcome::success();
        if (metadata != nullptr) {
#if defined(SLUICE_E1_MUTANT_METADATA_DROPS_RECORD)
            candidate.has_metadata = false;
#else
            candidate.has_metadata = true;
            candidate.metadata = *metadata;
#endif
        }
    }
    if (core_->offer_terminal(key, candidate) != detail::TerminalVerdict::chosen) {
        std::fprintf(stderr, "sluice::async::UringAsyncBackend: operation terminal lost "
                             "RequestCore winner authority (invariant violation)\n");
        std::fflush(stderr);
        std::terminate();
    }

    if (cookie_terminal_is_canceled(terminal)) {
        bump(stats_, &AsyncStats::canceled_ops);
    } else if (terminal.stored && terminal.is_error) {
        bump(stats_, &AsyncStats::completion_errors);
    } else if (is_byte_op && terminal.bytes < prep.length) {
        bump(stats_, &AsyncStats::short_completions);
    }

    route.terminal_delivered = true;
#if defined(SLUICE_B1C_MUTANT_PUBLISH_BEFORE_BORROW_RETIREMENT)
    publication_pending_->push_back(route.handle);
#else
    if (core_->release_execution(key) != detail::ExecutionRelease::borrow_touch_fully_retired) {
        detail::uring_core_handoff_fail_fast();
    }
    publication_pending_->push_back(route.handle);
#endif
    if (route.control_state == RouterEntry::ControlState::none) {
        retire_router_entry_(router_index);
    }
}

void UringAsyncBackend::handle_one_cqe(std::uint64_t user_data, int res) noexcept {
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    if (is_control_cookie(user_data)) {
        const std::uint64_t target_cookie = control_target_cookie(user_data);
        if (target_cookie == 0)
            return;
        const std::size_t router_index = find_live_router_cookie_(target_cookie);
        if (router_index == router_.size())
            return;

        RouterEntry& route = router_[router_index];
        if (route.control_state != RouterEntry::ControlState::submitted ||
            live_control_sqes_.load(std::memory_order_relaxed) == 0) {
            std::fprintf(stderr, "sluice::async::UringAsyncBackend: control CQE without its "
                                 "exact submitted control reference (invariant violation)\n");
            std::fflush(stderr);
            std::terminate();
        }
        route.control_state = RouterEntry::ControlState::none;
        live_control_sqes_.fetch_sub(1, std::memory_order_relaxed);
        const detail::RequestKey key{core_->context(), route.handle.slot, route.handle.generation};
        if (core_->release_control(key) != detail::ControlRelease::released) {
            detail::uring_core_handoff_fail_fast();
        }
#if defined(SLUICE_B1C_MUTANT_CANCEL_CQE_AS_ORIGINAL_TERMINAL)
        if (!route.terminal_delivered) {
            detail::TerminalCandidate fabricated;
            fabricated.kind = detail::TerminalCandidateKind::physical_outcome;
            fabricated.outcome =
                sluice::detail::IoOutcome::failure(IoError{IoError::Code::canceled});
            if (core_->offer_terminal(key, fabricated) == detail::TerminalVerdict::chosen) {
                route.terminal_delivered = true;
                (void)core_->release_execution(key);
                publication_pending_->push_back(route.handle);
            }
        }
#endif
#if defined(SLUICE_B1C_MUTANT_RECLAIM_BEFORE_CONTROL_RETIREMENT)
        retire_router_entry_(router_index);
#else
        if (route.terminal_delivered) {
            retire_router_entry_(router_index);
        }
#endif
        return;
    }
    if (user_data == 0)
        return;

    const std::size_t router_index = find_live_router_cookie_(user_data);
    if (router_index == router_.size())
        return;
    RouterEntry& entry = router_[router_index];

    const PreparedUringOp& prep = prepared_ops_[entry.handle.slot.value];
    const bool is_byte_op =
        (prep.kind == detail::OperationKind::read || prep.kind == detail::OperationKind::write);
    const bool is_metadata_op =
        (prep.kind == detail::OperationKind::file_info || prep.kind == detail::OperationKind::size);
    sluice::FileInfo metadata;
    const sluice::FileInfo* metadata_out = nullptr;
    detail::TerminalResult terminal;
    if (res < 0) {
#if defined(SLUICE_B1C_MUTANT_DROP_ORIGINAL_OUTCOME)
        if (is_byte_op)
            return;
#endif
        terminal = detail::TerminalResult::err(sluice::from_errno_value(-res));
    } else if (is_byte_op) {
        terminal = detail::TerminalResult::ok_bytes(static_cast<std::uint64_t>(res));
    } else if (is_metadata_op) {
        if (res != 0 || (prep.statx_buffer->stx_mask & STATX_SIZE) == 0 ||
            (prep.statx_buffer->stx_mask & (STATX_TYPE | STATX_MODE)) == 0) {
            terminal = detail::TerminalResult::err(IoError{IoError::Code::backend_error});
        } else {
            metadata.kind = S_ISREG(prep.statx_buffer->stx_mode) ? sluice::FileKind::regular
                                                                 : sluice::FileKind::other;
            metadata.size = prep.statx_buffer->stx_size;
            // statx leaves unrequested-in-mask fields undefined; identity is
            // published only when the kernel actually reported the inode.
            if ((prep.statx_buffer->stx_mask & STATX_INO) != 0) {
                metadata.identity = sluice::FileIdentity{
                    static_cast<std::uint64_t>(::makedev(prep.statx_buffer->stx_dev_major,
                                                         prep.statx_buffer->stx_dev_minor)),
                    prep.statx_buffer->stx_ino};
            }
            metadata_out = &metadata;
            terminal = detail::TerminalResult::ok_void();
        }
    } else {
        terminal = detail::TerminalResult::ok_void();
    }

    finalize_operation_terminal_(entry, router_index, terminal, metadata_out);
}

void UringAsyncBackend::reap_cqes() noexcept {
    constexpr unsigned BATCH = 32;
    io_uring_cqe* cqes[BATCH];
    unsigned got = 0;
    // The pass boundary is the CQ state visible at entry: entries the kernel
    // posts during the reap (or after an overflow flush) are the next pass's
    // immediate work, reported through has_immediate_physical_work().
    const auto reap_visible_bounded = [&](std::size_t bound) noexcept {
        std::size_t reaped = 0;
        while (reaped < bound) {
            got = ::io_uring_peek_batch_cqe(&ring_state_->ring, cqes, BATCH);
            if (got == 0)
                break;
            const std::size_t room = bound - reaped;
            const unsigned take = static_cast<unsigned>(room < got ? room : got);
            for (unsigned i = 0; i < take; ++i) {
                io_uring_cqe* cqe = cqes[i];

                const std::uint64_t user_data = ::io_uring_cqe_get_data64(cqe);
                const int res = cqe->res;
                ::io_uring_cqe_seen(&ring_state_->ring, cqe);

                handle_one_cqe(user_data, res);
            }
            reaped += take;
            if (got < BATCH)
                break;
        }
    };
    reap_visible_bounded(static_cast<std::size_t>(::io_uring_cq_ready(&ring_state_->ring)));
#if defined(SLUICE_E1_MUTANT_POST_POISON_FLUSH_GATED)
    bool overflow_flush_allowed = true;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        overflow_flush_allowed = !fatal_error_.has_value();
    }
    if (overflow_flush_allowed && ::io_uring_cq_has_overflow(&ring_state_->ring)) {
#else
    if (::io_uring_cq_has_overflow(&ring_state_->ring)) {
#endif
        // Completions parked in the kernel overflow list are invisible to the
        // shared-memory peek until an enter flushes them into the ring, so the
        // flush must stay live after poison: this enter submits nothing and
        // cannot consume the SQEs whose invisibility the poison recovery
        // proved, while a recovered flush is the only way a parked completion
        // still reaches its route. A failed flush is a health event: the
        // poison is sticky and idempotent, so a persisting failure reports
        // once per pass instead of spinning.
        // The routes behind a still-unflushable overflow keep their borrows
        // and stay pending under the failed health; their retirement proof is
        // the delivered CQE, never absence from the visible ring.
#if defined(SLUICE_B1C_MUTANT_OVERFLOW_FLUSH_IGNORED)
        (void)::io_uring_get_events(&ring_state_->ring);
        {
            std::lock_guard<std::mutex> lk(dispatch_mtx_);
            (void)reconcile_ledger_with_kernel_locked_();
        }
#else
        int flush_rc = 0;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        if (ring_state_->test_hooks.get_events != nullptr) {
            flush_rc = ring_state_->test_hooks.get_events(ring_state_->test_hooks.context,
                                                          &ring_state_->ring);
        } else
#endif
        {
            flush_rc = ::io_uring_get_events(&ring_state_->ring);
        }
        {
            std::lock_guard<std::mutex> lk(dispatch_mtx_);
            (void)reconcile_ledger_with_kernel_locked_();
            if (flush_rc < 0) {
                poison_and_recover_locked(IoError{IoError::Code::backend_error, -flush_rc});
            }
        }
#endif
        reap_visible_bounded(static_cast<std::size_t>(::io_uring_cq_ready(&ring_state_->ring)));
    }
}

#endif

}

