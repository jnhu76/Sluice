#pragma once

namespace sluice::async::detail {

[[noreturn]] void async_context_outstanding_fail_fast() noexcept;

[[noreturn]] void async_context_settlement_fail_fast() noexcept;

[[noreturn]] void async_context_progress_binding_fail_fast() noexcept;

[[noreturn]] void context_identity_exhausted_fail_fast() noexcept;

[[noreturn]] void request_nonterminal_release_fail_fast() noexcept;

[[noreturn]] void request_binding_invariant_fail_fast() noexcept;

[[noreturn]] void request_scope_settlement_driver_fail_fast() noexcept;

[[noreturn]] void request_scope_settlement_health_fail_fast() noexcept;

[[noreturn]] void stackful_host_suspend_invariant_fail_fast() noexcept;

[[noreturn]] void stackful_host_drive_health_fail_fast() noexcept;

[[noreturn]] void stackful_host_live_task_fail_fast() noexcept;

[[noreturn]] void threadpool_non_quiescent_destruction_fail_fast() noexcept;

[[noreturn]] void threadpool_core_handoff_fail_fast() noexcept;

[[noreturn]] void uring_non_quiescent_destruction_fail_fast() noexcept;

[[noreturn]] void uring_core_handoff_fail_fast() noexcept;

}
