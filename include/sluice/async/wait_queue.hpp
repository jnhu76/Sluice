#pragma once

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/mutex.hpp>
#include <sluice/async/thread_annotations.hpp>
#include <sluice/async/wait_node.hpp>

#include <cassert>

namespace sluice::async {

class Scheduler;

class WaitQueue {
  public:
    WaitQueue() noexcept = default;

    ~WaitQueue() {
        if (head_ != nullptr) {
            assert(head_ == nullptr &&
                   "WaitQueue destroyed with registered waiters (resolve them first)");
            detail::wait_queue_lifetime_fail_fast();
        }
    }

    WaitQueue(const WaitQueue&) = delete;
    WaitQueue& operator=(const WaitQueue&) = delete;
    WaitQueue(WaitQueue&&) = delete;
    WaitQueue& operator=(WaitQueue&&) = delete;

  private:
    friend class Scheduler;

    Mutex& mtx() noexcept SLUICE_RETURN_CAPABILITY(mtx_) { return mtx_; }

    bool empty_locked() const noexcept SLUICE_REQUIRES(mtx_) { return head_ == nullptr; }

    bool register_wait_locked(WaitNode& node, const WaitResume& resume = WaitResume::none())
        SLUICE_REQUIRES(mtx_) {
        if (!node.register_(this, resume))
            return false;

        node.next_ = nullptr;
        node.prev_ = tail_;
        if (tail_ != nullptr) {
            tail_->next_ = &node;
        } else {
            head_ = &node;
        }
        tail_ = &node;
        return true;
    }

    WaitNode* wake_one_locked() SLUICE_REQUIRES(mtx_) {
        if (head_ == nullptr)
            return nullptr;
        WaitNode* n = head_;
        if (n->resolve_(WaitOutcome::woken)) {
            unlink_locked(*n);
            return n;
        }

        return nullptr;
    }

    bool cancel_locked(WaitNode& node) SLUICE_REQUIRES(mtx_) {
        if (node.resolve_(WaitOutcome::cancelled)) {
            unlink_locked(node);
            return true;
        }
        return false;
    }

    bool wake_node_locked(WaitNode& node) SLUICE_REQUIRES(mtx_) {
        if (node.resolve_(WaitOutcome::woken)) {
            unlink_locked(node);
            return true;
        }
        return false;
    }

    bool expire_locked(WaitNode& node) SLUICE_REQUIRES(mtx_) {
        if (node.resolve_(WaitOutcome::expired)) {
            unlink_locked(node);
            return true;
        }
        return false;
    }

    bool contains_locked(const WaitNode& node) const noexcept SLUICE_REQUIRES(mtx_) {
        for (WaitNode* cur = head_; cur != nullptr; cur = cur->next_) {
            if (cur == &node)
                return true;
        }
        return false;
    }

    void unlink_locked(WaitNode& node) SLUICE_REQUIRES(mtx_) {
        if (node.prev_ != nullptr) {
            node.prev_->next_ = node.next_;
        } else {
            head_ = node.next_;
        }
        if (node.next_ != nullptr) {
            node.next_->prev_ = node.prev_;
        } else {
            tail_ = node.prev_;
        }
        node.next_ = nullptr;
        node.prev_ = nullptr;
        node.home_ = nullptr;
    }

    Mutex mtx_;
    WaitNode* head_ SLUICE_GUARDED_BY(mtx_){nullptr};
    WaitNode* tail_ SLUICE_GUARDED_BY(mtx_){nullptr};
};

} // namespace sluice::async
