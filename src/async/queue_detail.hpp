// sluice::async (internal) — E12-E Queue shared internals.
//
// NON-INSTALLED src/ header shared between src/async/queue_port.cpp and
// src/async/scheduler.cpp. Defines the per-Queue-wait-op context
// (QueueWaitCtx) stashed on a WaitNode via set_user() so the QueuePort
// reconciler (fast-path success on the OTHER role) can commit the resource
// to the specific winner resolved by wake_wait_one_locked.
//
// Nothing in this header is part of the installed surface; it is not visible
// to downstream TUs.
#pragma once

#include <sluice/async/detail/queue_item.hpp>  // QueueItemControl / QueueItemLease
#include <sluice/async/detail/queue_port.hpp>  // QueuePort / QueueRole

namespace sluice::async {

// Per-Queue-wait-op context. The admit caller stashes a pointer to one of
// these on its WaitNode (node.set_user) BEFORE registering. The reconciler
// reads won->user() after wake_wait_one_locked returns the winner.
struct QueueWaitCtx {
    detail::QueuePort* port;
    detail::QueueRole role;
    // Producer: the control being admitted AND the address of the caller's
    // stack lease that owns it. The reconciler moves *prod_lease whole into a
    // ring slot (location -> ring) on grant; on closed/expired the lease is
    // left in the caller's custody (prod_control stays producer_operation) and
    // the caller returns it.
    detail::QueueItemControl* prod_control;  // producer only (else nullptr)
    detail::QueueItemLease* prod_lease;      // producer only (else nullptr)
    // Consumer: the address of the caller's empty out-lease. The reconciler
    // moves a ring item into it on grant (location -> consumer_operation); on
    // closed+empty/expired it is left empty and the caller returns closed/
    // expired.
    detail::QueueItemLease* cons_out;  // consumer only (else nullptr)
};

}  // namespace sluice::async
