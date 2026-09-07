









#pragma once

#include <sluice/async/detail/queue_item.hpp>
#include <sluice/async/detail/queue_port.hpp>

namespace sluice::async {




struct QueueWaitCtx {
    detail::QueuePort* port;
    detail::QueueRole role;





    detail::QueueItemControl* prod_control;
    detail::QueueItemLease* prod_lease;




    detail::QueueItemLease* cons_out;
};

}
