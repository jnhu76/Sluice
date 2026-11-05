// Mechanism smoke for the #595 R4 arms: correctness across every arm, plus a
// state-trajectory exposure check proving the state-aware budget changes its
// quantum with pipeline state while A0/B/R presentations stay fixed. Not a
// performance harness; it never times anything.

#include "bench_common.hpp"
#include "r3_ready_batch.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileOpen;
using sluice::IoError;
using sluice::async::AsyncIoContext;
using sluice::async::NativeFileRef;
using sluice::async::ReadOp;
using sluice::async::Request;
using sluice::async::RequestReadiness;
using sluice::async::ShutdownPolicy;

constexpr std::size_t kBlock = 4096;
constexpr std::size_t kDepth = 16;
constexpr std::size_t kFixtureBlocks = 2048;

int g_failed = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  PASS %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("  FAIL %s\n", what.c_str());
    }
}

std::string describe(const IoError& e) {
    return std::string(sluice::to_string(e.code)) + "(errno=" + std::to_string(e.os_errno) +
           ")";
}

FileOpen rw_open_existing() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::open_existing;
    return mode;
}

bool write_fixture(const std::string& path) {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::create_if_missing;
    mode.contents = sluice::FileInitialContents::truncate;
    auto opened = File::open(path, mode);
    if (!opened.has_value()) {
        return false;
    }
    File file = std::move(opened).value();
    std::vector<std::byte> buffer(kFixtureBlocks * kBlock);
    bench::fill_pattern(buffer.data(), buffer.size(), 0);
    auto res = sluice::blocking::write_all_at(
        file, 0, std::span<const std::byte>(buffer.data(), buffer.size()));
    if (!res.has_value() || !res.value().complete()) {
        return false;
    }
    return sluice::blocking::sync_data(file).has_value();
}

std::optional<AsyncIoContext> make_context() {
    sluice::async::UringConfig config;
    config.request_capacity = kDepth;
    config.queue_depth = kDepth;
    auto made = sluice::async::make_uring_context(config);
    if (!made.has_value()) {
        return std::nullopt;
    }
    return std::move(made).value();
}

struct Slots {
    std::vector<std::vector<std::byte>> buffers;
    std::vector<std::size_t> free_slots;

    explicit Slots(std::size_t depth) : buffers(depth, std::vector<std::byte>(kBlock)) {
        for (std::size_t i = 0; i < depth; ++i) {
            free_slots.push_back(i);
        }
        for (std::size_t i = 0; i < depth; ++i) {
            bench::fill_pattern(buffers[i].data(), kBlock, 0);
        }
    }
};

struct Batch {
    std::vector<Request<std::size_t>> requests;
    std::vector<std::size_t> slots;
    std::vector<std::uint64_t> offsets;
};

std::vector<ReadOp> take_ops(File& file, Slots& slots, std::size_t base, std::size_t count,
                             Batch& batch) {
    std::vector<ReadOp> ops;
    ops.reserve(count);
    batch.slots.assign(slots.free_slots.end() - count, slots.free_slots.end());
    slots.free_slots.resize(slots.free_slots.size() - count);
    batch.offsets.clear();
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t sequence = base + k;
        ReadOp op;
        op.file = NativeFileRef(file);
        op.dst = slots.buffers[batch.slots[k]].data();
        op.len = kBlock;
        op.offset = static_cast<std::uint64_t>(sequence % kFixtureBlocks) * kBlock;
        batch.offsets.push_back(op.offset);
        ops.push_back(op);
    }
    return ops;
}

// Drives passes until every request publishes, takes and verifies results,
// and refreshes the local pass-fact snapshot exactly like the W03 driver.
bool settle(AsyncIoContext& ctx, Slots& slots, Batch& batch, bench::r3::DecisionFacts& facts) {
    for (std::size_t guard = 0; guard < 2000000; ++guard) {
        bool all_ready = true;
        for (const Request<std::size_t>& request : batch.requests) {
            if (!request.ready()) {
                all_ready = false;
                break;
            }
        }
        if (all_ready) {
            break;
        }
        auto pass = ctx.poll_progress();
        if (!pass.has_value()) {
            std::printf("  FAIL poll_progress rejected: %s\n", describe(pass.error()).c_str());
            return false;
        }
        facts.last_pass_completed = pass.value().completed;
        facts.last_pass_immediate_work = pass.value().immediate_work_remains;
        facts.pressure_known = true;
    }
    for (std::size_t i = 0; i < batch.requests.size(); ++i) {
        const auto observed = batch.requests[i].take_result();
        if (observed.readiness != RequestReadiness::ready || !observed.result.has_value() ||
            observed.result.value() != kBlock) {
            std::printf("  FAIL settled request %zu was not a successful full read\n", i);
            return false;
        }
        const auto mismatch = bench::verify_pattern(slots.buffers[batch.slots[i]].data(), kBlock,
                                                    batch.offsets[i]);
        if (!mismatch.ok) {
            std::printf("  FAIL byte mismatch on request %zu\n", i);
            return false;
        }
        slots.free_slots.push_back(batch.slots[i]);
    }
    batch.requests.clear();
    batch.slots.clear();
    batch.offsets.clear();
    return true;
}

bool submit_batch(bench::r3::Adapter& adapter, File& file, Slots& slots, std::size_t base,
                  std::size_t count, Batch& batch, const bench::r3::DecisionFacts& facts,
                  const char* label) {
    const std::vector<ReadOp> ops = take_ops(file, slots, base, count, batch);
    batch.requests.clear();
    auto outcome =
        adapter.submit(std::span<const ReadOp>(ops.data(), ops.size()), batch.requests, facts);
    if (outcome.error.has_value() || batch.requests.size() != count) {
        std::printf("  FAIL %s disposed %zu of %zu reads (%s)\n", label, batch.requests.size(),
                    count, outcome.error.has_value() ? describe(*outcome.error).c_str() : "no error");
        return false;
    }
    return true;
}

bool correctness_arm(bench::r3::Arm arm, File& file) {
    auto context = make_context();
    if (!context.has_value()) {
        std::printf("  FAIL uring context unavailable\n");
        return false;
    }
    AsyncIoContext ctx = std::move(*context);
    auto owner = ctx.claim_progress_owner();
    if (!owner.has_value()) {
        std::printf("  FAIL claim_progress_owner rejected\n");
        return false;
    }
    bench::r3::Adapter adapter(ctx, arm);
    Slots slots(kDepth);
    bench::r3::DecisionFacts facts;
    bool ok = true;
    for (std::size_t round = 0; round < 3 && ok; ++round) {
        Batch batch;
        if (!submit_batch(adapter, file, slots, round * kDepth, kDepth, batch, facts,
                          (std::string("arm ") + bench::r3::arm_name(arm)).c_str())) {
            ok = false;
            break;
        }
        ok = settle(ctx, slots, batch, facts);
    }
    ctx.request_stop(ShutdownPolicy::drain);
    ctx.detach_progress_host();
    const auto shutdown = ctx.shutdown(ShutdownPolicy::drain);
    if (!shutdown.has_value()) {
        std::printf("  FAIL arm %s shutdown: %s\n", bench::r3::arm_name(arm),
                    describe(shutdown.error()).c_str());
        ok = false;
    }
    return ok;
}

} // namespace

int main() {
    const std::string dir = "/tmp/sluice-r4-arm-smoke";
    const std::string path = dir + "/fixture.bin";
    bench::ensure_dir(dir);
    if (!write_fixture(path)) {
        std::fprintf(stderr, "fixture creation failed\n");
        return 1;
    }
    auto opened = File::open(path, rw_open_existing());
    if (!opened.has_value()) {
        std::fprintf(stderr, "fixture reopen failed\n");
        return 1;
    }
    File file = std::move(opened).value();

    std::printf("=== correctness smoke: 3x16 verified reads per arm ===\n");
    const std::array<bench::r3::Arm, 9> arms = {
        bench::r3::Arm::a0,     bench::r3::Arm::b,              bench::r3::Arm::r,
        bench::r3::Arm::s,      bench::r3::Arm::s_no_inflight,  bench::r3::Arm::s_no_pressure,
        bench::r3::Arm::q2,     bench::r3::Arm::q8,             bench::r3::Arm::shadow};
    for (const bench::r3::Arm arm : arms) {
        std::printf(" arm=%s\n", bench::r3::arm_name(arm));
        check(correctness_arm(arm, file),
              std::string("correctness ") + bench::r3::arm_name(arm));
    }

    std::printf("=== perception exposure: quantum must track pipeline state ===\n");
    {
        auto context = make_context();
        if (!context.has_value()) {
            std::printf("  FAIL uring context unavailable\n");
            ++g_failed;
        } else {
            AsyncIoContext ctx = std::move(*context);
            auto owner = ctx.claim_progress_owner();
            check(owner.has_value(), "owner claim for perception run");
            Slots slots(kDepth);
            bench::r3::DecisionFacts facts;

            bench::r3::Adapter s_adapter(ctx, bench::r3::Arm::s);
            bench::r3::Adapter::TraceSink trace;
            s_adapter.set_trace_sink(&trace);

            Batch first;
            check(submit_batch(s_adapter, file, slots, 0, kDepth, first, facts, "S first refill"),
                  "S first refill accepts all 16 through the shared entry");
            check(trace.size() == 3, "S first refill forms three state-tracked decisions");
            if (trace.size() == 3) {
                check(trace[0].quantum == 2 && trace[0].present_len == 2 &&
                          trace[0].active_read && trace[0].active_data == 0 &&
                          !trace[0].pressure && !trace[0].cap_cause && !trace[0].chunk_cause &&
                          std::string(trace[0].action_row) == "inflight_zero",
                      "empty pipeline decision: inflight_zero row, quantum 2, presents 2");
                check(trace[1].quantum == 8 && trace[1].present_len == 8 &&
                          trace[1].active_read && trace[1].active_data == 2 &&
                          trace[1].chunk_cause &&
                          std::string(trace[1].action_row) == "quantum_base",
                      "same refill after progress: active data raises quantum to 8");
                check(trace[2].quantum == 8 && trace[2].present_len == 6 &&
                          trace[2].active_read && trace[2].active_data == 10 &&
                          trace[2].chunk_cause,
                      "tail decision keeps quantum 8 and presents the remainder");
            }
            check(settle(ctx, slots, first, facts), "first refill settles verified");
            check(facts.pressure_known && facts.last_pass_completed > 0,
                  "settle passes publish completion-pressure facts");

            bench::r3::Adapter q8_adapter(ctx, bench::r3::Arm::q8);
            Batch inflight;
            check(submit_batch(q8_adapter, file, slots, 16, 2, inflight, facts,
                               "hint-free outstanding probe"),
                  "two reads left outstanding via the hint-free arm");
            check(ctx.outstanding() == 2, "authority fact check: outstanding()==2");

            trace.records.clear();
            Batch pressured;
            check(submit_batch(s_adapter, file, slots, 18, 4, pressured, facts,
                               "S pressure probe"),
                  "S accepts all 4 with a completion-backlog snapshot and 2 outstanding");
            if (trace.size() == 2) {
                check(trace[0].quantum == 2 && trace[0].present_len == 2 &&
                          trace[0].active_read && trace[0].active_data == 2 &&
                          trace[0].pressure && trace[0].pressure_known &&
                          trace[0].last_pass_completed > 0 &&
                          std::string(trace[0].action_row) == "completion_pressure",
                      "completion backlog with active data: pressure clamps quantum to 2");
            } else {
                check(false, "S pressure probe forms two decisions");
            }
            bench::r3::Adapter snp_adapter(ctx, bench::r3::Arm::s_no_pressure);
            bench::r3::Adapter::TraceSink snp_trace;
            snp_adapter.set_trace_sink(&snp_trace);
            Batch snp_batch;
            check(submit_batch(snp_adapter, file, slots, 22, 4, snp_batch, facts, "SNP probe"),
                  "SNP accepts all 4 with the same state");
            if (snp_trace.size() == 1) {
                check(snp_trace[0].quantum == 8 && snp_trace[0].present_len == 4 &&
                          snp_trace[0].active_read && snp_trace[0].active_data == 6 &&
                          !snp_trace[0].pressure_read &&
                          std::string(snp_trace[0].action_row) == "quantum_base",
                      "ablation SNP: pressure hint ignored, quantum stays 8 at the same state");
            } else {
                check(false, "SNP probe forms a single decision");
            }
            check(settle(ctx, slots, pressured, facts), "pressure probe settles verified");
            check(settle(ctx, slots, snp_batch, facts), "SNP probe settles verified");
            check(settle(ctx, slots, inflight, facts),
                  "outstanding probe reads settle verified");

            bench::r3::Adapter b_adapter(ctx, bench::r3::Arm::b);
            bench::r3::Adapter::TraceSink b_trace;
            b_adapter.set_trace_sink(&b_trace);
            Batch b_pressured;
            check(submit_batch(b_adapter, file, slots, 28, kDepth, b_pressured, facts,
                               "B state probe"),
                  "B accepts all 16 at the pressured state");
            check(b_trace.size() == 1 &&
                      b_trace[0].present_len == kDepth && !b_trace[0].active_read &&
                      !b_trace[0].pressure_read,
                  "B presentation ignores state entirely (one full-span decision, no hint reads)");
            check(settle(ctx, slots, b_pressured, facts), "B state probe settles verified");

            bench::r3::Adapter r_adapter(ctx, bench::r3::Arm::r);
            bench::r3::Adapter::TraceSink r_trace;
            r_adapter.set_trace_sink(&r_trace);
            Batch r_pressured;
            check(submit_batch(r_adapter, file, slots, 44, kDepth, r_pressured, facts,
                               "R state probe"),
                  "R accepts all 16 at the pressured state");
            check(r_trace.size() == 2 && r_trace[0].present_len == kDepth &&
                      r_trace[1].present_len == 8 && !r_trace[1].active_read &&
                      r_trace[1].cap_cause,
                  "R presentation ignores state: full span, cap stop, 8-op suffix reentry");
            check(settle(ctx, slots, r_pressured, facts), "R state probe settles verified");

            ctx.request_stop(ShutdownPolicy::drain);
            ctx.detach_progress_host();
            const auto shutdown = ctx.shutdown(ShutdownPolicy::drain);
            check(shutdown.has_value(), "perception run shutdown");
        }
    }

    std::printf("=== ablation and shadow exposure on a fresh pipeline ===\n");
    {
        auto context = make_context();
        if (!context.has_value()) {
            std::printf("  FAIL uring context unavailable\n");
            ++g_failed;
        } else {
            AsyncIoContext ctx = std::move(*context);
            auto owner = ctx.claim_progress_owner();
            check(owner.has_value(), "owner claim for ablation run");
            Slots slots(kDepth);
            bench::r3::DecisionFacts facts;

            bench::r3::Adapter sni_adapter(ctx, bench::r3::Arm::s_no_inflight);
            bench::r3::Adapter::TraceSink sni_trace;
            sni_adapter.set_trace_sink(&sni_trace);
            Batch sni_batch;
            check(submit_batch(sni_adapter, file, slots, 0, 8, sni_batch, facts, "SNI probe"),
                  "SNI accepts all 8 on the empty pipeline");
            if (sni_trace.size() == 1) {
                check(sni_trace[0].quantum == 8 && sni_trace[0].present_len == 8 &&
                          !sni_trace[0].active_read &&
                          std::string(sni_trace[0].action_row) == "quantum_base",
                      "ablation SNI: inflight hint ignored, empty pipeline stays quantum 8");
            } else {
                check(false, "SNI probe forms a single decision");
            }
            check(settle(ctx, slots, sni_batch, facts), "SNI probe settles verified");

            bench::r3::Adapter q2_adapter(ctx, bench::r3::Arm::q2);
            bench::r3::Adapter::TraceSink q2_trace;
            q2_adapter.set_trace_sink(&q2_trace);
            Batch q2_batch;
            check(submit_batch(q2_adapter, file, slots, 8, 4, q2_batch, facts, "Q2 probe"),
                  "Q2 accepts all 4");
            bool q2_all_two = q2_trace.size() == 2;
            for (const bench::r3::DecisionRecord& record : q2_trace) {
                q2_all_two = q2_all_two && record.quantum == 2 && record.present_len == 2 &&
                             !record.active_read && !record.pressure_read;
            }
            check(q2_all_two, "fixed Q2: pinned quantum 2 with no hint reads");
            check(settle(ctx, slots, q2_batch, facts), "Q2 probe settles verified");

            bench::r3::Adapter shadow_adapter(ctx, bench::r3::Arm::shadow);
            bench::r3::Adapter::TraceSink shadow_trace;
            shadow_adapter.set_trace_sink(&shadow_trace);
            Batch shadow_batch;
            bench::r3::DecisionFacts shadow_facts;
            shadow_facts.caller_window = 5;
            check(submit_batch(shadow_adapter, file, slots, 12, kDepth, shadow_batch,
                               shadow_facts, "SHADOW probe"),
                  "SHADOW accepts all 16 through the R-shaped path");
            if (shadow_trace.size() == 2) {
                check(shadow_trace[0].quantum == 2 && shadow_trace[0].present_len == kDepth &&
                          shadow_trace[0].active_read && shadow_trace[0].active_data == 0 &&
                          shadow_trace[0].pressure_read && shadow_trace[0].caller_window == 5,
                      "shadow: hints read and logged, presentation stays the full span");
                check(shadow_trace[1].cap_cause && shadow_trace[1].present_len == 8,
                      "shadow: cap reentry follows the R action");
            } else {
                check(false, "SHADOW probe forms two decisions");
            }
            check(settle(ctx, slots, shadow_batch, facts), "SHADOW probe settles verified");

            ctx.request_stop(ShutdownPolicy::drain);
            ctx.detach_progress_host();
            const auto shutdown = ctx.shutdown(ShutdownPolicy::drain);
            check(shutdown.has_value(), "ablation run shutdown");
        }
    }

    std::printf("=== mechanism counters on one 16-op refill (empty SQ, depth 16) ===\n");
    {
        auto context = make_context();
        if (!context.has_value()) {
            std::printf("  FAIL uring context unavailable\n");
            ++g_failed;
        } else {
            AsyncIoContext ctx = std::move(*context);
            auto owner = ctx.claim_progress_owner();
            check(owner.has_value(), "owner claim for counter run");
            const struct Expected {
                bench::r3::Arm arm;
                std::size_t presentations;
                std::size_t suffix_singles;
                std::size_t cap_reentries;
                std::size_t chunk_reentries;
            } expected[] = {
                {bench::r3::Arm::a0, 0, 0, 0, 0},
                {bench::r3::Arm::b, 1, 8, 0, 0},
                {bench::r3::Arm::r, 2, 0, 1, 0},
                {bench::r3::Arm::q8, 2, 0, 0, 1},
                {bench::r3::Arm::q2, 8, 0, 0, 7},
            };
            for (const Expected& want : expected) {
                Slots round_slots(kDepth);
                bench::r3::Adapter adapter(ctx, want.arm);
                Batch batch;
                const std::vector<ReadOp> ops = take_ops(file, round_slots, 0, kDepth, batch);
                std::vector<Request<std::size_t>> accepted;
                auto outcome =
                    adapter.submit(std::span<const ReadOp>(ops.data(), ops.size()), accepted);
                const std::string name = bench::r3::arm_name(want.arm);
                const bool all_accepted =
                    accepted.size() == kDepth && outcome.error == std::nullopt;
                const bool counters_match =
                    outcome.counters.r4_batch_presentations == want.presentations &&
                    outcome.counters.suffix_single_calls == want.suffix_singles &&
                    outcome.counters.r4_cap_reentries == want.cap_reentries &&
                    outcome.counters.r4_chunk_reentries == want.chunk_reentries;
                check(all_accepted && counters_match,
                      name + ": accept-all and presentation/singles/reentry counts match");
                batch.requests = std::move(accepted);
                bench::r3::DecisionFacts facts;
                check(settle(ctx, round_slots, batch, facts),
                      name + ": refill settles verified");
            }
            ctx.request_stop(ShutdownPolicy::drain);
            ctx.detach_progress_host();
            const auto shutdown = ctx.shutdown(ShutdownPolicy::drain);
            check(shutdown.has_value(), "counter run shutdown");
        }
    }

    std::printf("\n=== r4 arm smoke complete, failing checks=%d ===\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
