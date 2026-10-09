// e1 (#474): COPY-B contract oracle cases over the current
// run_pipelined_copy_with_backend API. The scripted backend, controller,
// scenario harness and the CONTRACT_DECISION_REQUIRED analysis live in
// tests/support/pipelined_copy_oracle_support.hpp.

#include "copy_task.hpp"
#include "support/pipelined_copy_oracle_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using namespace sluice_copy_oracle;
using namespace sluice_copy;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;

bool depth1_single_outstanding_read() {
    constexpr std::size_t B = 16;
    CopyScenario sc;
    check(sc.open_files(), "depth1 opens");
    sc.start(B, 1, SyncPolicy::none);

    for (int chunk = 0; chunk < 3; ++chunk) {
        const std::uint64_t rid =
            sc.ctrl->wait_completable_at(detail::OperationKind::read, chunk * B);
        check_msg(rid != 0, "depth1 read at offset " + std::to_string(chunk * B));
        check(sc.ctrl->completable_count(detail::OperationKind::read) == 1,
              "depth1 exactly one outstanding read");
        check(sc.ctrl->complete_bytes(rid, B), "depth1 read completes");
        const std::uint64_t wid =
            sc.ctrl->wait_completable_at(detail::OperationKind::write, chunk * B);
        check_msg(wid != 0, "depth1 write at offset " + std::to_string(chunk * B));
        check(sc.ctrl->complete_bytes(wid, B), "depth1 write completes");
    }
    const std::uint64_t eof_rid = sc.ctrl->wait_completable_at(detail::OperationKind::read, 3 * B);
    check(eof_rid != 0, "depth1 EOF read appears");
    check(sc.ctrl->complete_eof(eof_rid), "depth1 EOF completes");

    sc.drain_and_join();

    check(sc.copy_result.has_value() && sc.copy_result->has_value(), "depth1 succeeds");
    check(sc.copy_result->value().bytes_copied == B * 3, "depth1 byte count");
    check(sc.ctrl->peak_live(detail::OperationKind::read) == 1, "depth1 peak live reads == 1");
    check(sc.ctrl->pending_empty(), "depth1 pending empty");
    check(!sc.ctrl->borrow_overlap_violation(), "depth1 no borrow overlap");
    check(sc.ctrl->protocol_violation().empty(), "depth1 no protocol violation");
    return sc.copy_result.has_value() && sc.copy_result->has_value() &&
           sc.ctrl->peak_live(detail::OperationKind::read) == 1;
}

bool depth4_full_read_window() {
    constexpr std::size_t B = 16;
    constexpr std::size_t DEPTH = 4;
    constexpr std::uint64_t TOTAL = B * DEPTH;
    CopyScenario sc;
    check(sc.open_files(), "depth4 opens");
    sc.start(B, DEPTH, SyncPolicy::none);

    check(sc.ctrl->wait_completable(detail::OperationKind::read, DEPTH),
          "depth4 full read window outstanding");
    for (std::size_t i = 0; i < DEPTH; ++i) {
        const std::uint64_t rid =
            sc.ctrl->wait_completable_at(detail::OperationKind::read, i * B);
        check_msg(rid != 0, "depth4 read at offset " + std::to_string(i * B));
    }
    check(sc.ctrl->peak_live(detail::OperationKind::read) == DEPTH,
          "depth4 submit-time peak == depth");
    sc.ctrl->drive_to_end(TOTAL, sc.copy_published);
    sc.drain_and_join();

    check(sc.copy_result.has_value() && sc.copy_result->has_value(), "depth4 succeeds");
    check(sc.copy_result->value().bytes_copied == TOTAL, "depth4 byte count");
    check(sc.copy_result->value().write_ops == DEPTH, "depth4 write op tally");
    check(sc.ctrl->shadow_matches(TOTAL), "depth4 content pattern");
    check(sc.ctrl->pending_empty(), "depth4 pending empty");
    check(!sc.ctrl->borrow_overlap_violation(), "depth4 no borrow overlap");
    return sc.copy_result.has_value() && sc.copy_result->has_value() &&
           sc.ctrl->peak_live(detail::OperationKind::read) == DEPTH;
}

bool out_of_order_read_in_order_write() {
    constexpr std::size_t B = 16;
    constexpr std::uint64_t TOTAL = B * 4;
    CopyScenario sc;
    check(sc.open_files(), "ooo opens");
    sc.start(B, 2, SyncPolicy::none);

    check(sc.ctrl->wait_completable(detail::OperationKind::read, 2), "ooo read window");
    const std::uint64_t rB = sc.ctrl->wait_completable_at(detail::OperationKind::read, B);
    check(rB != 0, "ooo offset-B read present");
    check(sc.ctrl->complete_bytes(rB, B), "ooo offset-B read completes first");
    const std::uint64_t r0 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 0);
    check(r0 != 0, "ooo offset-0 read present");
    check(sc.ctrl->complete_bytes(r0, B), "ooo offset-0 read completes second");

    std::vector<std::uint64_t> write_order;
    bool all_writes_observed = true;
    while (write_order.size() < 4 && !sc.published()) {
        if (!sc.ctrl->wait_completable(detail::OperationKind::write, 1)) {
            all_writes_observed = false;
            break;
        }
        std::vector<ScriptedOpView> writes;
        for (const ScriptedOpView& op : sc.ctrl->completable_ops())
            if (op.kind == detail::OperationKind::write)
                writes.push_back(op);
        std::sort(writes.begin(), writes.end(),
                  [](const ScriptedOpView& a, const ScriptedOpView& b) {
                      return a.submit_id < b.submit_id;
                  });
        check_msg(writes.size() == 1,
                  "ooo single write outstanding, got " + std::to_string(writes.size()));
        for (const ScriptedOpView& w : writes) {
            write_order.push_back(w.offset);
            check(sc.ctrl->complete_bytes(w.submit_id, w.len), "ooo write completes");
        }
        for (const ScriptedOpView& op : sc.ctrl->completable_ops())
            if (op.kind == detail::OperationKind::read)
                check(sc.ctrl->complete_bytes(op.submit_id, op.offset >= TOTAL ? 0 : op.len),
                      "ooo read-ahead completes");
    }
    if (all_writes_observed)
        sc.ctrl->drive_to_end(TOTAL, sc.copy_published);
    sc.drain_and_join();

    check(sc.copy_result.has_value() && sc.copy_result->has_value(), "ooo succeeds");
    check(write_order.size() == 4, "ooo four writes recorded");
    const bool ascending = std::is_sorted(write_order.begin(), write_order.end());
    check(ascending, "ooo observed write order ascending");
    check(std::set<std::uint64_t>(write_order.begin(), write_order.end()).size() ==
              write_order.size(),
          "ooo write offsets distinct");
    check(sc.copy_result->value().bytes_copied == TOTAL, "ooo byte count");
    check(sc.ctrl->shadow_matches(TOTAL), "ooo content pattern");
    check(sc.ctrl->pending_empty(), "ooo pending empty");
    return all_writes_observed && ascending && write_order.size() == 4 &&
           sc.copy_result.has_value() && sc.copy_result->has_value();
}

bool short_read_retry_same_slot() {
    constexpr std::size_t B = 16;
    CopyScenario sc;
    check(sc.open_files(), "short-read opens");
    sc.start(B, 1, SyncPolicy::none);

    const std::uint64_t r0 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 0);
    check(r0 != 0, "short-read first read appears");
    const auto orig0 = sc.ctrl->op_view(r0);
    check(sc.ctrl->complete_bytes(r0, 4), "short-read partial completion");

    const std::uint64_t r4 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 4);
    check(r4 != 0, "short-read continuation at offset 4");
    const auto cont4 = sc.ctrl->op_view(r4);
    std::size_t continuation_len = cont4.has_value() ? cont4->len : 0;
    check(continuation_len == B - 4, "short-read continuation length 12");
    check(orig0.has_value() && cont4.has_value() && orig0->completion == cont4->completion &&
              cont4->buffer == orig0->buffer + 4,
          "short-read continuation reuses the same slot buffer and completion");
    check(sc.ctrl->complete_bytes(r4, B - 4), "short-read continuation completes");

    const std::uint64_t w0 = sc.ctrl->wait_completable_at(detail::OperationKind::write, 0);
    check(w0 != 0, "short-read write appears");
    check(sc.ctrl->complete_bytes(w0, B), "short-read write completes");

    sc.ctrl->drive_to_end(B * 2, sc.copy_published);
    sc.drain_and_join();

    check(sc.copy_result.has_value() && sc.copy_result->has_value(), "short-read succeeds");
    check(sc.copy_result->value().bytes_copied == B * 2, "short-read byte count");
    check(sc.ctrl->shadow_matches(B * 2), "short-read content pattern");
    check(sc.ctrl->pending_empty(), "short-read pending empty");
    return continuation_len == B - 4 && sc.copy_result.has_value() &&
           sc.copy_result->has_value();
}

bool partial_write_advance() {
    constexpr std::size_t B = 16;
    CopyScenario sc;
    check(sc.open_files(), "short-write opens");
    sc.start(B, 1, SyncPolicy::none);

    const std::uint64_t r0 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 0);
    check(r0 != 0, "short-write read appears");
    check(sc.ctrl->complete_bytes(r0, B), "short-write read completes");

    const std::uint64_t w0 = sc.ctrl->wait_completable_at(detail::OperationKind::write, 0);
    check(w0 != 0, "short-write write appears");
    const auto orig_w = sc.ctrl->op_view(w0);
    check(sc.ctrl->complete_bytes(w0, 7), "short-write partial completion");

    const std::uint64_t w7 = sc.ctrl->wait_completable_at(detail::OperationKind::write, 7);
    check(w7 != 0, "short-write continuation at offset 7");
    const auto cont_w = sc.ctrl->op_view(w7);
    std::size_t continuation_len = cont_w.has_value() ? cont_w->len : 0;
    check(continuation_len == B - 7, "short-write continuation length 9");
    check(orig_w.has_value() && cont_w.has_value() && orig_w->completion == cont_w->completion &&
              cont_w->buffer == orig_w->buffer + 7,
          "short-write continuation reuses the same slot buffer and completion");
    check(sc.ctrl->complete_bytes(w7, B - 7), "short-write continuation completes");

    sc.ctrl->drive_to_end(B * 2, sc.copy_published);
    sc.drain_and_join();

    check(sc.copy_result.has_value() && sc.copy_result->has_value(), "short-write succeeds");
    check(sc.copy_result->value().bytes_copied == B * 2, "short-write byte count");
    check(sc.copy_result->value().short_writes == 1, "short-write tally");
    check(sc.ctrl->shadow_matches(B * 2), "short-write content pattern");
    check(sc.ctrl->pending_empty(), "short-write pending empty");
    return continuation_len == B - 7 && sc.copy_result.has_value() &&
           sc.copy_result->has_value();
}

bool zero_progress_write_is_error() {
    constexpr std::size_t B = 16;
    CopyScenario sc;
    check(sc.open_files(), "zero-write opens");
    sc.start(B, 1, SyncPolicy::none);

    const std::uint64_t r0 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 0);
    check(r0 != 0, "zero-write read appears");
    check(sc.ctrl->complete_bytes(r0, B), "zero-write read completes");
    const std::uint64_t w0 = sc.ctrl->wait_completable_at(detail::OperationKind::write, 0);
    check(w0 != 0, "zero-write write appears");
    check(sc.ctrl->complete_bytes(w0, 0), "zero-write completes with 0 bytes");

    sc.drain_and_join();

    check(sc.copy_result.has_value() && !sc.copy_result->has_value(), "zero-write errors");
    check(sc.copy_result->error().code == IoError::Code::backend_error, "zero-write error code");
    check(sc.ctrl->pending_empty(), "zero-write pending empty");
    return sc.copy_result.has_value() && !sc.copy_result->has_value() &&
           sc.copy_result->error().code == IoError::Code::backend_error;
}

bool read_error_settles_readahead() {
    constexpr std::size_t B = 16;
    CopyScenario sc;
    check(sc.open_files(), "read-error opens");
    sc.start(B, 2, SyncPolicy::none);

    check(sc.ctrl->wait_completable(detail::OperationKind::read, 2), "read-error window");
    const std::uint64_t r0 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 0);
    check(r0 != 0, "read-error chunk0 present");
    check(sc.ctrl->complete_error(r0, IoError{IoError::Code::no_space}), "read-error staged");

    sc.drain_and_join();

    check(sc.copy_result.has_value() && !sc.copy_result->has_value(), "read-error errors");
    check(sc.copy_result->error().code == IoError::Code::no_space, "read-error primary code");
    check(sc.ctrl->accepted(detail::OperationKind::write) == 0, "read-error no writes accepted");
    check(sc.ctrl->accepted(detail::OperationKind::sync_data) == 0, "read-error no sync");
    check(sc.ctrl->pending_empty(), "read-error pending empty");
    return sc.copy_result.has_value() && !sc.copy_result->has_value() &&
           sc.copy_result->error().code == IoError::Code::no_space;
}

bool write_error_settles_readahead() {
    constexpr std::size_t B = 16;
    CopyScenario sc;
    check(sc.open_files(), "write-error opens");
    sc.start(B, 2, SyncPolicy::none);

    const std::uint64_t r0 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 0);
    check(r0 != 0, "write-error chunk0 read");
    check(sc.ctrl->complete_bytes(r0, B), "write-error chunk0 read completes");
    const std::uint64_t w0 = sc.ctrl->wait_completable_at(detail::OperationKind::write, 0);
    check(w0 != 0, "write-error write appears");
    check(sc.ctrl->complete_error(w0, IoError{IoError::Code::no_space}),
          "write-error staged on write");

    sc.drain_and_join();

    check(sc.copy_result.has_value() && !sc.copy_result->has_value(), "write-error errors");
    check(sc.copy_result->error().code == IoError::Code::no_space, "write-error primary code");
    check(sc.ctrl->pending_empty(), "write-error pending empty");
    check(sc.ctrl->accepted(detail::OperationKind::sync_all) == 0, "write-error no sync");
    return sc.copy_result.has_value() && !sc.copy_result->has_value() &&
           sc.copy_result->error().code == IoError::Code::no_space;
}

bool primary_error_is_first_in_await_order() {
    constexpr std::size_t B = 16;
    CopyScenario sc;
    check(sc.open_files(), "primary-order opens");
    sc.start(B, 3, SyncPolicy::none);

    check(sc.ctrl->wait_completable(detail::OperationKind::read, 3), "primary-order window");
    const std::uint64_t r2 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 2 * B);
    check(r2 != 0, "primary-order chunk2 read");
    check(sc.ctrl->complete_error(r2, IoError{IoError::Code::interrupted}),
          "primary-order chunk2 error staged first");
    const std::uint64_t r1 = sc.ctrl->wait_completable_at(detail::OperationKind::read, B);
    check(r1 != 0, "primary-order chunk1 read");
    check(sc.ctrl->complete_error(r1, IoError{IoError::Code::no_space}),
          "primary-order chunk1 error staged second");
    const std::uint64_t r0 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 0);
    check(r0 != 0, "primary-order chunk0 read");
    check(sc.ctrl->complete_bytes(r0, B), "primary-order chunk0 succeeds");

    const std::uint64_t w0 = sc.ctrl->wait_completable_at(detail::OperationKind::write, 0);
    check(w0 != 0, "primary-order chunk0 write appears");
    check(sc.ctrl->complete_bytes(w0, B), "primary-order chunk0 write completes");

    sc.drain_and_join();

    check(sc.copy_result.has_value() && !sc.copy_result->has_value(), "primary-order errors");
    check(sc.copy_result->error().code == IoError::Code::no_space,
          "primary = first error in await order, not staging order");
    check(sc.ctrl->pending_empty(), "primary-order pending empty");
    return sc.copy_result.has_value() && !sc.copy_result->has_value() &&
           sc.copy_result->error().code == IoError::Code::no_space;
}

bool drained_results_never_override_primary() {
    constexpr std::size_t B = 16;
    CopyScenario sc;
    check(sc.open_files(), "drain-override opens");
    sc.start(B, 2, SyncPolicy::none);

    check(sc.ctrl->wait_completable(detail::OperationKind::read, 2), "drain-override window");
    const std::uint64_t r0 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 0);
    check(r0 != 0, "drain-override chunk0 read");
    check(sc.ctrl->complete_error(r0, IoError{IoError::Code::permission_denied}),
          "drain-override primary staged");
    const std::uint64_t rB = sc.ctrl->wait_completable_at(detail::OperationKind::read, B);
    check(rB != 0, "drain-override chunk1 read");
    check(sc.ctrl->complete_error(rB, IoError{IoError::Code::interrupted}),
          "drain-override different drain-time error staged");

    sc.drain_and_join();

    check(sc.copy_result.has_value() && !sc.copy_result->has_value(), "drain-override errors");
    check(sc.copy_result->error().code == IoError::Code::permission_denied,
          "drained op result does not override primary");
    check(sc.ctrl->pending_empty(), "drain-override pending empty");
    return sc.copy_result.has_value() && !sc.copy_result->has_value() &&
           sc.copy_result->error().code == IoError::Code::permission_denied;
}

bool submit_failure_settles_accepted_work() {
    constexpr std::size_t B = 16;
    CopyScenario sc;
    check(sc.open_files(), "submit-failure opens");
    sc.start(B, 2, SyncPolicy::none);

    const std::uint64_t r0 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 0);
    check(r0 != 0, "submit-failure chunk0 read");
    sc.ctrl->fail_next_submit(detail::OperationKind::write, IoError{IoError::Code::no_space});
    check(sc.ctrl->complete_bytes(r0, B), "submit-failure chunk0 read completes");

    sc.drain_and_join();

    check(sc.copy_result.has_value() && !sc.copy_result->has_value(), "submit-failure errors");
    check(sc.copy_result->error().code == IoError::Code::no_space, "submit-failure code");
    check(sc.ctrl->accepted(detail::OperationKind::write) == 0,
          "submit-failure no write accepted");
    check(sc.ctrl->pending_empty(), "submit-failure pending empty");
    return sc.copy_result.has_value() && !sc.copy_result->has_value() &&
           sc.copy_result->error().code == IoError::Code::no_space;
}

bool capacity_refusal_settles_accepted_work() {
    constexpr std::size_t B = 16;
    constexpr std::size_t DEPTH = 4;
    constexpr std::size_t SLOTS = DEPTH - 1;
    CopyScenario sc;
    check(sc.open_files(), "capacity opens");
    sc.start(B, DEPTH, SyncPolicy::none, 1, SLOTS);

    check(sc.ctrl->wait_completable(detail::OperationKind::read, SLOTS),
          "capacity fills accepted window");
    sc.drain_and_join();

    check(sc.copy_result.has_value() && !sc.copy_result->has_value(), "capacity errors");
    check(sc.copy_result->error().code == IoError::Code::would_block, "capacity refusal code");
    check(sc.ctrl->accepted(detail::OperationKind::read) == SLOTS, "capacity accepted count");
    check(sc.ctrl->pending_empty(), "capacity pending empty");
    return sc.copy_result.has_value() && !sc.copy_result->has_value() &&
           sc.copy_result->error().code == IoError::Code::would_block;
}

bool eof_drains_readahead_without_post_eof_write() {
    constexpr std::size_t B = 16;
    CopyScenario sc;
    check(sc.open_files(), "eof opens");
    sc.start(B, 2, SyncPolicy::none);

    const std::uint64_t r0 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 0);
    check(r0 != 0, "eof chunk0 read");
    check(sc.ctrl->complete_bytes(r0, B), "eof chunk0 read completes");
    const std::uint64_t rB = sc.ctrl->wait_completable_at(detail::OperationKind::read, B);
    check(rB != 0, "eof read-ahead submitted");
    check(sc.ctrl->complete_eof(rB), "eof read-ahead completes at EOF");
    const std::uint64_t w0 = sc.ctrl->wait_completable_at(detail::OperationKind::write, 0);
    check(w0 != 0, "eof chunk0 write appears");
    check(sc.ctrl->complete_bytes(w0, B), "eof chunk0 write completes");

    sc.drain_and_join();

    check(sc.copy_result.has_value() && sc.copy_result->has_value(), "eof succeeds");
    check(sc.copy_result->value().bytes_copied == B, "eof byte count");
    check(sc.ctrl->accepted(detail::OperationKind::write) == 1, "eof no post-EOF write");
    check(sc.ctrl->pending_empty(), "eof pending empty");
    return sc.copy_result.has_value() && sc.copy_result->has_value() &&
           sc.ctrl->accepted(detail::OperationKind::write) == 1;
}

bool sync_only_after_all_writes() {
    constexpr std::size_t B = 16;
    constexpr std::uint64_t TOTAL = B * 4;
    CopyScenario sc;
    check(sc.open_files(), "sync opens");
    sc.start(B, 2, SyncPolicy::data);

    std::uint64_t max_write_submit = 0;
    std::uint64_t sync_submit = 0;
    bool sync_seen = false;
    const auto deadline = std::chrono::steady_clock::now() + kDriveBound;
    while (!sc.published() && std::chrono::steady_clock::now() < deadline) {
        bool staged_any = false;
        for (const ScriptedOpView& op : sc.ctrl->completable_ops()) {
            if (op.kind == detail::OperationKind::read) {
                staged_any = sc.ctrl->complete_bytes(op.submit_id,
                                                     op.offset >= TOTAL ? 0 : op.len) ||
                             staged_any;
            } else if (op.kind == detail::OperationKind::write) {
                max_write_submit = std::max(max_write_submit, op.submit_id);
                staged_any = sc.ctrl->complete_bytes(op.submit_id, op.len) || staged_any;
            } else {
                if (!sync_seen) {
                    sync_seen = true;
                    sync_submit = op.submit_id;
                }
                staged_any = sc.ctrl->complete_sync(op.submit_id) || staged_any;
            }
        }
        if (!staged_any)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sc.drain_and_join();

    check(sync_seen, "sync op submitted");
    check(sync_submit > max_write_submit, "sync submitted after every write");
    check(sc.copy_result.has_value() && sc.copy_result->has_value(), "sync succeeds");
    check(sc.copy_result->value().bytes_copied == TOTAL, "sync byte count");
    check(sc.ctrl->accepted(detail::OperationKind::write) == TOTAL / B, "sync write count");
    check(sc.ctrl->shadow_matches(TOTAL), "sync content pattern");
    check(sc.ctrl->pending_empty(), "sync pending empty");
    return sync_seen && sync_submit > max_write_submit && sc.copy_result.has_value() &&
           sc.copy_result->has_value();
}

bool bounded_reuse_multiround() {
    constexpr std::size_t B = 16;
    constexpr std::size_t DEPTH = 3;
    constexpr std::size_t CHUNKS = DEPTH * 3 + 1;
    constexpr std::uint64_t TOTAL = B * CHUNKS;
    CopyScenario sc;
    check(sc.open_files(), "multiround opens");
    sc.start(B, DEPTH, SyncPolicy::none);

    std::set<void*> slot_ids;
    std::set<std::byte*> buffer_bases;
    std::map<void*, std::set<std::uint64_t>> slot_offsets;
    std::vector<std::uint64_t> write_offsets;

    const auto deadline = std::chrono::steady_clock::now() + kDriveBound;
    while (!sc.published() && std::chrono::steady_clock::now() < deadline) {
        bool staged_any = false;
        for (const ScriptedOpView& op : sc.ctrl->completable_ops()) {
            if (op.kind == detail::OperationKind::read) {
                slot_ids.insert(op.completion);
                buffer_bases.insert(op.buffer - static_cast<std::ptrdiff_t>(op.offset % B));
                slot_offsets[op.completion].insert(op.offset);
                staged_any = sc.ctrl->complete_bytes(op.submit_id,
                                                     op.offset >= TOTAL ? 0 : op.len) ||
                             staged_any;
            } else if (op.kind == detail::OperationKind::write) {
                write_offsets.push_back(op.offset);
                staged_any = sc.ctrl->complete_bytes(op.submit_id, op.len) || staged_any;
            }
        }
        if (!staged_any)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sc.drain_and_join();

    check(sc.copy_result.has_value() && sc.copy_result->has_value(), "multiround succeeds");
    check(sc.copy_result->value().bytes_copied == TOTAL, "multiround byte count");
    check(sc.copy_result->value().write_ops == CHUNKS, "multiround write count");
    check(std::is_sorted(write_offsets.begin(), write_offsets.end()),
          "multiround write order ascending");
    check_msg(buffer_bases.size() == DEPTH, "multiround distinct buffer bases == depth");
    check_msg(slot_ids.size() == DEPTH, "multiround distinct slot identities == depth");
    check(sc.ctrl->peak_live(detail::OperationKind::read) == DEPTH,
          "multiround peak outstanding reads == depth");
    bool stride_ok = slot_offsets.size() == DEPTH;
    for (const auto& [id, offsets] : slot_offsets)
        if (offsets.size() < 3)
            stride_ok = false;
    check(stride_ok, "multiround every slot reused >= 3 chunks");
    check(sc.ctrl->shadow_matches(TOTAL), "multiround content pattern");
    check(!sc.ctrl->borrow_overlap_violation(), "multiround no borrow overlap");
    check(sc.ctrl->pending_empty(), "multiround pending empty");
    return buffer_bases.size() == DEPTH && slot_ids.size() == DEPTH && stride_ok &&
           sc.copy_result.has_value() && sc.copy_result->has_value();
}

bool slot_not_recycled_before_write_completes() {
    constexpr std::size_t B = 16;
    CopyScenario sc;
    check(sc.open_files(), "recycle opens");
    sc.start(B, 1, SyncPolicy::none);

    const std::uint64_t r0 = sc.ctrl->wait_completable_at(detail::OperationKind::read, 0);
    check(r0 != 0, "recycle first read");
    check(sc.ctrl->complete_bytes(r0, B), "recycle first read completes");
    const std::uint64_t w0 = sc.ctrl->wait_completable_at(detail::OperationKind::write, 0);
    check(w0 != 0, "recycle write outstanding");

    bool premature_read = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const ScriptedOpView& op : sc.ctrl->completable_ops())
            if (op.kind == detail::OperationKind::read && op.offset >= B)
                premature_read = true;
        if (premature_read)
            break;
        sc.ctrl->wait_completable(detail::OperationKind::read, 1,
                                  std::chrono::milliseconds(10));
    }
    check(!premature_read, "no read into slot buffer before its write completes");

    check(sc.ctrl->complete_bytes(w0, B), "recycle write completes");
    const std::uint64_t r1 = sc.ctrl->wait_completable_at(detail::OperationKind::read, B);
    check(r1 != 0, "recycle next read after write completion");

    sc.ctrl->drive_to_end(B * 3, sc.copy_published);
    sc.drain_and_join();

    check(sc.copy_result.has_value() && sc.copy_result->has_value(), "recycle succeeds");
    check(!sc.ctrl->borrow_overlap_violation(), "recycle no borrow overlap");
    check(sc.ctrl->pending_empty(), "recycle pending empty");
    return !premature_read;
}

bool throwing_backend_submit_surfaces_error() {
    CopyScenario sc;
    check(sc.open_files(), "throwing opens");
    if (!sc.src.has_value() || !sc.dst.has_value())
        return false;
    sc.copy_thread = std::thread([&sc]() {
        auto r = run_pipelined_copy_with_backend(*sc.src, NativeFileRef{*sc.dst}, 16, 2, 1,
                                                 SyncPolicy::none,
                                                 std::make_unique<ThrowingBackend>());
        sc.copy_result = std::move(r);
        sc.copy_published.store(true, std::memory_order_release);
    });
    sc.drain_and_join_for(std::chrono::seconds(10));
    check(sc.copy_result.has_value() && !sc.copy_result->has_value(), "throwing errors");
    check(sc.copy_result->error().code == IoError::Code::backend_error, "throwing error code");
    return sc.copy_result.has_value() && !sc.copy_result->has_value();
}

bool invalid_arguments_rejected_synchronously() {
    const TempFile src("/tmp/sluice_copy_oracle_src_XXXXXX");
    const TempFile dst("/tmp/sluice_copy_oracle_dst_XXXXXX");
    if (src.path.empty() || dst.path.empty())
        return false;
    auto src_open = File::open(src.path);
    FileOpen write_mode;
    write_mode.access = FileAccess::write_only;
    auto dst_open = File::open(dst.path, write_mode);
    if (!src_open.has_value() || !dst_open.has_value())
        return false;
    File src_file = std::move(src_open).value();
    File dst_file = std::move(dst_open).value();
    const NativeFileRef dst_ref{dst_file};

    struct Case {
        const char* name;
        std::size_t buffer;
        std::size_t depth;
        unsigned workers;
        bool null_backend;
    };
    const Case cases[] = {
        {"depth zero", 16, 0, 1, false},
        {"buffer zero", 0, 2, 1, false},
        {"workers zero", 16, 2, 0, false},
        {"null backend", 16, 2, 1, true},
        {"buffer cap", kMaxBufferSize + 1, 1, 1, false},
        {"depth cap", 16, kMaxPipelineDepth + 1, 1, false},
        {"workers cap", 16, 2, kMaxWorkers + 1, false},
    };
    bool all_ok = true;
    for (const Case& c : cases) {
        auto shared = std::make_shared<ScriptedShared>();
        auto backend = c.null_backend ? nullptr : std::make_unique<ScriptedBackend>(shared, 64);
        auto r = run_pipelined_copy_with_backend(src_file, dst_ref, c.buffer, c.depth, c.workers,
                                                 SyncPolicy::none, std::move(backend));
        const bool ok = !r.has_value() && r.error().code == IoError::Code::invalid_state;
        check_msg(ok, std::string("invalid args: ") + c.name);
        all_ok = all_ok && ok;
    }
    {
        auto shared = std::make_shared<ScriptedShared>();
        constexpr std::size_t huge = static_cast<std::size_t>(-1) / 2 + 1;
        auto r = run_pipelined_copy_with_backend(src_file, dst_ref, huge, 2, 1, SyncPolicy::none,
                                                 std::make_unique<ScriptedBackend>(shared, 64));
        const bool ok = !r.has_value() && r.error().code == IoError::Code::invalid_state;
        check(ok, "invalid args: product overflow");
        all_ok = all_ok && ok;
    }
    {
        auto shared = std::make_shared<ScriptedShared>();
        constexpr std::size_t kB32 = 32 * 1024 * 1024;
        auto r = run_pipelined_copy_with_backend(src_file, dst_ref, kB32, 32, 1, SyncPolicy::none,
                                                 std::make_unique<ScriptedBackend>(shared, 64));
        const bool ok = !r.has_value() && r.error().code == IoError::Code::invalid_state;
        check(ok, "invalid args: total byte cap");
        all_ok = all_ok && ok;
    }
    return all_ok;
}

bool watchdog_selftest_child() {
    if (std::getenv("SLUICE_ORACLE_CHILD") == nullptr)
        return true;
    std::atomic<bool> gate{false};
    CopyScenario sc;
    check(sc.open_files(), "watchdog child opens");
    sc.copy_thread = std::thread([&gate]() {
        while (!gate.load(std::memory_order_acquire))
            std::this_thread::yield();
    });
    sc.drain_and_join_for(std::chrono::seconds(2));
    return false;
}

bool watchdog_selftest_bounded() {
    int pipefd[2];
    if (::pipe(pipefd) != 0)
        return false;
    const pid_t pid = ::fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        ::close(pipefd[0]);
        if (::dup2(pipefd[1], STDERR_FILENO) < 0)
            std::_Exit(88);
        ::close(pipefd[1]);
        ::setenv("SLUICE_ORACLE_FILTER", "watchdog_selftest_child", 1);
        ::setenv("SLUICE_ORACLE_CHILD", "1", 1);
        char self[4096];
        const ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n <= 0)
            std::_Exit(88);
        self[n] = '\0';
        ::execl(self, self, static_cast<char*>(nullptr));
        std::_Exit(88);
    }
    ::close(pipefd[1]);
    ::fcntl(pipefd[0], F_SETFL, ::fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);

    std::string captured;
    char buf[4096];
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        for (;;) {
            const ssize_t r = ::read(pipefd[0], buf, sizeof(buf));
            if (r > 0)
                captured.append(buf, static_cast<std::size_t>(r));
            else
                break;
        }
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (;;) {
        const ssize_t r = ::read(pipefd[0], buf, sizeof(buf));
        if (r > 0)
            captured.append(buf, static_cast<std::size_t>(r));
        else
            break;
    }
    ::close(pipefd[0]);
    if (!exited) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        check(false, "watchdog child still alive after deadline");
        return false;
    }
    const bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    check(aborted, "watchdog child aborted");
    check(captured.find("[watchdog]") != std::string::npos, "watchdog diagnostics present");
    return aborted && captured.find("[watchdog]") != std::string::npos;
}

}  // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"depth1_single_outstanding_read", depth1_single_outstanding_read},
        {"depth4_full_read_window", depth4_full_read_window},
        {"out_of_order_read_in_order_write", out_of_order_read_in_order_write},
        {"short_read_retry_same_slot", short_read_retry_same_slot},
        {"partial_write_advance", partial_write_advance},
        {"zero_progress_write_is_error", zero_progress_write_is_error},
        {"read_error_settles_readahead", read_error_settles_readahead},
        {"write_error_settles_readahead", write_error_settles_readahead},
        {"primary_error_is_first_in_await_order", primary_error_is_first_in_await_order},
        {"drained_results_never_override_primary", drained_results_never_override_primary},
        {"submit_failure_settles_accepted_work", submit_failure_settles_accepted_work},
        {"capacity_refusal_settles_accepted_work", capacity_refusal_settles_accepted_work},
        {"eof_drains_readahead_without_post_eof_write",
         eof_drains_readahead_without_post_eof_write},
        {"sync_only_after_all_writes", sync_only_after_all_writes},
        {"bounded_reuse_multiround", bounded_reuse_multiround},
        {"slot_not_recycled_before_write_completes", slot_not_recycled_before_write_completes},
        {"throwing_backend_submit_surfaces_error", throwing_backend_submit_surfaces_error},
        {"invalid_arguments_rejected_synchronously", invalid_arguments_rejected_synchronously},
        {"watchdog_selftest_child", watchdog_selftest_child},
        {"watchdog_selftest_bounded", watchdog_selftest_bounded},
    };

    const char* filter = std::getenv("SLUICE_ORACLE_FILTER");
    int failed = 0;
    for (const NamedTest& t : tests) {
        if (filter != nullptr && std::strcmp(filter, t.name) != 0)
            continue;
        const int failures_before = g_checks_failed;
        if (!t.fn() || g_checks_failed != failures_before) {
            std::fprintf(stderr, "CASE FAIL: %s\n", t.name);
            ++failed;
        } else {
            std::printf("ok: %s\n", t.name);
        }
    }
    if (failed == 0) {
        std::printf("all %zu app copy pipeline oracle tests passed\n",
                    sizeof(tests) / sizeof(tests[0]));
        return 0;
    }
    return 1;
}
