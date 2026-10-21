#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <liburing.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

using namespace sluice::async;

int fake_submit_no_kernel(void*, ::io_uring* ring) noexcept {
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    *ring->sq.khead = ring->sq.sqe_tail;
    return static_cast<int>(ready);
}

struct Fixture {
    std::unique_ptr<AsyncIoContext> ctx;
    UringAsyncBackend* raw = nullptr;
    FILE* backing = nullptr;

    static Fixture create(std::size_t capacity) {
        Fixture made;
        made.backing = ::tmpfile();
        if (made.backing == nullptr)
            return made;
        std::byte content[4096];
        std::memset(content, 0x33, sizeof(content));
        if (::fwrite(content, 1, sizeof(content), made.backing) != sizeof(content))
            return Fixture{};
        ::rewind(made.backing);
        UringBackendSubmitTestHooks hooks;
        hooks.submit = fake_submit_no_kernel;
        auto owned = std::make_unique<UringAsyncBackend>(UringConfig{capacity, 8}, hooks);
        made.raw = owned.get();
        made.ctx = std::make_unique<AsyncIoContext>(detail::make_context(std::move(owned)));
        return made;
    }

    bool ok() const { return ctx != nullptr && raw != nullptr && backing != nullptr; }

    ReadOp zero_op(std::byte* buffer, std::uint64_t offset) {
        return ReadOp{NativeFileRef(::fileno(backing), sluice::FileAccess::read_only), buffer, 0,
                      offset};
    }

    ReadOp data_op(std::byte* buffer, std::size_t len, std::uint64_t offset) {
        return ReadOp{NativeFileRef(::fileno(backing), sluice::FileAccess::read_only), buffer, len,
                      offset};
    }

    WriteOp zero_write(std::byte* buffer, std::uint64_t offset) {
        return WriteOp{NativeFileRef(::fileno(backing), sluice::FileAccess::read_write), buffer, 0,
                       offset};
    }
};

// The counter is a derived summary; the authoritative scan over the delivery
// records and the externally observable sink deliveries must agree with it at
// every quiescent checkpoint.
bool counter_matches_authority(std::size_t expected, UringAsyncBackend* raw) {
    return raw->owed_count_for_test() == expected &&
           raw->owed_scan_for_test() == (expected != 0);
}

bool quiescent_and_settled(Fixture& made) {
    return counter_matches_authority(0, made.raw) && made.raw->internal_work_retired();
}

bool counter_tracks_zero_op_reads() {
    Fixture made = Fixture::create(4);
    if (!made.ok())
        return false;
    std::byte buffers[3][1];
    std::vector<Request<std::size_t>> requests;
    for (std::uint64_t i = 0; i < 3; ++i) {
        auto submitted = made.ctx->submit_read(made.zero_op(buffers[i], i));
        if (!submitted.has_value())
            return false;
        requests.push_back(std::move(submitted).value());
        if (!counter_matches_authority(i + 1, made.raw))
            return false;
    }
    const std::size_t before = made.raw->sink_deliveries();
    const std::size_t published = made.ctx->poll().value_or(0);
    if (published != 3 || made.raw->sink_deliveries() != before + 3)
        return false;
    if (!quiescent_and_settled(made))
        return false;
    for (auto& request : requests) {
        if (!request.ready())
            return false;
        const auto observed = request.take_result();
        if (!observed.result.has_value() || observed.result.value() != 0)
            return false;
    }
    // A pass over an empty delivery set must deliver nothing and leave the
    // counter untouched: repeated consumption stays a no-op.
    if (made.ctx->poll().value_or(1) != 0 || made.raw->sink_deliveries() != before + 3)
        return false;
    return quiescent_and_settled(made);
}

bool counter_ignores_data_ops() {
    Fixture made = Fixture::create(2);
    if (!made.ok())
        return false;
    std::byte buffer[4];
    auto submitted = made.ctx->submit_read(made.data_op(buffer, sizeof(buffer), 0));
    if (!submitted.has_value())
        return false;
    if (!counter_matches_authority(0, made.raw))
        return false;
    (void)made.ctx->poll();
    if (!counter_matches_authority(0, made.raw))
        return false;
    const auto identity = made.raw->live_cookie_for_offset_for_test(0);
    if (!identity.has_value())
        return false;
    made.raw->inject_cqe_for_test(*identity, static_cast<int>(sizeof(buffer)));
    (void)made.ctx->poll();
    if (!submitted.value().ready())
        return false;
    const auto observed = submitted.value().take_result();
    return observed.result.has_value() && observed.result.value() == sizeof(buffer) &&
           quiescent_and_settled(made);
}

bool zero_length_write_is_owed() {
    Fixture made = Fixture::create(2);
    if (!made.ok())
        return false;
    std::byte buffer[1];
    auto submitted = made.ctx->submit_write(made.zero_write(buffer, 0));
    if (!submitted.has_value())
        return false;
    if (!counter_matches_authority(1, made.raw))
        return false;
    if (made.ctx->poll().value_or(0) != 1)
        return false;
    if (!submitted.value().ready())
        return false;
    const auto observed = submitted.value().take_result();
    return observed.result.has_value() && observed.result.value() == 0 &&
           quiescent_and_settled(made);
}

bool full_capacity_sweeps_exactly_once() {
    constexpr std::size_t kCapacity = 8;
    Fixture made = Fixture::create(kCapacity);
    if (!made.ok())
        return false;
    std::vector<std::byte> buffers[kCapacity];
    std::vector<Request<std::size_t>> requests;
    for (std::size_t i = 0; i < kCapacity; ++i) {
        buffers[i].assign(1, std::byte{0});
        auto submitted =
            made.ctx->submit_read(made.zero_op(buffers[i].data(), static_cast<std::uint64_t>(i)));
        if (!submitted.has_value())
            return false;
        requests.push_back(std::move(submitted).value());
    }
    if (!counter_matches_authority(kCapacity, made.raw))
        return false;
    // Capacity exhaustion: the pinned slots admit no further reservation
    // until their owed events are delivered.
    std::byte extra[1];
    const auto rejected = made.ctx->submit_read(made.zero_op(extra, kCapacity));
    if (rejected.has_value() ||
        rejected.error().code != sluice::IoError::Code::would_block)
        return false;
    if (made.ctx->poll().value_or(0) != kCapacity)
        return false;
    if (made.raw->sink_deliveries() != kCapacity)
        return false;
    if (!quiescent_and_settled(made))
        return false;
    for (auto& request : requests) {
        const auto observed = request.take_result();
        if (!observed.result.has_value() || observed.result.value() != 0)
            return false;
    }
    auto recovered = made.ctx->submit_read(made.zero_op(extra, kCapacity));
    if (!recovered.has_value())
        return false;
    if (!counter_matches_authority(1, made.raw))
        return false;
    (void)made.ctx->poll();
    const auto observed = recovered.value().take_result();
    return observed.result.has_value() && quiescent_and_settled(made);
}

// Consuming a published result before its owed delivery must not free the
// slot: the owed control reference pins it, so the next submit is rejected
// until the sweep retires the obligation. On the reused slot the counter
// must track the new obligation without absorbing the old one.
bool slot_reuse_before_delivery_keeps_parity() {
    Fixture made = Fixture::create(1);
    if (!made.ok())
        return false;
    std::byte first_buffer[1];
    std::byte second_buffer[1];
    auto first = made.ctx->submit_read(made.zero_op(first_buffer, 0));
    if (!first.has_value())
        return false;
    if (!counter_matches_authority(1, made.raw))
        return false;
    const auto observed = first.value().take_result();
    if (!observed.result.has_value() || observed.result.value() != 0)
        return false;
    if (!counter_matches_authority(1, made.raw))
        return false;
    auto early = made.ctx->submit_read(made.zero_op(second_buffer, 4));
    if (early.has_value() || early.error().code != sluice::IoError::Code::would_block)
        return false;
    if (made.ctx->poll().value_or(0) != 1)
        return false;
    if (made.raw->sink_deliveries() != 1)
        return false;
    if (!quiescent_and_settled(made))
        return false;
    auto second = made.ctx->submit_read(made.zero_op(second_buffer, 4));
    if (!second.has_value())
        return false;
    if (!counter_matches_authority(1, made.raw))
        return false;
    if (made.ctx->poll().value_or(0) != 1)
        return false;
    if (made.raw->sink_deliveries() != 2)
        return false;
    const auto second_observed = second.value().take_result();
    return second_observed.result.has_value() && quiescent_and_settled(made);
}

bool cancel_leaves_owed_delivery_intact() {
    Fixture made = Fixture::create(2);
    if (!made.ok())
        return false;
    std::byte buffer[1];
    auto submitted = made.ctx->submit_read(made.zero_op(buffer, 0));
    if (!submitted.has_value())
        return false;
    if (!counter_matches_authority(1, made.raw))
        return false;
    const auto disposition = made.ctx->cancel(submitted.value().id());
    if (!disposition.has_value() ||
        disposition.value() != CancelDisposition::already_terminal)
        return false;
    if (!counter_matches_authority(1, made.raw))
        return false;
    if (made.ctx->poll().value_or(0) != 1)
        return false;
    if (made.raw->sink_deliveries() != 1)
        return false;
    const auto observed = submitted.value().take_result();
    return observed.result.has_value() && observed.result.value() == 0 &&
           quiescent_and_settled(made);
}

template <class Gate> void wait_gate_paused(Gate& gate) {
    std::atomic<bool>& paused = gate.paused;
    bool seen = paused.load(std::memory_order_acquire);
    while (!seen) {
        paused.wait(seen, std::memory_order_acquire);
        seen = paused.load(std::memory_order_acquire);
    }
}

template <class Gate> void resume_gate(Gate& gate) noexcept {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

// An owed event created while a pass sits between its sweep and its report
// must show up in the counter immediately (producer side, no round-trip
// through the pass) and in the pass's residue verdict, and the next pass
// must deliver it exactly once.
bool owed_during_pass_is_counted_and_reported() {
    Fixture made = Fixture::create(2);
    if (!made.ok())
        return false;
    UringAsyncBackend::DeliverySweepEpiloguePauseGate gate{};
    made.raw->set_delivery_sweep_epilogue_pause_gate(&gate);

    std::byte buffer[1];
    std::optional<ProgressPass> observed;
    std::thread driver([&] {
        auto claimed = made.ctx->claim_progress_owner();
        if (!claimed.has_value())
            return;
        ProgressOwner owner = std::move(claimed).value();
        const auto r = made.ctx->poll_progress();
        if (r.has_value())
            observed = r.value();
    });
    wait_gate_paused(gate);
    auto submitted = made.ctx->submit_read(made.zero_op(buffer, 0));
    const bool landed = submitted.has_value();
    Request<std::size_t> zero = landed ? std::move(submitted).value() : Request<std::size_t>{};
    const bool counted_while_paused = counter_matches_authority(1, made.raw);
    resume_gate(gate);
    driver.join();
    made.raw->set_delivery_sweep_epilogue_pause_gate(nullptr);

    if (!landed || !counted_while_paused || !observed.has_value())
        return false;
    if (observed->completed != 0 || !observed->immediate_work_remains)
        return false;
    if (made.ctx->poll_progress().value_or(ProgressPass{}).completed != 1)
        return false;
    if (made.raw->sink_deliveries() != 1)
        return false;
    const auto outcome = zero.try_result();
    zero.discard();
    return outcome.readiness == RequestReadiness::ready && outcome.result.has_value() &&
           outcome.result.value() == 0 && quiescent_and_settled(made);
}

bool concurrent_producers_keep_exactly_once() {
    Fixture made = Fixture::create(64);
    if (!made.ok())
        return false;
    constexpr std::size_t kOperations = 500;
    std::byte buffer[1];
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::atomic<std::size_t> accepted{0};
    std::thread producer([&] {
        std::size_t count = 0;
        while (count < kOperations) {
            auto submitted = made.ctx->submit_read(made.zero_op(buffer, count));
            if (!submitted.has_value()) {
                if (submitted.error().code == sluice::IoError::Code::would_block) {
                    std::this_thread::yield();
                    continue;
                }
                failed.store(true, std::memory_order_release);
                break;
            }
            const auto observed = submitted.value().take_result();
            if (!observed.result.has_value()) {
                failed.store(true, std::memory_order_release);
                break;
            }
            ++count;
            accepted.store(count, std::memory_order_release);
        }
        done.store(true, std::memory_order_release);
    });
    for (std::size_t i = 0; i < 2000000; ++i) {
        (void)made.ctx->poll();
        if (done.load(std::memory_order_acquire) &&
            made.raw->sink_deliveries() >= kOperations)
            break;
        std::this_thread::yield();
    }
    producer.join();
    if (failed.load(std::memory_order_acquire))
        return false;
    if (accepted.load(std::memory_order_acquire) != kOperations)
        return false;
    if (made.raw->sink_deliveries() != kOperations)
        return false;
    return quiescent_and_settled(made);
}

} // namespace

int main() {
    ::alarm(120);
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"counter_tracks_zero_op_reads", counter_tracks_zero_op_reads},
        {"counter_ignores_data_ops", counter_ignores_data_ops},
        {"zero_length_write_is_owed", zero_length_write_is_owed},
        {"full_capacity_sweeps_exactly_once", full_capacity_sweeps_exactly_once},
        {"slot_reuse_before_delivery_keeps_parity", slot_reuse_before_delivery_keeps_parity},
        {"cancel_leaves_owed_delivery_intact", cancel_leaves_owed_delivery_intact},
        {"owed_during_pass_is_counted_and_reported", owed_during_pass_is_counted_and_reported},
        {"concurrent_producers_keep_exactly_once", concurrent_producers_keep_exactly_once},
    };
    std::size_t passed = 0;
    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
        ++passed;
        std::printf("ok %s\n", t.name);
        std::fflush(stdout);
    }
    std::printf("all %zu uring owed-count parity tests passed\n", passed);
    return 0;
}
