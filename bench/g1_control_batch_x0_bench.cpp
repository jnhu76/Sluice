// g1_control_batch_x0_bench — BATCH-X0 (G1-Control Candidate 3, #227/#221/#259).
//
// RESEARCH-ONLY falsification bench. Production code untouched; the current
// public Batch contract is unchanged. Preregistration:
// research/g1-control-batch-x0/BATCH-X0-PREREGISTRATION.md (freeze c0da5db5).
//
// Modes (machine-readable JSON lines on stdout; human text on stderr):
//   semantic  — fixtures S1..S10 (scripted backend + real-uring capacity rows
//               + the decisive S9 interleaving witness on the MB substrate)
//   selftest  — mutants M1..M5 + the M4 verifier-sanity check; M6/M7/M8 are
//               validator-side (research/g1-control-batch-x0/scripts/)
//   perf      — one arm/op/size/N cell; one JSON row per rep
//
// Arms (prereg §2): B0 raw liburing loop / B1 production per-op submit_* +
// manual drive / B2 production Batch / MB1 mini research floor per-op shape /
// MB3 the same floor with ONE admission section per batch. MB arms keep the
// FULL per-op identity ladder (adetail::RequestArena +
// adetail::submit_transaction: per-op slots, generations, would_block
// admission, reap-only publication) and differ from MB1 ONLY in admission
// critical-section count and install-episode count. They are attribution
// instruments, never production evidence. Without liburing the bench fails
// closed (no arms exist to measure).

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/batch.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/async/detail/submit_transaction.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace sluice;
using namespace sluice::async;
namespace adetail = sluice::async::detail;

namespace {

// ---------------------------------------------------------------------------
// utilities
// ---------------------------------------------------------------------------

double now_ns() noexcept {
    return double(std::chrono::steady_clock::now().time_since_epoch().count());
}

double process_cpu_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return double(ts.tv_sec) * 1e9 + double(ts.tv_nsec);
}

std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

void fill_pattern(std::byte* buf, std::size_t len, std::uint64_t offset) noexcept {
    for (std::size_t pos = 0; pos + 8 <= len; pos += 8) {
        const std::uint64_t v = splitmix64(offset + pos);
        std::memcpy(buf + pos, &v, 8);
    }
    for (std::size_t pos = len & ~std::size_t{7}; pos < len; ++pos)
        buf[pos] = std::byte{static_cast<unsigned char>(splitmix64(offset + pos))};
}

bool verify_pattern(const std::byte* buf, std::size_t len, std::uint64_t offset) noexcept {
    for (std::size_t pos = 0; pos + 8 <= len; pos += 8) {
        std::uint64_t got = 0;
        std::memcpy(&got, buf + pos, 8);
        if (got != splitmix64(offset + pos)) return false;
    }
    for (std::size_t pos = len & ~std::size_t{7}; pos < len; ++pos) {
        if (buf[pos] !=
            std::byte{static_cast<unsigned char>(splitmix64(offset + pos))})
            return false;
    }
    return true;
}

std::uint64_t offset_for(std::uint64_t counter, std::size_t size,
                         std::size_t file_size) {
    return (counter * static_cast<std::uint64_t>(size)) %
           static_cast<std::uint64_t>(file_size);
}

void pin_cpu(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::fprintf(stderr, "sched_setaffinity failed\n");
        std::exit(4);
    }
}

ssize_t full_pread(int fd, void* buf, std::size_t len, std::uint64_t off) {
    std::byte* p = static_cast<std::byte*>(buf);
    std::size_t done = 0;
    while (done < len) {
        const ssize_t r = ::pread(fd, p + done, len - done,
                                  static_cast<off_t>(off + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        done += static_cast<std::size_t>(r);
    }
    return static_cast<ssize_t>(done);
}
ssize_t full_pwrite(int fd, const void* buf, std::size_t len, std::uint64_t off) {
    const std::byte* p = static_cast<const std::byte*>(buf);
    std::size_t done = 0;
    while (done < len) {
        const ssize_t r = ::pwrite(fd, p + done, len - done,
                                   static_cast<off_t>(off + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += static_cast<std::size_t>(r);
    }
    return static_cast<ssize_t>(done);
}

void emit_fixture(const char* name, const char* result, const std::string& detail) {
    std::printf("{\"kind\":\"fixture\",\"fixture\":\"%s\",\"result\":\"%s\","
                "\"detail\":\"%s\"}\n",
                name, result, detail.c_str());
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// shared iteration-trace verifiers (fixtures prove them; mutants must FAIL
// them — the M1/M2/M3/M5 self-tests feed deliberately corrupted traces)
// ---------------------------------------------------------------------------

struct IterRecord {
    std::size_t index = 0;
    BatchResultOrigin origin = BatchResultOrigin::accepted_and_completed;
    bool is_void = false;
    std::optional<Result<std::size_t>> size_res;
    std::optional<Result<void>> void_res;
};

bool check_order(const std::vector<IterRecord>& got,
                 const std::vector<std::size_t>& expect, std::string& why) {
    if (got.size() != expect.size()) {
        why = "count " + std::to_string(got.size()) + " != " +
              std::to_string(expect.size());
        return false;
    }
    for (std::size_t i = 0; i < expect.size(); ++i) {
        if (got[i].index != expect[i]) {
            why = "order mismatch at " + std::to_string(i) + ": got index " +
                  std::to_string(got[i].index);
            return false;
        }
    }
    return true;
}

bool check_membership(const std::vector<IterRecord>& got, std::size_t n,
                      std::string& why) {
    if (got.size() != n) {
        why = "member count " + std::to_string(got.size()) + " != " +
              std::to_string(n);
        return false;
    }
    std::vector<bool> seen(n, false);
    for (const auto& r : got) {
        if (r.index >= n || seen[r.index]) {
            why = "duplicate/out-of-range index " + std::to_string(r.index);
            return false;
        }
        seen[r.index] = true;
    }
    return true;
}

bool check_origin(const std::vector<IterRecord>& got,
                  const std::map<std::size_t, BatchResultOrigin>& expect,
                  std::string& why) {
    for (const auto& r : got) {
        auto it = expect.find(r.index);
        if (it == expect.end() || it->second != r.origin) {
            why = "origin mismatch at index " + std::to_string(r.index);
            return false;
        }
    }
    return true;
}

// M5: a group-cancel collapse marks unaffected members canceled.
bool check_no_group_collapse(const std::vector<IterRecord>& got,
                             std::size_t canceled_index,
                             const std::map<std::size_t, std::size_t>& ok_bytes,
                             std::string& why) {
    for (const auto& r : got) {
        if (r.index == canceled_index) continue;
        auto it = ok_bytes.find(r.index);
        if (it == ok_bytes.end()) {
            why = "no expectation for index " + std::to_string(r.index);
            return false;
        }
        if (r.origin != BatchResultOrigin::accepted_and_completed ||
            r.is_void || !r.size_res.has_value() || !r.size_res->has_value() ||
            r.size_res->value() != it->second) {
            why = "member " + std::to_string(r.index) +
                  " was disturbed by another member's cancel (group collapse)";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// ScriptedBackend — deterministic semantic-mode backend through the PUBLIC
// submit path (SequenceBackend shape from tests/batch_reap_order_test.cpp),
// extended with submission-ordinal rejection injection, wait-error injection,
// and backend-side member cancel (the public Batch surface exposes NO member
// Completion handles — see the S10 note).
// ---------------------------------------------------------------------------
class ScriptedBackend final : public AsyncBackend {
public:
    void auto_stage_size(std::size_t slot, std::size_t bytes) {
        auto_plan_[slot] = Plan{bytes, false, IoError{IoError::Code::backend_error},
                                false};
    }
    void auto_stage_size_error(std::size_t slot, IoError e) {
        auto_plan_[slot] = Plan{0, true, e, false};
    }
    void auto_stage_void_ok(std::size_t slot) {
        auto_plan_[slot] = Plan{0, false, IoError{IoError::Code::backend_error},
                                true};
    }
    void stage_size(std::size_t slot, std::size_t bytes) {
        stage_size_res(capture_size_at(slot), Result<std::size_t>{bytes});
    }
    void stage_size_error(std::size_t slot, IoError e) {
        stage_size_res(capture_size_at(slot), make_unexpected<std::size_t>(e));
    }
    void stage_void_ok(std::size_t slot) {
        stage_void_res(capture_void_at(slot), Result<void>{});
    }
    void reject_submission_ordinal(std::size_t ordinal) {
        reject_ordinals_.insert(ordinal);
    }
    void set_next_wait_one_error(IoError e) { next_wait_err_ = e; }

    Result<void> submit_read(ReadOp, Completion<std::size_t>& c) override {
        return capture_size(c);
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>& c) override {
        return capture_size(c);
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>& c) override {
        return capture_void(c);
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>& c) override {
        return capture_void(c);
    }

    std::size_t poll() override {
        std::size_t n = 0;
        while (!reap_queue_.empty()) {
            Entry e = reap_queue_.front();
            reap_queue_.pop_front();
            if (!e.is_void) {
                auto* c = static_cast<Completion<std::size_t>*>(e.c);
                if (e.size_res.has_value()) publish(*c, std::move(*e.size_res));
            } else {
                auto* c = static_cast<Completion<void>*>(e.c);
                if (e.void_res.has_value()) publish(*c, std::move(*e.void_res));
            }
            --outstanding_;
            ++n;
        }
        return n;
    }

    Result<std::size_t> wait_one() override {
        if (next_wait_err_.has_value()) {
            IoError e = *next_wait_err_;
            next_wait_err_.reset();
            return make_unexpected<std::size_t>(e);
        }
        return Result<std::size_t>{poll()};
    }

    // Backend-side scripted member cancel: publish a canceled terminal for
    // exactly ONE member; drop it from the pending reap queue if present.
    // Other members are never touched (that is the property M5 attacks).
    void cancel_member(std::size_t slot) {
        const std::size_t kind = kinds_.at(slot);
        void* target = captures_.at(slot).c;
        for (auto it = reap_queue_.begin(); it != reap_queue_.end(); ++it) {
            if (it->c == target && it->is_void == (kind != 0)) {
                reap_queue_.erase(it);
                break;
            }
        }
        if (kind == 0) {
            auto* c = static_cast<Completion<std::size_t>*>(target);
            if (!c->ready()) {
                publish(*c, make_unexpected<std::size_t>(
                                IoError{IoError::Code::canceled}));
                --outstanding_;
            }
        } else {
            auto* c = static_cast<Completion<void>*>(target);
            if (!c->ready()) {
                publish(*c, make_unexpected<void>(IoError{IoError::Code::canceled}));
                --outstanding_;
            }
        }
    }

    std::size_t outstanding() const noexcept override { return outstanding_; }

private:
    struct Plan {
        std::size_t bytes = 0;
        bool is_error = false;
        IoError err{IoError::Code::backend_error};
        bool is_void = false;
    };
    struct Entry {
        void* c = nullptr;
        bool is_void = false;
        std::optional<Result<std::size_t>> size_res;
        std::optional<Result<void>> void_res;
    };

    Result<void> capture_size(Completion<std::size_t>& c) {
        if (take_rejection()) return make_unexpected<void>(IoError{IoError::Code::would_block});
        if (!try_claim(c))
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        kinds_.push_back(0);
        captures_.push_back(Entry{&c, false});
        ++outstanding_;
        apply_auto_size(last_submit_ordinal_, c);
        return {};
    }
    Result<void> capture_void(Completion<void>& c) {
        if (take_rejection()) return make_unexpected<void>(IoError{IoError::Code::would_block});
        if (!try_claim(c))
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        kinds_.push_back(1);
        captures_.push_back(Entry{&c, true});
        ++outstanding_;
        apply_auto_void(last_submit_ordinal_, c);
        return {};
    }
    bool take_rejection() {
        const bool hit = reject_ordinals_.count(submit_ordinal_) != 0;
        if (hit)
            reject_ordinals_.erase(submit_ordinal_);
        else
            last_submit_ordinal_ = submit_ordinal_;
        ++submit_ordinal_;
        return hit;
    }
    void apply_auto_size(std::size_t ordinal, Completion<std::size_t>& c) {
        auto it = auto_plan_.find(ordinal);
        if (it == auto_plan_.end()) return;
        const Plan p = it->second;
        auto_plan_.erase(it);
        if (p.is_error)
            stage_size_res(&c, make_unexpected<std::size_t>(p.err));
        else
            stage_size_res(&c, Result<std::size_t>{p.bytes});
    }
    void apply_auto_void(std::size_t ordinal, Completion<void>& c) {
        auto it = auto_plan_.find(ordinal);
        if (it == auto_plan_.end()) return;
        auto_plan_.erase(it);
        stage_void_res(&c, Result<void>{});
    }
    void stage_size_res(Completion<std::size_t>* c, Result<std::size_t> r) {
        Entry e;
        e.c = c;
        e.is_void = false;
        e.size_res = std::move(r);
        reap_queue_.push_back(std::move(e));
    }
    void stage_void_res(Completion<void>* c, Result<void> r) {
        Entry e;
        e.c = c;
        e.is_void = true;
        e.void_res = std::move(r);
        reap_queue_.push_back(std::move(e));
    }
    Completion<std::size_t>* capture_size_at(std::size_t slot) {
        return static_cast<Completion<std::size_t>*>(captures_.at(slot).c);
    }
    Completion<void>* capture_void_at(std::size_t slot) {
        return static_cast<Completion<void>*>(captures_.at(slot).c);
    }

    std::vector<Entry> captures_;
    std::vector<std::size_t> kinds_;  // 0 = size-carrying, 1 = void
    std::map<std::size_t, Plan> auto_plan_;
    std::deque<Entry> reap_queue_;
    std::set<std::size_t> reject_ordinals_;
    std::size_t submit_ordinal_ = 0;
    std::size_t last_submit_ordinal_ = 0;  // ordinal of the last ACCEPTED submit
    std::size_t outstanding_ = 0;
    std::optional<IoError> next_wait_err_;
};

// ---------------------------------------------------------------------------
// [MB-BEGIN] MiniBatchBackend — the research-only semantic floor (prereg §2).
// Full per-op identity ladder (RequestArena + submit_transaction): per-op
// slots, generations, would_block admission, transactional submission,
// reap-only publication through the protected helpers. It differs from the
// production uring backend ONLY in outer admission granularity:
//   MB1 shape: submit_member per op — 2 admission sections per op
//              (ladder | enqueue+install), production-shaped
//   MB3 shape: submit_batch — ONE admission section for the whole batch
// No cancel, no wait source, no close_admission, no router/ledger:
// single-driver research floor, quiescent destruction. NOT production
// evidence; never a vtable entry; never installed.
// ---------------------------------------------------------------------------
class MiniBatchBackend final : public AsyncBackend {
public:
    struct Member {
        enum class Kind : std::uint8_t { read, write };
        Kind kind = Kind::read;
        int fd = -1;
        void* buf = nullptr;
        std::size_t len = 0;
        std::uint64_t offset = 0;
        Completion<std::size_t>* c = nullptr;
    };

    MiniBatchBackend(std::size_t capacity, unsigned depth)
        : arena_(adetail::ContextIdentity::for_testing(kMiniContextId), capacity),
          route_(capacity) {
        for (std::size_t i = 0; i < capacity; ++i)
            route_free_.push_back(static_cast<std::uint32_t>(capacity - 1 - i));
        if (::io_uring_queue_init(depth, &ring_, 0) == 0) ring_ok_ = true;
    }

    // AsyncBackend pure virtuals: MB1 shape routes size ops through
    // submit_member; sync ops are out of the MB perf scope (not_supported).
    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) override {
        Member m{Member::Kind::read, op.fd, op.dst, op.len, op.offset, &c};
        return submit_member(m);
    }
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) override {
        Member m{Member::Kind::write, op.fd, const_cast<std::byte*>(op.src),
                 op.len, op.offset, &c};
        return submit_member(m);
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::not_supported});
    }

    ~MiniBatchBackend() override {
        if (ring_ok_) {
            if (outstanding() != 0) std::terminate();  // quiescent destruction
            ::io_uring_queue_exit(&ring_);
        }
    }

    bool ring_ready() const noexcept { return ring_ok_; }

    // MB1 shape: production-shaped per-op submission (2 admission sections).
    Result<void> submit_member(const Member& m) {
        adetail::SlotHandle h{};
        {
            std::lock_guard<std::mutex> lk(adm_mtx_);
            ++admission_sections_;
            auto r = ladder_locked_(m);
            if (!r.has_value()) return make_unexpected<void>(r.error());
            h = r.value();
        }
        {
            std::lock_guard<std::mutex> lk(adm_mtx_);
            ++admission_sections_;
            stage2_locked_(h, m);
        }
        return {};
    }

    // MB3 shape: ONE admission section for the whole batch; partial admission
    // is first-class (a failed member does not stop the others — Batch shape).
    std::vector<Result<void>> submit_batch(const Member* ms, std::size_t n) {
        std::vector<Result<void>> out(n);
        std::lock_guard<std::mutex> lk(adm_mtx_);
        ++admission_sections_;
        for (std::size_t i = 0; i < n; ++i) {
            auto r = ladder_locked_(ms[i]);
            if (!r.has_value()) {
                out[i] = make_unexpected<void>(r.error());
                continue;
            }
            stage2_locked_(r.value(), ms[i]);
            out[i] = Result<void>{};
        }
        return out;
    }

    std::size_t poll() override {
        {
            std::lock_guard<std::mutex> lk(adm_mtx_);
            drain_pending_locked_();
            flush_locked_();
        }
        reap_cqes_();
        return arena_.reap(sink_);
    }

    Result<std::size_t> wait_one() override {
        std::size_t n = poll();
        if (n > 0) return Result<std::size_t>{n};
        if (arena_.accepted_outstanding() == 0) return Result<std::size_t>{0};
        for (;;) {
            ++wait_enters_;
            const int rc =
                ::io_uring_enter(ring_.ring_fd, 0, 1, IORING_ENTER_GETEVENTS, nullptr);
            if (rc < 0) {
                if (errno == EINTR) continue;
                return make_unexpected<std::size_t>(
                    IoError{IoError::Code::backend_error});
            }
            reap_cqes_();
            n = arena_.reap(sink_);
            if (n > 0) return Result<std::size_t>{n};
            if (arena_.accepted_outstanding() == 0) return Result<std::size_t>{0};
        }
    }

    std::size_t outstanding() const noexcept override {
        return arena_.accepted_outstanding();
    }

    // research counters / witnesses (bench-consumed, per measured window)
    std::uint64_t admission_sections_ = 0;
    std::uint64_t flush_calls_ = 0;
    std::uint64_t wait_enters_ = 0;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> identity_log_;

    void reset_research_state() {
        admission_sections_ = 0;
        flush_calls_ = 0;
        wait_enters_ = 0;
        identity_log_.clear();
    }

private:
    static constexpr std::uint64_t kMiniContextId = 0x4241544330ull;  // 'BATC0'

    struct Route {
        std::uint64_t cookie = 0;
        adetail::SlotHandle handle{};
        bool in_use = false;
    };
    struct Pending {
        adetail::SlotHandle h{};
        Member m{};
    };

    struct Policy {
        using completion_type = Completion<std::size_t>;
        using op_type = Member;

        Policy(MiniBatchBackend& self, adetail::OperationKind k) noexcept
            : self_(self), kind_(k) {}

        adetail::OperationKind kind() const noexcept { return kind_; }

        static adetail::OperationKind kind_of(const Member& m) noexcept {
            return m.kind == Member::Kind::read ? adetail::OperationKind::read
                                                : adetail::OperationKind::write;
        }
        static adetail::BorrowMetadata borrow(const Member& m) noexcept {
            return adetail::BorrowMetadata{m.fd, m.buf, m.len};
        }
        static std::uint64_t requested_bytes(const Member& m) noexcept {
            return m.len;
        }
        static void (*publish_thunk() noexcept)(
            void*, const adetail::TerminalResult&) noexcept {
            return &MiniBatchBackend::publish_size_ready;
        }
        static bool begin_binding(Completion<std::size_t>& c) noexcept {
            return MiniBatchBackend::begin_binding(c);
        }
        static void install_binding(Completion<std::size_t>& c,
                                    adetail::RequestArena* arena,
                                    adetail::SlotHandle h) noexcept {
            MiniBatchBackend::install_binding(c, arena, h);
        }
        static void commit_binding(Completion<std::size_t>& c) noexcept {
            MiniBatchBackend::commit_binding(c);
        }
        static void rollback_binding(Completion<std::size_t>& c) noexcept {
            MiniBatchBackend::rollback_binding_before_accept(c);
        }
        Result<void> stage0_precheck() const noexcept {
            if (!self_.ring_ok_)
                return make_unexpected<void>(IoError{IoError::Code::backend_error});
            return {};
        }
        static Result<void> validate(const Member& m) noexcept {
            if (m.fd < 0 || m.buf == nullptr || m.len == 0 || m.len > 0xffffffffull)
                return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
            return {};
        }
        static void write_scratch(adetail::SlotHandle, const Member&) noexcept {}
        void pause_before_commit_binding() noexcept {}  // test seam hook; no-op here

      private:
        MiniBatchBackend& self_;
        adetail::OperationKind kind_;
    };

    Result<adetail::SlotHandle> ladder_locked_(const Member& m) {
        Policy policy{*this, Policy::kind_of(m)};
        auto r = adetail::submit_transaction(arena_, *m.c, m, policy);
        if (!r.has_value()) return make_unexpected<adetail::SlotHandle>(r.error());
        identity_log_.emplace_back(r.value().slot.value, r.value().generation.value);
        return r;
    }

    void stage2_locked_(adetail::SlotHandle h, const Member& m) {
        const auto out = arena_.enqueue(h);
        if (out != adetail::EnqueueOutcome::enqueued) {
            std::fprintf(stderr, "mini: enqueue not enqueued (invariant)\n");
            std::terminate();
        }
        pending_.push_back(Pending{h, m});
        drain_pending_locked_();
    }

    void drain_pending_locked_() {
        while (!pending_.empty()) {
            if (install_locked_(pending_.front().h, pending_.front().m))
                pending_.pop_front();
            else
                break;  // SQ full: front stays; retried at next drive
        }
    }

    bool install_locked_(adetail::SlotHandle h, const Member& m) {
        if (route_free_.empty()) {
            std::fprintf(stderr, "mini: router exhaustion (invariant)\n");
            std::terminate();
        }
        const std::uint32_t router_slot = route_free_.back();
        route_free_.pop_back();
        io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            flush_locked_();
            sqe = ::io_uring_get_sqe(&ring_);
            if (sqe == nullptr) {
                route_free_.push_back(router_slot);
                return false;
            }
        }
        const std::uint64_t cookie = ++next_cookie_;
        if (m.kind == Member::Kind::read)
            ::io_uring_prep_read(sqe, m.fd, m.buf, static_cast<unsigned>(m.len),
                                 static_cast<off_t>(m.offset));
        else
            ::io_uring_prep_write(sqe, m.fd, m.buf, static_cast<unsigned>(m.len),
                                  static_cast<off_t>(m.offset));
        ::io_uring_sqe_set_data64(sqe, cookie);
        route_[router_slot] = Route{cookie, h, true};
        if (!arena_.mark_running(h)) {
            std::fprintf(stderr, "mini: mark_running false (invariant)\n");
            std::terminate();
        }
        return true;
    }

    void flush_locked_() {
        if (!ring_ok_ || ::io_uring_sq_ready(&ring_) == 0) return;
        ++flush_calls_;
        int rc = 0;
        for (;;) {
            rc = ::io_uring_submit(&ring_);
            if (rc == -EINTR) continue;
            break;
        }
        if (rc < 0) {
            std::fprintf(stderr, "mini: io_uring_submit failed\n");
            std::terminate();  // research floor: no recovery controller
        }
    }

    void reap_cqes_() {
        io_uring_cqe* cqes[64];
        for (;;) {
            const unsigned got = ::io_uring_peek_batch_cqe(&ring_, cqes, 64);
            if (got == 0) break;
            for (unsigned i = 0; i < got; ++i) {
                const std::uint64_t cookie = ::io_uring_cqe_get_data64(cqes[i]);
                const int res = cqes[i]->res;
                adetail::TerminalResult t =
                    res >= 0 ? adetail::TerminalResult::ok_bytes(
                                   static_cast<std::uint64_t>(res))
                             : adetail::TerminalResult::err(
                                   IoError{IoError::Code::backend_error});
                for (std::size_t ri = 0; ri < route_.size(); ++ri) {
                    if (route_[ri].in_use && route_[ri].cookie == cookie) {
                        if (!arena_.record_terminal(route_[ri].handle, t)) {
                            std::fprintf(stderr,
                                         "mini: record_terminal rejected "
                                         "(generation invariant)\n");
                            std::terminate();
                        }
                        route_[ri] = Route{};
                        route_free_.push_back(static_cast<std::uint32_t>(ri));
                        break;
                    }
                }
            }
            ::io_uring_cq_advance(&ring_, got);
        }
    }

    static void publish_size_ready(void* completion,
                                   const adetail::TerminalResult& t) noexcept {
        auto* c = static_cast<Completion<std::size_t>*>(completion);
        if (!t.is_error)
            AsyncBackend::publish(
                *c, Result<std::size_t>{static_cast<std::size_t>(t.bytes)});
        else
            AsyncBackend::publish(*c, make_unexpected<std::size_t>(t.error));
    }

    adetail::RequestArena arena_;
    adetail::ReferenceReadySink sink_;
    std::mutex adm_mtx_;
    io_uring ring_{};
    bool ring_ok_ = false;
    std::vector<Route> route_;
    std::vector<std::uint32_t> route_free_;
    std::deque<Pending> pending_;
    std::uint64_t next_cookie_ = 0;
};
// [MB-END]

// ---------------------------------------------------------------------------
// perf mode
// ---------------------------------------------------------------------------

struct PerfConfig {
    std::string arm;   // B0 B1 B2 MB1 MB3
    std::string op;    // read write
    std::size_t size = 4096;
    std::size_t n = 1;
    int reps = 7;
    int cpu = -1;
    std::string file;
    std::size_t file_size = std::size_t{8} << 20;
};

struct RoundCounters {
    std::uint64_t submits = 0;
    std::uint64_t episodes = 0;
};

void emit_perf_row(const std::string& arm, const PerfConfig& cfg, int rep,
                   std::uint64_t rounds, double wall_ns, double cpu_ns,
                   const RoundCounters& rc, const MiniBatchBackend* mb) {
    const double ops = double(rounds * cfg.n);
    std::uint64_t sections = 0, flushes = 0, waiters = 0, ilog = 0,
                  distinct = 0;
    if (mb != nullptr) {
        sections = mb->admission_sections_;
        flushes = mb->flush_calls_;
        waiters = mb->wait_enters_;
        ilog = mb->identity_log_.size();
        if (ilog >= cfg.n) {
            const std::size_t start =
                static_cast<std::size_t>(ilog / cfg.n - 1) * cfg.n;
            std::set<std::uint32_t> slots;
            for (std::size_t i = start; i < ilog; ++i)
                slots.insert(mb->identity_log_[i].first);
            distinct = slots.size();
        }
    }
    std::printf(
        "{\"kind\":\"perf\",\"arm\":\"%s\",\"op\":\"%s\",\"size\":%zu,"
        "\"n\":%zu,\"rep\":%d,\"rounds\":%" PRIu64 ",\"ops\":%" PRIu64
        ",\"wall_ns\":%.0f,\"wall_per_op_ns\":%.1f,\"cpu_per_op_ns\":%.1f,"
        "\"submits\":%" PRIu64 ",\"drive_episodes\":%" PRIu64
        ",\"admission_sections\":%" PRIu64 ",\"flush_calls\":%" PRIu64
        ",\"wait_enters\":%" PRIu64 ",\"distinct_slots_per_round\":%" PRIu64
        ",\"identity_entries\":%" PRIu64 ",\"work\":\"ok\"}\n",
        arm.c_str(), cfg.op.c_str(), cfg.size, cfg.n, rep, rounds,
        rounds * cfg.n, wall_ns, wall_ns / ops, cpu_ns / ops, rc.submits,
        rc.episodes, sections, flushes, waiters,
        static_cast<std::uint64_t>(distinct), ilog);
    std::fflush(stdout);
}

std::uint64_t calibrate_rounds(const PerfConfig& cfg, double probe_ns) {
    const double per_round = probe_ns / 3.0;
    const double need = 50e6;  // prereg §7: rep wall >= 50 ms
    double rounds = std::max(2000.0 / double(cfg.n), need / per_round);
    if (rounds < 1.0) rounds = 1.0;
    return std::min<std::uint64_t>(static_cast<std::uint64_t>(std::ceil(rounds)),
                                   200000);
}

// verify the LAST round's n ops (outside the timed window)
int verify_last_round(const PerfConfig& cfg, int fd,
                      std::vector<std::vector<std::byte>>& bufs, std::uint64_t counter,
                      bool is_read) {
    const std::uint64_t base = counter - cfg.n;
    for (std::size_t i = 0; i < cfg.n; ++i) {
        const std::uint64_t off = offset_for(base + i, cfg.size, cfg.file_size);
        if (is_read) {
            if (!verify_pattern(bufs[i].data(), cfg.size, off)) return 6;
        } else {
            std::vector<std::byte> rb(cfg.size);
            if (full_pread(fd, rb.data(), cfg.size, off) != (ssize_t)cfg.size)
                return 6;
            if (!verify_pattern(rb.data(), cfg.size, off)) return 6;
        }
    }
    return 0;
}

int arm_b0(const PerfConfig& cfg, int fd, std::vector<std::vector<std::byte>>& bufs) {
    io_uring ring{};
    if (::io_uring_queue_init(64, &ring, 0) != 0) return 3;
    std::vector<io_uring_cqe*> cqes(64);
    const bool is_read = cfg.op == "read";
    std::uint64_t counter = 0;
    for (int rep = 0; rep < cfg.reps; ++rep) {
        const double t0 = now_ns();
        for (std::uint64_t rd = 0; rd < 3; ++rd) {  // calibration probe
            for (std::size_t i = 0; i < cfg.n; ++i) {
                const std::uint64_t off =
                    offset_for(counter++, cfg.size, cfg.file_size);
                io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
                if (is_read)
                    ::io_uring_prep_read(sqe, fd, bufs[i].data(),
                                         static_cast<unsigned>(cfg.size),
                                         static_cast<off_t>(off));
                else
                    ::io_uring_prep_write(sqe, fd, bufs[i].data(),
                                          static_cast<unsigned>(cfg.size),
                                          static_cast<off_t>(off));
                ::io_uring_sqe_set_data64(sqe, i);
            }
            if (::io_uring_submit_and_wait(&ring, static_cast<unsigned>(cfg.n)) < 0)
                return 5;
            unsigned got = 0;
            while (got < cfg.n) {
                const unsigned g =
                    ::io_uring_peek_batch_cqe(&ring, cqes.data(), 64);
                if (g == 0) continue;
                for (unsigned k = 0; k < g; ++k)
                    if (cqes[k]->res < 0) return 5;
                got += g;
                ::io_uring_cq_advance(&ring, g);
            }
        }
        const std::uint64_t rounds = calibrate_rounds(cfg, now_ns() - t0);
        RoundCounters rc;
        const double w0 = now_ns();
        const double c0 = process_cpu_ns();
        for (std::uint64_t rd = 0; rd < rounds; ++rd) {
            for (std::size_t i = 0; i < cfg.n; ++i) {
                const std::uint64_t off =
                    offset_for(counter++, cfg.size, cfg.file_size);
                if (!is_read) fill_pattern(bufs[i].data(), cfg.size, off);
                io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
                if (is_read)
                    ::io_uring_prep_read(sqe, fd, bufs[i].data(),
                                         static_cast<unsigned>(cfg.size),
                                         static_cast<off_t>(off));
                else
                    ::io_uring_prep_write(sqe, fd, bufs[i].data(),
                                          static_cast<unsigned>(cfg.size),
                                          static_cast<off_t>(off));
                ::io_uring_sqe_set_data64(sqe, i);
            }
            ++rc.submits;
            if (::io_uring_submit_and_wait(&ring, static_cast<unsigned>(cfg.n)) < 0)
                return 5;
            unsigned got = 0;
            while (got < cfg.n) {
                const unsigned g =
                    ::io_uring_peek_batch_cqe(&ring, cqes.data(), 64);
                if (g == 0) continue;
                for (unsigned k = 0; k < g; ++k)
                    if (cqes[k]->res != static_cast<int>(cfg.size)) return 5;
                got += g;
                ::io_uring_cq_advance(&ring, g);
            }
            ++rc.episodes;
        }
        const double wall = now_ns() - w0;
        const double cpu = process_cpu_ns() - c0;
        const int vr = verify_last_round(cfg, fd, bufs, counter, is_read);
        if (vr != 0) {
            ::io_uring_queue_exit(&ring);
            return vr;
        }
        emit_perf_row("B0", cfg, rep, rounds, wall, cpu, rc, nullptr);
    }
    ::io_uring_queue_exit(&ring);
    return 0;
}

bool round_b1(const PerfConfig& cfg, int fd, std::vector<std::vector<std::byte>>& bufs,
              AsyncIoContext& ctx, std::vector<Completion<std::size_t>>& comps,
              std::uint64_t& counter, bool is_read, RoundCounters* rc) {
    (void)fd;
    for (auto& c : comps) c.reset();
    for (std::size_t i = 0; i < cfg.n; ++i) {
        const std::uint64_t off = offset_for(counter++, cfg.size, cfg.file_size);
        if (!is_read) fill_pattern(bufs[i].data(), cfg.size, off);
        Result<void> r =
            is_read ? ctx.submit_read(ReadOp{fd, bufs[i].data(), cfg.size, off},
                                      comps[i])
                    : ctx.submit_write(WriteOp{fd, bufs[i].data(), cfg.size, off},
                                       comps[i]);
        if (!r.has_value()) return false;
        if (rc != nullptr) ++rc->submits;
    }
    std::vector<char> done(cfg.n, 0);
    std::size_t done_count = 0;
    while (done_count < cfg.n) {
        auto wr = ctx.wait_one();
        if (!wr.has_value()) return false;
        if (rc != nullptr) ++rc->episodes;
        for (std::size_t i = 0; i < cfg.n; ++i) {
            if (done[i] == 0 && comps[i].ready()) {
                done[i] = 1;
                ++done_count;
            }
        }
        if (wr.value() == 0 && ctx.outstanding() > 0) return false;
    }
    for (std::size_t i = 0; i < cfg.n; ++i) {
        const auto& r = comps[i].result();
        if (!r.has_value() || r.value() != cfg.size) return false;
    }
    return true;
}

int arm_b1(const PerfConfig& cfg, int fd, std::vector<std::vector<std::byte>>& bufs) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{64, 64});
    if (!backend->available()) return 3;
    AsyncIoContext ctx(std::move(backend));
    const bool is_read = cfg.op == "read";
    std::vector<Completion<std::size_t>> comps(cfg.n);
    std::uint64_t counter = 0;
    for (int rep = 0; rep < cfg.reps; ++rep) {
        const double t0 = now_ns();
        std::uint64_t probe_counter = counter;
        for (std::uint64_t rd = 0; rd < 3; ++rd)
            if (!round_b1(cfg, fd, bufs, ctx, comps, probe_counter, is_read,
                          nullptr))
                return 5;
        const std::uint64_t rounds = calibrate_rounds(cfg, now_ns() - t0);
        RoundCounters rc;
        const double w0 = now_ns();
        const double c0 = process_cpu_ns();
        for (std::uint64_t rd = 0; rd < rounds; ++rd)
            if (!round_b1(cfg, fd, bufs, ctx, comps, counter, is_read, &rc))
                return 5;
        const double wall = now_ns() - w0;
        const double cpu = process_cpu_ns() - c0;
        const int vr = verify_last_round(cfg, fd, bufs, counter, is_read);
        if (vr != 0) return vr;
        emit_perf_row("B1", cfg, rep, rounds, wall, cpu, rc, nullptr);
    }
    return 0;
}

bool round_b2(const PerfConfig& cfg, int fd, std::vector<std::vector<std::byte>>& bufs,
              AsyncIoContext& ctx, std::uint64_t& counter, bool is_read,
              RoundCounters* rc) {
    Batch batch;
    for (std::size_t i = 0; i < cfg.n; ++i) {
        const std::uint64_t off = offset_for(counter++, cfg.size, cfg.file_size);
        if (!is_read) fill_pattern(bufs[i].data(), cfg.size, off);
        BatchOp op;
        if (is_read) {
            op.kind = BatchOp::Kind::read;
            op.read = ReadOp{fd, bufs[i].data(), cfg.size, off};
        } else {
            op.kind = BatchOp::Kind::write;
            op.write = WriteOp{fd, bufs[i].data(), cfg.size, off};
        }
        (void)batch.add(op);
    }
    std::size_t popped = 0;
    while (popped < cfg.n) {
        auto ar = batch.await_one(ctx);
        if (!ar.has_value()) return false;
        if (rc != nullptr) ++rc->episodes;
        while (auto res = batch.next()) {
            ++popped;
            if (res->origin != BatchResultOrigin::accepted_and_completed)
                return false;
            if (!res->is_void && res->size_res->value() != cfg.size)
                return false;
        }
    }
    return true;
}

int arm_b2(const PerfConfig& cfg, int fd, std::vector<std::vector<std::byte>>& bufs) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{64, 64});
    if (!backend->available()) return 3;
    AsyncIoContext ctx(std::move(backend));
    const bool is_read = cfg.op == "read";
    std::uint64_t counter = 0;
    for (int rep = 0; rep < cfg.reps; ++rep) {
        const double t0 = now_ns();
        std::uint64_t probe_counter = counter;
        for (std::uint64_t rd = 0; rd < 3; ++rd)
            if (!round_b2(cfg, fd, bufs, ctx, probe_counter, is_read, nullptr))
                return 5;
        const std::uint64_t rounds = calibrate_rounds(cfg, now_ns() - t0);
        RoundCounters rc;
        const double w0 = now_ns();
        const double c0 = process_cpu_ns();
        for (std::uint64_t rd = 0; rd < rounds; ++rd)
            if (!round_b2(cfg, fd, bufs, ctx, counter, is_read, &rc)) return 5;
        const double wall = now_ns() - w0;
        const double cpu = process_cpu_ns() - c0;
        const int vr = verify_last_round(cfg, fd, bufs, counter, is_read);
        if (vr != 0) return vr;
        emit_perf_row("B2", cfg, rep, rounds, wall, cpu, rc, nullptr);
    }
    return 0;
}

bool round_mb(const PerfConfig& cfg, int fd, std::vector<std::vector<std::byte>>& bufs,
              MiniBatchBackend& mb, std::vector<Completion<std::size_t>>& comps,
              std::vector<MiniBatchBackend::Member>& members,
              std::uint64_t& counter, bool is_read, bool fused,
              RoundCounters* rc) {
    for (auto& c : comps) c.reset();
    members.resize(cfg.n);
    for (std::size_t i = 0; i < cfg.n; ++i) {
        const std::uint64_t off = offset_for(counter++, cfg.size, cfg.file_size);
        if (!is_read) fill_pattern(bufs[i].data(), cfg.size, off);
        members[i] = MiniBatchBackend::Member{
            is_read ? MiniBatchBackend::Member::Kind::read
                    : MiniBatchBackend::Member::Kind::write,
            fd, bufs[i].data(), cfg.size, off, &comps[i]};
    }
    if (fused) {
        auto rs = mb.submit_batch(members.data(), members.size());
        for (const auto& r : rs)
            if (!r.has_value()) return false;
    } else {
        for (const auto& m : members)
            if (!mb.submit_member(m).has_value()) return false;
    }
    std::vector<char> done(cfg.n, 0);
    std::size_t done_count = 0;
    while (done_count < cfg.n) {
        auto wr = mb.wait_one();
        if (!wr.has_value()) return false;
        if (rc != nullptr) ++rc->episodes;
        for (std::size_t i = 0; i < cfg.n; ++i) {
            if (done[i] == 0 && comps[i].ready()) {
                done[i] = 1;
                ++done_count;
            }
        }
        if (wr.value() == 0 && mb.outstanding() > 0) return false;
    }
    for (std::size_t i = 0; i < cfg.n; ++i) {
        const auto& r = comps[i].result();
        if (!r.has_value() || r.value() != cfg.size) return false;
    }
    return true;
}

int arm_mb(const PerfConfig& cfg, int fd, std::vector<std::vector<std::byte>>& bufs,
           bool fused) {
    MiniBatchBackend mb(64, 64);
    if (!mb.ring_ready()) return 3;
    std::vector<Completion<std::size_t>> comps(cfg.n);
    std::vector<MiniBatchBackend::Member> members;
    const bool is_read = cfg.op == "read";
    const std::string arm = fused ? "MB3" : "MB1";
    std::uint64_t counter = 0;
    for (int rep = 0; rep < cfg.reps; ++rep) {
        const double t0 = now_ns();
        std::uint64_t probe_counter = counter;
        for (std::uint64_t rd = 0; rd < 3; ++rd)
            if (!round_mb(cfg, fd, bufs, mb, comps, members, probe_counter,
                          is_read, fused, nullptr))
                return 5;
        const std::uint64_t rounds = calibrate_rounds(cfg, now_ns() - t0);
        counter = probe_counter;  // timed window replays the probe range
        mb.reset_research_state();
        RoundCounters rc;
        const double w0 = now_ns();
        const double c0 = process_cpu_ns();
        for (std::uint64_t rd = 0; rd < rounds; ++rd)
            if (!round_mb(cfg, fd, bufs, mb, comps, members, counter, is_read,
                          fused, &rc))
                return 5;
        const double wall = now_ns() - w0;
        const double cpu = process_cpu_ns() - c0;
        const int vr = verify_last_round(cfg, fd, bufs, counter, is_read);
        if (vr != 0) return vr;
        emit_perf_row(arm, cfg, rep, rounds, wall, cpu, rc, &mb);
    }
    return 0;
}

int run_perf(const PerfConfig& cfg, int fd) {
    if (cfg.n == 0 || cfg.size == 0) return 2;
    std::vector<std::vector<std::byte>> bufs(cfg.n);
    for (auto& b : bufs) {
        b.resize(cfg.size);
        fill_pattern(b.data(), b.size(), 0xdeadbeef);
    }
    if (cfg.arm == "B0") return arm_b0(cfg, fd, bufs);
    if (cfg.arm == "B1") return arm_b1(cfg, fd, bufs);
    if (cfg.arm == "B2") return arm_b2(cfg, fd, bufs);
    if (cfg.arm == "MB1") return arm_mb(cfg, fd, bufs, false);
    if (cfg.arm == "MB3") return arm_mb(cfg, fd, bufs, true);
    return 2;
}

// ---------------------------------------------------------------------------
// semantic fixtures
// ---------------------------------------------------------------------------

// S1 — real uring: 4 independent positional reads, exact patterns, all
// accepted_and_completed. (Scripted all-succeed + mixed-kind + reap-order
// permutations are pinned by tests/batch_*; the formal grid needs the
// real-backend row.)
bool fixture_s1(int fd) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{16, 16});
    AsyncIoContext ctx(std::move(backend));
    Batch batch;
    std::vector<std::unique_ptr<std::byte[]>> bufs;
    const std::vector<std::uint64_t> offs = {0, 4096, 8192, 12288};
    for (std::size_t i = 0; i < 4; ++i) {
        bufs.push_back(std::make_unique<std::byte[]>(4096));
        BatchOp op;
        op.kind = BatchOp::Kind::read;
        op.read = ReadOp{fd, bufs[i].get(), 4096, offs[i]};
        (void)batch.add(op);
    }
    bool ok = true;
    std::string why = "ok";
    std::vector<IterRecord> got;
    std::size_t popped = 0;
    while (popped < 4) {
        auto ar = batch.await_one(ctx);
        if (!ar.has_value()) {
            emit_fixture("S1", "FAIL", "await_one error");
            return false;
        }
        while (auto res = batch.next()) {
            ++popped;
            got.push_back(IterRecord{res->index, res->origin, res->is_void,
                                     res->size_res, res->void_res});
        }
    }
    ok = check_membership(got, 4, why);
    for (const auto& r : got) {
        const std::size_t i = r.index;
        if (!ok) break;
        if (r.origin != BatchResultOrigin::accepted_and_completed ||
            r.is_void || !r.size_res.has_value() ||
            r.size_res->value() != 4096 ||
            !verify_pattern(bufs[i].get(), 4096, offs[i])) {
            why = "member " + std::to_string(i) + " wrong result/pattern";
            ok = false;
        }
    }
    emit_fixture("S1", ok ? "PASS" : "FAIL", ok ? why : why);
    return ok;
}

// S2 — real uring, request capacity 4, batch of 6: exactly members 4,5 are
// submit-rejected (would_block, origin rejected); members 0-3 complete.
bool fixture_s2(int fd) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 16});
    AsyncIoContext ctx(std::move(backend));
    Batch batch;
    std::vector<std::unique_ptr<std::byte[]>> bufs;
    for (std::size_t i = 0; i < 6; ++i) {
        bufs.push_back(std::make_unique<std::byte[]>(4096));
        BatchOp op;
        op.kind = BatchOp::Kind::read;
        op.read = ReadOp{fd, bufs[i].get(), 4096, i * 4096};
        (void)batch.add(op);
    }
    bool ok = true;
    std::string why;
    std::map<std::size_t, BatchResultOrigin> expect;
    std::vector<IterRecord> got;
    std::size_t popped = 0;
    while (popped < 6) {
        auto ar = batch.await_one(ctx);
        if (!ar.has_value()) {
            emit_fixture("S2", "FAIL", "await_one error");
            return false;
        }
        while (auto res = batch.next()) {
            ++popped;
            got.push_back(IterRecord{res->index, res->origin, res->is_void,
                                     res->size_res, res->void_res});
        }
    }
    for (std::size_t i = 0; i < 6; ++i)
        expect[i] = i < 4 ? BatchResultOrigin::accepted_and_completed
                          : BatchResultOrigin::rejected;
    ok = check_membership(got, 6, why) && check_origin(got, expect, why);
    for (const auto& r : got) {
        if (!ok) break;
        if (r.index >= 4) {
            if (r.size_res->error().code != IoError::Code::would_block) {
                why = "rejected member " + std::to_string(r.index) +
                      " is not would_block";
                ok = false;
            }
        } else if (r.size_res->value() != 4096 ||
                   !verify_pattern(bufs[r.index].get(), 4096, r.index * 4096)) {
            why = "accepted member " + std::to_string(r.index) + " wrong result";
            ok = false;
        }
    }
    // submit-rejections surface FIRST, in submission order (indexes 4 then 5)
    if (ok && (got[0].index != 4 || got[1].index != 5)) {
        why = "rejections did not surface first in submission order";
        ok = false;
    }
    emit_fixture("S2", ok ? "PASS" : "FAIL", ok ? "4 accepted + 2 would_block, rejections first" : why);
    return ok;
}

// S3 — accepted I/O error distinguishable from submit rejection (origin is
// NOT inferred from success/error).
bool fixture_s3() {
    auto be = std::make_unique<ScriptedBackend>();
    ScriptedBackend* sb = be.get();
    AsyncIoContext ctx(std::move(be));
    Batch batch;
    std::vector<std::byte> buf(64);
    for (std::size_t i = 0; i < 3; ++i) {
        BatchOp op;
        op.kind = BatchOp::Kind::read;
        op.read = ReadOp{3, buf.data(), 64, i * 64};
        (void)batch.add(op);
    }
    sb->reject_submission_ordinal(0);                       // member 0: rejected
    sb->auto_stage_size_error(1, IoError{IoError::Code::backend_error, 5});  // accepted error
    sb->auto_stage_size(2, 64);                             // accepted success
    bool ok = true;
    std::string why;
    std::vector<IterRecord> got;
    std::size_t popped = 0;
    while (popped < 3) {
        auto ar = batch.await_one(ctx);
        if (!ar.has_value()) {
            emit_fixture("S3", "FAIL", "await_one error");
            return false;
        }
        while (auto res = batch.next()) {
            ++popped;
            got.push_back(IterRecord{res->index, res->origin, res->is_void,
                                     res->size_res, res->void_res});
        }
    }
    std::map<std::size_t, BatchResultOrigin> expect;
    expect[0] = BatchResultOrigin::rejected;
    expect[1] = BatchResultOrigin::accepted_and_completed;
    expect[2] = BatchResultOrigin::accepted_and_completed;
    ok = check_membership(got, 3, why) && check_origin(got, expect, why);
    for (const auto& r : got) {
        if (!ok) break;
        if (r.index == 0 &&
            r.size_res->error().code != IoError::Code::would_block) {
            why = "member 0 rejection is not would_block";
            ok = false;
        }
        if (r.index == 1 &&
            r.size_res->error().code != IoError::Code::backend_error) {
            why = "member 1 accepted error lost its I/O error";
            ok = false;
        }
    }
    emit_fixture("S3", ok ? "PASS" : "FAIL", ok ? "rejected(would_block) vs accepted(io_error) distinguished" : why);
    return ok;
}

// S4 — out-of-order completion: submit 0,1,2; reap 2,0,1; next() must follow.
bool fixture_s4() {
    auto be = std::make_unique<ScriptedBackend>();
    ScriptedBackend* sb = be.get();
    AsyncIoContext ctx(std::move(be));
    Batch batch;
    std::vector<std::byte> buf(64);
    for (std::size_t i = 0; i < 3; ++i) {
        BatchOp op;
        op.kind = BatchOp::Kind::read;
        op.read = ReadOp{3, buf.data(), 64, i * 64};
        (void)batch.add(op);
    }
    // Deterministic Phase-1 escape (the same mechanism the regression suite
    // uses for post-submit staging): one injected wait error makes the first
    // await_one return AFTER it has submitted all three members but BEFORE
    // any result exists. The error is harness plumbing, not part of the
    // witness; the clean second await drives the staged 2,0,1 reaps.
    sb->set_next_wait_one_error(IoError{IoError::Code::backend_error});
    auto ar0 = batch.await_one(ctx);
    if (ar0.has_value()) {
        emit_fixture("S4", "FAIL", "phase-1 escape did not error");
        return false;
    }
    sb->stage_size(2, 64);
    sb->stage_size(0, 64);
    sb->stage_size(1, 64);
    std::vector<IterRecord> got;
    std::size_t popped = 0;
    while (popped < 3) {
        auto ar = batch.await_one(ctx);
        if (!ar.has_value()) {
            emit_fixture("S4", "FAIL", "await_one error (drive)");
            return false;
        }
        while (auto res = batch.next()) {
            ++popped;
            got.push_back(IterRecord{res->index, res->origin, res->is_void,
                                     res->size_res, res->void_res});
        }
    }
    std::string why;
    const bool ok =
        check_order(got, {2, 0, 1}, why) && check_membership(got, 3, why);
    emit_fixture("S4", ok ? "PASS" : "FAIL", ok ? "reap order 2,0,1 preserved" : why);
    return ok;
}

// S5 — mixed-kinds batch (read + write + sync_data + sync_all).
bool fixture_s5() {
    auto be = std::make_unique<ScriptedBackend>();
    ScriptedBackend* sb = be.get();
    AsyncIoContext ctx(std::move(be));
    Batch batch;
    std::vector<std::byte> buf(64);
    BatchOp r;
    r.kind = BatchOp::Kind::read;
    r.read = ReadOp{3, buf.data(), 64, 0};
    (void)batch.add(r);
    BatchOp w;
    w.kind = BatchOp::Kind::write;
    w.write = WriteOp{3, buf.data(), 64, 0};
    (void)batch.add(w);
    BatchOp sd;
    sd.kind = BatchOp::Kind::sync_data;
    sd.sync_data = SyncDataOp{3};
    (void)batch.add(sd);
    BatchOp sa;
    sa.kind = BatchOp::Kind::sync_all;
    sa.sync_all = SyncAllOp{3};
    (void)batch.add(sa);
    sb->auto_stage_size(0, 64);
    sb->auto_stage_size(1, 64);
    sb->auto_stage_void_ok(2);
    sb->auto_stage_void_ok(3);
    bool ok = true;
    std::string why;
    std::vector<IterRecord> got;
    std::size_t popped = 0;
    while (popped < 4) {
        auto ar = batch.await_one(ctx);
        if (!ar.has_value()) {
            emit_fixture("S5", "FAIL", "await_one error");
            return false;
        }
        while (auto res = batch.next()) {
            ++popped;
            got.push_back(IterRecord{res->index, res->origin, res->is_void,
                                     res->size_res, res->void_res});
        }
    }
    ok = check_order(got, {0, 1, 2, 3}, why);
    for (const auto& rec : got) {
        if (!ok) break;
        const bool expect_void = rec.index >= 2;
        if (rec.is_void != expect_void ||
            rec.origin != BatchResultOrigin::accepted_and_completed) {
            why = "kind/origin mismatch at " + std::to_string(rec.index);
            ok = false;
        }
    }
    emit_fixture("S5", ok ? "PASS" : "FAIL", ok ? "mixed kinds iterated with per-kind payloads" : why);
    return ok;
}

// S6 — zero members: as-built audited behavior (idle ctx: await_one == 0,
// next() == nullopt; busy ctx: the CONTEXT-GLOBAL outstanding predicate makes
// the empty batch wait out unrelated work — audit F8).
bool fixture_s6() {
    auto be = std::make_unique<ScriptedBackend>();
    ScriptedBackend* sb = be.get();
    AsyncIoContext ctx(std::move(be));
    Batch batch;
    auto ar = batch.await_one(ctx);
    bool ok = ar.has_value() && ar.value() == 0 && !batch.next().has_value();
    std::string why = ok ? "idle: await_one==0, next()==nullopt" : "idle behavior diverged";
    // busy-context variant: one UNRELATED outstanding op
    if (ok) {
        Completion<std::size_t> ext;
        sb->auto_stage_size(0, 64);
        auto sr = ctx.submit_read(ReadOp{3, nullptr, 0, 0}, ext);
        if (!sr.has_value()) {
            emit_fixture("S6", "FAIL", "external submit failed");
            return false;
        }
        auto ar2 = batch.await_one(ctx);
        const bool busy_ok = ar2.has_value() && ar2.value() == 0 &&
                             !batch.next().has_value() && ext.ready();
        why = busy_ok ? "idle + busy-context (external drained) recorded"
                      : "busy-context behavior diverged";
        ok = ok && busy_ok;
    }
    emit_fixture("S6", ok ? "PASS" : "FAIL", why);
    return ok;
}

// S7 — one member: single-op batch completes with correct identity.
bool fixture_s7(int fd) {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 8});
    AsyncIoContext ctx(std::move(backend));
    Batch batch;
    std::unique_ptr<std::byte[]> buf(new std::byte[4096]);
    BatchOp op;
    op.kind = BatchOp::Kind::read;
    op.read = ReadOp{fd, buf.get(), 4096, 0};
    (void)batch.add(op);
    bool ok = true;
    std::size_t popped = 0;
    while (popped < 1) {
        auto ar = batch.await_one(ctx);
        if (!ar.has_value()) {
            emit_fixture("S7", "FAIL", "await_one error");
            return false;
        }
        while (auto res = batch.next()) {
            ++popped;
            ok = res->index == 0 &&
                 res->origin == BatchResultOrigin::accepted_and_completed &&
                 res->size_res->value() == 4096 &&
                 verify_pattern(buf.get(), 4096, 0);
        }
    }
    emit_fixture("S7", ok ? "PASS" : "FAIL", ok ? "N=1 negative control clean" : "N=1 wrong result");
    return ok;
}

// S8 — capacity boundary on the real arena backend: N = C-1, C, C+1, fresh
// backend each; exactly zero/zero/one rejections.
bool fixture_s8(int fd) {
    bool all_ok = true;
    std::string why_all;
    for (std::size_t n : {std::size_t{7}, std::size_t{8}, std::size_t{9}}) {
        auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 16});
        AsyncIoContext ctx(std::move(backend));
        Batch batch;
        std::vector<std::unique_ptr<std::byte[]>> bufs;
        for (std::size_t i = 0; i < n; ++i) {
            bufs.push_back(std::make_unique<std::byte[]>(4096));
            BatchOp op;
            op.kind = BatchOp::Kind::read;
            op.read = ReadOp{fd, bufs[i].get(), 4096, i * 4096};
            (void)batch.add(op);
        }
        std::size_t rejected = 0, popped = 0;
        bool ok = true;
        while (popped < n) {
            auto ar = batch.await_one(ctx);
            if (!ar.has_value()) {
                ok = false;
                break;
            }
            while (auto res = batch.next()) {
                ++popped;
                if (res->origin == BatchResultOrigin::rejected) {
                    ++rejected;
                    if (res->size_res->error().code != IoError::Code::would_block)
                        ok = false;
                }
            }
        }
        const std::size_t expect = (n == 9) ? 1 : 0;
        if (rejected != expect || !ok) {
            why_all = "N=" + std::to_string(n) + ": rejected=" +
                      std::to_string(rejected) + " expected=" +
                      std::to_string(expect);
            all_ok = false;
            emit_fixture("S8", "FAIL", why_all);
            return false;
        }
    }
    emit_fixture("S8", "PASS", "C-1/C/C+1 rejection shape exact");
    return all_ok;
}

// S9 — THE decisive interleaving/accepted-membership witness (prereg §4/§6).
// MB substrate, capacity 3. Per-op shape admits the external B between batch
// members; the fused shape cannot. The accepted SET is the observable.
struct S9Set {
    std::vector<std::string> accepted;
    std::vector<std::string> rejected;
    bool operator==(const S9Set& o) const {
        return accepted == o.accepted && rejected == o.rejected;
    }
};

std::string s9_json_array(const std::vector<std::string>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += "\"" + v[i] + "\"";
    }
    return s + "]";
}

const char* s9_verdict(const S9Set& per_op, const S9Set& fused) {
    return per_op == fused ? "NO_DIVERGENCE" : "DIVERGENCE";
}

bool fixture_s9(int fd) {
    const std::size_t len = 4096;
    const std::size_t cap = 3;

    // --- per-op shape: A1, A2, external B, A3, A4 ---
    MiniBatchBackend mb(cap, 16);
    if (!mb.ring_ready()) {
        emit_fixture("S9", "FAIL", "no ring");
        return false;
    }
    std::vector<std::byte> bufA(len), bufB(len);
    std::vector<Completion<std::size_t>> comps(5);
    const auto member_of = [&](std::size_t i, std::uint64_t off) {
        return MiniBatchBackend::Member{MiniBatchBackend::Member::Kind::read,
                                        fd, bufA.data(), len, off, &comps[i]};
    };
    S9Set per_op;
    const auto note = [](S9Set& s, const char* name, const Result<void>& r) {
        if (r.has_value())
            s.accepted.push_back(name);
        else
            s.rejected.push_back(r.error().code == IoError::Code::would_block
                                     ? std::string(name) + ":would_block"
                                     : std::string(name) + ":error");
    };
    auto r1 = mb.submit_read(ReadOp{fd, bufA.data(), len, 0}, comps[0]);
    note(per_op, "A1", r1);
    auto r2 = mb.submit_read(ReadOp{fd, bufA.data(), len, len}, comps[1]);
    note(per_op, "A2", r2);
    auto rb = mb.submit_read(ReadOp{fd, bufB.data(), len, 0}, comps[2]);
    note(per_op, "B", rb);
    auto r3 = mb.submit_read(ReadOp{fd, bufA.data(), len, 2 * len}, comps[3]);
    note(per_op, "A3", r3);
    auto r4 = mb.submit_read(ReadOp{fd, bufA.data(), len, 3 * len}, comps[4]);
    note(per_op, "A4", r4);
    (void)member_of;
    while (mb.outstanding() > 0) (void)mb.wait_one();

    // --- fused shape: batch {A1..A4} in ONE admission section, then B ---
    MiniBatchBackend mb2(cap, 16);
    std::vector<Completion<std::size_t>> comps2(5);
    std::vector<MiniBatchBackend::Member> batch;
    const std::uint64_t offs[4] = {0, len, 2 * len, 3 * len};
    for (std::size_t i = 0; i < 4; ++i)
        batch.push_back(MiniBatchBackend::Member{
            MiniBatchBackend::Member::Kind::read, fd, bufA.data(), len,
            offs[i], &comps2[i]});
    auto fres = mb2.submit_batch(batch.data(), batch.size());
    auto fb = mb2.submit_read(ReadOp{fd, bufB.data(), len, 0}, comps2[4]);
    S9Set fused;
    const char* names[4] = {"A1", "A2", "A3", "A4"};
    for (std::size_t i = 0; i < 4; ++i)
        note(fused, names[i], fres[i]);
    note(fused, "B", fb);
    while (mb2.outstanding() > 0) (void)mb2.wait_one();

    const char* v = s9_verdict(per_op, fused);
    std::printf(
        "{\"kind\":\"fixture\",\"fixture\":\"S9\",\"verdict\":\"%s\","
        "\"per_op_accepted\":%s,\"per_op_rejected\":%s,"
        "\"fused_accepted\":%s,\"fused_rejected\":%s}\n",
        v, s9_json_array(per_op.accepted).c_str(),
        s9_json_array(per_op.rejected).c_str(),
        s9_json_array(fused.accepted).c_str(),
        s9_json_array(fused.rejected).c_str());
    std::fflush(stdout);
    emit_fixture("S9", "PASS", std::string("witness recorded: ") + v);
    return true;  // the witness is valid whatever it shows (prereg §4)
}

// S10 — per-member terminal independence (backend-side member cancel; the
// public Batch surface exposes NO member Completion handles — recorded fact).
bool fixture_s10() {
    auto be = std::make_unique<ScriptedBackend>();
    ScriptedBackend* sb = be.get();
    AsyncIoContext ctx(std::move(be));
    Batch batch;
    std::vector<std::byte> buf(64);
    for (std::size_t i = 0; i < 3; ++i) {
        BatchOp op;
        op.kind = BatchOp::Kind::read;
        op.read = ReadOp{3, buf.data(), 64, i * 64};
        (void)batch.add(op);
    }
    sb->auto_stage_size(0, 64);
    // member 1: canceled terminal (backend-side; wins before its staged result
    // would exist — a canceled member)
    sb->auto_stage_size_error(1, IoError{IoError::Code::canceled});
    sb->auto_stage_size(2, 64);
    bool ok = true;
    std::string why;
    std::vector<IterRecord> got;
    std::size_t popped = 0;
    while (popped < 3) {
        auto ar = batch.await_one(ctx);
        if (!ar.has_value()) {
            emit_fixture("S10", "FAIL", "await_one error");
            return false;
        }
        while (auto res = batch.next()) {
            ++popped;
            got.push_back(IterRecord{res->index, res->origin, res->is_void,
                                     res->size_res, res->void_res});
        }
    }
    ok = check_membership(got, 3, why);
    // canceled member: origin accepted_and_completed (it crossed accept) with
    // a canceled error; the other two members keep their own results.
    std::map<std::size_t, std::size_t> ok_bytes{{0, 64}, {2, 64}};
    bool canceled_seen = false;
    for (const auto& r : got) {
        if (!ok) break;
        if (r.index == 1) {
            canceled_seen =
                r.origin == BatchResultOrigin::accepted_and_completed &&
                r.size_res->error().code == IoError::Code::canceled;
            if (!canceled_seen) {
                why = "member 1 canceled terminal lost";
                ok = false;
            }
        }
    }
    if (ok) ok = check_no_group_collapse(got, 1, ok_bytes, why);
    emit_fixture("S10", ok ? "PASS" : "FAIL", ok ? "per-member terminals independent; canceled member origin accepted" : why);
    return ok;
}

int run_semantic(int fd) {
    int bad = 0;
    // Order: S9 LAST so a harness crash cannot hide the cheaper fixtures.
    if (!fixture_s1(fd)) ++bad;
    if (!fixture_s2(fd)) ++bad;
    if (!fixture_s3()) ++bad;
    if (!fixture_s4()) ++bad;
    if (!fixture_s5()) ++bad;
    if (!fixture_s6()) ++bad;
    if (!fixture_s7(fd)) ++bad;
    if (!fixture_s8(fd)) ++bad;
    if (!fixture_s10()) ++bad;
    if (!fixture_s9(fd)) ++bad;
    return bad == 0 ? 0 : 9;
}

// ---------------------------------------------------------------------------
// mutant self-tests: each M* feeds a DELIBERATELY WRONG trace/claim through
// the same verifiers the fixtures use; REJECT is the only acceptable outcome.
// ---------------------------------------------------------------------------

struct MutantOutcome {
    const char* mutant;
    const char* result;
    const char* detail;
};

void emit_mutant(const MutantOutcome& m) {
    std::printf("{\"kind\":\"mutant\",\"mutant\":\"%s\",\"result\":\"%s\","
                "\"detail\":\"%s\"}\n",
                m.mutant, m.result, m.detail);
    std::fflush(stdout);
}

int run_selftest() {
    int bad = 0;
    MutantOutcome m;
    std::string why;

    // M1 — batch-level terminal collapse: only one member present.
    {
        std::vector<IterRecord> got{
            IterRecord{0, BatchResultOrigin::accepted_and_completed, false,
                       Result<std::size_t>{64}, {}}};
        const bool rejected = !check_membership(got, 3, why);
        m = {"M1", rejected ? "REJECT" : "PASS", rejected ? "collapsed trace rejected" : why.c_str()};
        if (!rejected) ++bad;
        emit_mutant(m);
    }
    // M2 — origin collapse: submit rejection disguised as accepted error.
    {
        std::vector<IterRecord> got{
            IterRecord{0, BatchResultOrigin::accepted_and_completed, false,
                       make_unexpected<std::size_t>(IoError{IoError::Code::would_block}), {}},
            IterRecord{1, BatchResultOrigin::accepted_and_completed, false,
                       Result<std::size_t>{64}, {}}};
        std::map<std::size_t, BatchResultOrigin> expect{
            {0, BatchResultOrigin::rejected},
            {1, BatchResultOrigin::accepted_and_completed}};
        const bool rejected = !check_origin(got, expect, why);
        m = {"M2", rejected ? "REJECT" : "PASS", rejected ? "origin collapse rejected" : why.c_str()};
        if (!rejected) ++bad;
        emit_mutant(m);
    }
    // M3 — submission-order iteration where reap order was 2,0,1.
    {
        std::vector<IterRecord> got{
            IterRecord{0, BatchResultOrigin::accepted_and_completed, false, Result<std::size_t>{64}, {}},
            IterRecord{1, BatchResultOrigin::accepted_and_completed, false, Result<std::size_t>{64}, {}},
            IterRecord{2, BatchResultOrigin::accepted_and_completed, false, Result<std::size_t>{64}, {}}};
        const bool rejected = !check_order(got, {2, 0, 1}, why);
        m = {"M3", rejected ? "REJECT" : "PASS", rejected ? "submission-order trace rejected" : why.c_str()};
        if (!rejected) ++bad;
        emit_mutant(m);
    }
    // M4 — hidden atomic admission: the s9 verdict helper must call two
    // different accepted-sets DIVERGENCE (the equivalence-claim rejector).
    {
        S9Set a;
        a.accepted = {"A1", "A2", "B"};
        a.rejected = {"A3:would_block", "A4:would_block"};
        S9Set b;
        b.accepted = {"A1", "A2", "A3"};
        b.rejected = {"A4:would_block", "B:would_block"};
        S9Set b2 = b;
        const bool ok = std::string(s9_verdict(a, b)) == "DIVERGENCE" &&
                        std::string(s9_verdict(b2, b)) == "NO_DIVERGENCE";
        m = {"M4", ok ? "REJECT" : "PASS", ok ? "equivalence claim rejected on diverged sets" : "s9 verdict helper broken"};
        if (!ok) ++bad;
        emit_mutant(m);
    }
    // M5 — group cancellation collapse: one canceled member disturbs others.
    {
        std::vector<IterRecord> got{
            IterRecord{0, BatchResultOrigin::accepted_and_completed, false, Result<std::size_t>{64}, {}},
            IterRecord{1, BatchResultOrigin::accepted_and_completed, false,
                       make_unexpected<std::size_t>(IoError{IoError::Code::canceled}), {}},
            IterRecord{2, BatchResultOrigin::accepted_and_completed, false,
                       make_unexpected<std::size_t>(IoError{IoError::Code::canceled}), {}}};
        std::map<std::size_t, std::size_t> ok_bytes{{0, 64}, {2, 64}};
        const bool rejected = !check_no_group_collapse(got, 1, ok_bytes, why);
        m = {"M5", rejected ? "REJECT" : "PASS", rejected ? "group-collapse trace rejected" : why.c_str()};
        if (!rejected) ++bad;
        emit_mutant(m);
    }
    // M6/M7/M8 — validator-side (enter-count claims, identity witnesses,
    // MB-region budget). Recorded as locator rows for the validator.
    m = {"M6", "VALIDATOR", "enter-count claims checked by validate_batch_x0.py"};
    emit_mutant(m);
    m = {"M7", "VALIDATOR", "identity witnesses checked by validate_batch_x0.py"};
    emit_mutant(m);
    m = {"M8", "VALIDATOR", "MB-region budget checked by validate_batch_x0.py"};
    emit_mutant(m);
    return bad == 0 ? 0 : 8;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        std::fprintf(stderr,
                     "usage: %s semantic --file PATH | selftest | perf "
                     "--arm B0|B1|B2|MB1|MB3 --op read|write --size N --n N "
                     "[--reps N] [--cpu N] --file PATH\n",
                     argv[0]);
        return 2;
    }
#if !defined(SLUICE_HAS_LIBURING)
    std::printf("{\"kind\":\"meta\",\"supported\":false,\"reason\":\"no liburing\"}\n");
    return 3;
#else
    const std::string mode = args[0];
    PerfConfig cfg;
    std::string file;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        const std::string& k = args[i];
        const std::string& v = args[i + 1];
        if (k == "--arm") cfg.arm = v;
        else if (k == "--op") cfg.op = v;
        else if (k == "--size") cfg.size = std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "--n") cfg.n = std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "--reps") cfg.reps = std::atoi(v.c_str());
        else if (k == "--cpu") cfg.cpu = std::atoi(v.c_str());
        else if (k == "--file") file = v;
    }
    pin_cpu(cfg.cpu);
    if (mode == "prepare") {
        // prefill the data file with the splitmix per-offset pattern READ
        // verification expects; used by the driver before READ cells
        const int pfd = ::open(file.c_str(), O_RDWR | O_CREAT, 0644);
        if (pfd < 0) {
            std::fprintf(stderr, "open(%s) failed: %s\n", file.c_str(), strerror(errno));
            return 4;
        }
        const std::size_t chunk = 65536;
        std::vector<std::byte> buf(chunk);
        for (std::size_t off = 0; off < cfg.file_size; off += chunk) {
            fill_pattern(buf.data(), chunk, off);
            if (full_pwrite(pfd, buf.data(), chunk, off) != (ssize_t)chunk) {
                std::fprintf(stderr, "prepare write failed\n");
                ::close(pfd);
                return 5;
            }
        }
        ::fsync(pfd);
        ::close(pfd);
        std::printf("{\"kind\":\"meta\",\"mode\":\"prepare\",\"bytes\":%zu}\n",
                    cfg.file_size);
        return 0;
    }
    if (mode == "selftest") {
        std::printf("{\"kind\":\"meta\",\"supported\":true,\"mode\":\"selftest\"}\n");
        return run_selftest();
    }
    if (file.empty() && mode != "prepare") {
        std::fprintf(stderr, "--file is required for %s\n", mode.c_str());
        return 2;
    }
    if (mode == "prepare" && file.empty()) {
        std::fprintf(stderr, "--file is required for prepare\n");
        return 2;
    }
    int fd;
    if (mode == "perf" && cfg.op == "write")
        fd = ::open(file.c_str(), O_RDWR);
    else
        fd = ::open(file.c_str(), O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "open(%s) failed: %s\n", file.c_str(),
                     strerror(errno));
        return 4;
    }
    if (mode == "semantic") {
        std::printf("{\"kind\":\"meta\",\"supported\":true,\"mode\":\"semantic\"}\n");
        return run_semantic(fd);
    }
    if (mode == "perf") {
        std::printf("{\"kind\":\"meta\",\"supported\":true,\"mode\":\"perf\","
                    "\"arm\":\"%s\",\"op\":\"%s\",\"size\":%zu,\"n\":%zu,"
                    "\"reps\":%d}\n",
                    cfg.arm.c_str(), cfg.op.c_str(), cfg.size, cfg.n, cfg.reps);
        return run_perf(cfg, fd);
    }
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    ::close(fd);
    return 2;
#endif
}
