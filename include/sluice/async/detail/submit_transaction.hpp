#pragma once

#include <sluice/async/detail/request_arena.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <optional>
#include <utility>

namespace sluice::async::detail {

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

enum class SubmitStage { reserve, prepare, commit };
#endif

template <class Policy>
Result<SlotHandle> submit_transaction(RequestArena& arena, typename Policy::completion_type& c,
                                      const typename Policy::op_type& op, Policy& policy) noexcept {
    if (auto pre = policy.stage0_precheck(); !pre.has_value()) {
        return make_unexpected<SlotHandle>(pre.error());
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (auto inj = policy.injected_precommit_stage_failure(SubmitStage::reserve); inj.has_value()) {
        return make_unexpected<SlotHandle>(*inj);
    }
#endif

    auto rh = arena.reserve();
    if (!rh.has_value()) {
        return make_unexpected<SlotHandle>(rh.error());
    }
    SlotHandle h = rh.value();

    if (auto v = policy.validate(op); !v.has_value()) {
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(v.error());
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (auto inj = policy.injected_precommit_stage_failure(SubmitStage::prepare); inj.has_value()) {
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(*inj);
    }
#endif

    if (auto ph = arena.prepare(h, policy.kind(), policy.borrow(op)); !ph.has_value()) {
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(ph.error());
    }

    policy.write_scratch(h, op);

    if (auto bh = arena.install_publication_binding(h, &c, policy.requested_bytes(op),
                                                    policy.publish_thunk());
        !bh.has_value()) {
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(bh.error());
    }

    if (!policy.begin_binding(c)) {
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(IoError{IoError::Code::invalid_state});
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (auto inj = policy.injected_precommit_stage_failure(SubmitStage::commit); inj.has_value()) {
        policy.rollback_binding(c);
        (void)arena.rollback_reserved_or_prepared(h);
        return make_unexpected<SlotHandle>(*inj);
    }
#endif

    if (auto ch = arena.commit(h); !ch.has_value()) {
        policy.rollback_binding(c);
        (void)arena.rollback_reserved_or_prepared(h);

        (void)ch;
        return make_unexpected<SlotHandle>(IoError{IoError::Code::invalid_state});
    }

    policy.pause_before_commit_binding();

    policy.install_binding(c, &arena, h);
    policy.commit_binding(c);
    return h;
}

} // namespace sluice::async::detail
