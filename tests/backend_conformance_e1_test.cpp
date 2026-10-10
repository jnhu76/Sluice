// E1 cross-backend File-contract conformance: the same semantic claims must
// hold for the ThreadPool profile and the io_uring profile. The file is
// compiled twice; SLUICE_E1_CONFORMANCE_URING selects the io_uring variant.
// The uring variant also links the ThreadPool backend so the metadata records
// of both profiles can be compared against each other and against the kernel.

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#if defined(SLUICE_E1_CONFORMANCE_URING)
#include <sluice/async/uring_backend.hpp>
#endif
#include <sluice/effect.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(SLUICE_E1_CONFORMANCE_URING)
#ifndef SLUICE_HAS_LIBURING
#error "the uring conformance variant must receive SLUICE_HAS_LIBURING"
#endif
#include <liburing.h>
#endif

namespace {

using namespace sluice::async;
using sluice::EffectCertainty;
using sluice::EffectReport;
using sluice::File;
using sluice::FileInfo;
using sluice::FileSize;
using sluice::IoError;
using sluice::Result;
using sluice::async::detail::CoreSnapshot;
using sluice::async::detail::PublicCancel;
using sluice::async::detail::RequestCore;

constexpr std::uint64_t kControlTag = std::uint64_t{1} << 63u;
constexpr std::uint64_t kUnrepresentableOffset = std::numeric_limits<std::uint64_t>::max();

struct Tracker {
    const char* name;
    int failures = 0;

    void check(bool ok, const char* label) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s\n", name, label);
        }
    }
};

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_e1_conformance_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    if (!content.empty()) {
        const ssize_t n = ::write(fd, content.data(), content.size());
        if (n != static_cast<ssize_t>(content.size())) {
            ::close(fd);
            return {};
        }
    }
    ::close(fd);
    return path;
}

struct Fixture {
    int fd = -1;

    bool ok() const { return fd >= 0; }
};

Fixture make_fixture(const std::string& content) {
    const std::string path = make_temp_file(content);
    if (path.empty())
        return {};
    const int fd = ::open(path.c_str(), O_RDWR);
    ::unlink(path.c_str());
    if (fd < 0)
        return {};
    return Fixture{fd};
}

#if defined(SLUICE_E1_CONFORMANCE_URING)
using Backend = UringAsyncBackend;

std::unique_ptr<Backend> make_backend(std::size_t capacity) {
    return std::make_unique<Backend>(UringConfig{capacity, 8});
}

bool backend_quiescent(Backend& backend) {
    return backend.dispatch_size_for_test() == 0 && backend.live_cookies_for_test() == 0;
}
#else
using Backend = ThreadPoolBackend;

std::unique_ptr<Backend> make_backend(std::size_t capacity) {
    return std::make_unique<Backend>(ThreadPoolConfig{capacity, 1});
}

bool backend_quiescent(Backend& backend) {
    return backend.dispatch_occupancy() == 0 && backend.active_workers() == 0;
}
#endif

bool core_is_idle(const CoreSnapshot& s) {
    return s.reserved == 0 && s.accepted_live == 0 && s.terminal_live == 0 &&
           s.published_live == 0 && s.execution_refs == 0 && s.control_refs == 0 &&
           s.publication_refs == 0 && s.public_bindings == 0;
}

template <class Gate> void wait_gate_paused(Gate& gate) {
    while (!gate.paused.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

template <class Gate> struct GateGuard {
    Gate& gate;
    explicit GateGuard(Gate& g) : gate(g) {}
    ~GateGuard() {
        gate.resume.store(true, std::memory_order_release);
        gate.resume.notify_all();
    }
    GateGuard(const GateGuard&) = delete;
    GateGuard& operator=(const GateGuard&) = delete;
};

template <class T> void drive_ready(AsyncIoContext& ctx, Request<T>& request) {
    while (!request.ready())
        (void)ctx.poll();
}

template <class T> Request<T>& req(Result<Request<T>>& submitted) {
    return submitted.value();
}

bool read_reports_confirmed_counts_and_effect(Tracker& t) {
    const std::string content = "sluice e1 read conformance body";
    Fixture fix = make_fixture(content);
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    auto backend = make_backend(4);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> full(content.size(), std::byte{0});
    auto whole = ctx.submit_read(ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write},
                                        full.data(), full.size(), 0});
    t.check(whole.has_value(), "the full-range read is accepted");
    if (!whole.has_value())
        return false;
    drive_ready(ctx, req(whole));
    auto observed = req(whole).try_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == content.size(),
            "the full read reports the confirmed count");
    t.check(observed.effect == EffectReport{content.size(), EffectCertainty::accounted},
            "a confirmed read reports the accounted effect");
    t.check(std::memcmp(full.data(), content.data(), content.size()) == 0,
            "the read fills the buffer from the file");
    req(whole).take_result();

    std::vector<std::byte> tail(4, std::byte{0});
    auto past_end = ctx.submit_read(ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write},
                                           tail.data(), tail.size(), content.size()});
    t.check(past_end.has_value(), "the past-end read is accepted");
    if (!past_end.has_value())
        return false;
    drive_ready(ctx, req(past_end));
    auto eof = req(past_end).try_result();
    t.check(eof.readiness == RequestReadiness::ready && eof.result.has_value() &&
                eof.result.value() == 0,
            "a read entirely past the end reports EOF as zero");
    t.check(eof.effect == EffectReport{0, EffectCertainty::accounted},
            "EOF is an accounted zero effect");
    req(past_end).take_result();

    std::vector<std::byte> partial(5, std::byte{0});
    auto clipped = ctx.submit_read(ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write},
                                          partial.data(), partial.size(), content.size() - 4});
    t.check(clipped.has_value(), "the clipped read is accepted");
    if (!clipped.has_value())
        return false;
    drive_ready(ctx, req(clipped));
    auto short_read = req(clipped).try_result();
    t.check(short_read.readiness == RequestReadiness::ready && short_read.result.has_value() &&
                short_read.result.value() == 4,
            "a read crossing the end reports the short count");
    t.check(short_read.effect == EffectReport{4, EffectCertainty::accounted},
            "the short read reports its confirmed effect");

    req(clipped).take_result();

    t.check(core_is_idle(core.snapshot()), "the reads leave no core residue");
    t.check(backend_quiescent(*raw), "the backend retires every physical trace");
    return true;
}

bool write_sync_and_persistence_agree(Tracker& t) {
    Fixture fix = make_fixture(std::string(24, '\0'));
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));

    const std::string payload = "e1 write body\n";
    std::vector<std::byte> bytes(16);
    std::memcpy(bytes.data(), payload.data(), payload.size());
    auto written = ctx.submit_write(
        WriteOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, bytes.data(), bytes.size(), 0});
    t.check(written.has_value(), "the write is accepted");
    if (!written.has_value())
        return false;
    drive_ready(ctx, req(written));
    auto observed = req(written).take_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == 16,
            "the write reports the confirmed count");
    t.check(observed.effect == EffectReport{16, EffectCertainty::accounted},
            "a confirmed write reports the accounted effect");

    std::array<char, 16> reread{};
    const ssize_t n = ::pread(fix.fd, reread.data(), reread.size(), 0);
    t.check(n == 16 && std::memcmp(reread.data(), payload.data(), payload.size()) == 0,
            "the written bytes are physically present");

    auto sync_data = ctx.submit_sync_data(SyncDataOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}});
    auto sync_all = ctx.submit_sync_all(SyncAllOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}});
    t.check(sync_data.has_value() && sync_all.has_value(), "the sync operations are accepted");
    if (sync_data.has_value()) {
        drive_ready(ctx, req(sync_data));
        const auto sync_observed = req(sync_data).take_result();
        t.check(sync_observed.readiness == RequestReadiness::ready &&
                    sync_observed.result.has_value(),
                "sync_data completes successfully");
        t.check(sync_observed.effect == EffectReport{0, EffectCertainty::accounted},
                "a sync carries no data effect");
    }
    if (sync_all.has_value()) {
        drive_ready(ctx, req(sync_all));
        const auto sync_observed = req(sync_all).take_result();
        t.check(sync_observed.readiness == RequestReadiness::ready &&
                    sync_observed.result.has_value(),
                "sync_all completes successfully");
        t.check(sync_observed.effect == EffectReport{0, EffectCertainty::accounted},
                "a sync_all carries no data effect");
    }
    return true;
}

bool file_info_and_size_match_the_kernel_record(Tracker& t) {
    const std::string content = "sluice e1 metadata conformance";
    const std::string path = make_temp_file(content);
    if (path.empty()) {
        t.check(false, "fixture created");
        return false;
    }
    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    {
        auto opened = File::open(path);
        if (!opened.has_value()) {
            t.check(false, "fixture opens");
            return false;
        }
        File file = std::move(opened).value();
        auto info = ctx.submit_file_info(FileInfoOp{NativeFileRef{file}});
        t.check(info.has_value(), "file_info is accepted");
        if (!info.has_value())
            return false;
        drive_ready(ctx, req(info));
        const auto observed = req(info).take_result();
        t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value(),
                "file_info completes successfully");
        if (!observed.result.has_value())
            return false;
        const FileInfo& recorded = observed.result.value();
        struct ::stat truth {};
        t.check(::stat(path.c_str(), &truth) == 0, "the kernel record is readable");
        t.check(recorded.kind == sluice::FileKind::regular,
                "a regular file reports the regular kind");
        t.check(recorded.size == static_cast<std::uint64_t>(content.size()),
                "file_info reports the file length");
        t.check(recorded.identity.has_value() &&
                    recorded.identity->device == static_cast<std::uint64_t>(truth.st_dev) &&
                    recorded.identity->inode == static_cast<std::uint64_t>(truth.st_ino),
                "file_info reports the kernel device and inode identity");
        t.check(observed.effect == EffectReport{0, EffectCertainty::accounted},
                "file_info carries no data effect");

        auto sized = ctx.submit_size(SizeOp{NativeFileRef{file}});
        t.check(sized.has_value(), "size is accepted");
        if (!sized.has_value())
            return false;
        drive_ready(ctx, req(sized));
        const auto size_observed = req(sized).take_result();
        t.check(size_observed.readiness == RequestReadiness::ready &&
                    size_observed.result.has_value() &&
                    size_observed.result.value() == FileSize{content.size()},
                "size reports the file length through its own carrier");
        t.check(size_observed.effect == EffectReport{0, EffectCertainty::accounted},
                "size carries no data effect");
    }
    ::unlink(path.c_str());
    return true;
}

bool closed_file_rejects_every_operation_before_admission(Tracker& t) {
    const std::string path = make_temp_file("x");
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value()) {
        t.check(false, "fixture opens");
        return false;
    }
    File closed = std::move(opened).value();
    (void)closed.close();

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(4, std::byte{0});
    const NativeFileRef ref{closed};
    auto read = ctx.submit_read(ReadOp{ref, buffer.data(), buffer.size(), 0});
    auto write = ctx.submit_write(WriteOp{ref, buffer.data(), buffer.size(), 0});
    auto sync_data = ctx.submit_sync_data(SyncDataOp{ref});
    auto sync_all = ctx.submit_sync_all(SyncAllOp{ref});
    auto info = ctx.submit_file_info(FileInfoOp{ref});
    auto sized = ctx.submit_size(SizeOp{ref});
    t.check(!read.has_value() && read.error().code == IoError::Code::invalid_state,
            "read rejects a closed file before admission");
    t.check(!write.has_value() && write.error().code == IoError::Code::invalid_state,
            "write rejects a closed file before admission");
    t.check(!sync_data.has_value() && sync_data.error().code == IoError::Code::invalid_state,
            "sync_data rejects a closed file before admission");
    t.check(!sync_all.has_value() && sync_all.error().code == IoError::Code::invalid_state,
            "sync_all rejects a closed file before admission");
    t.check(!info.has_value() && info.error().code == IoError::Code::invalid_state,
            "file_info rejects a closed file before admission");
    t.check(!sized.has_value() && sized.error().code == IoError::Code::invalid_state,
            "size rejects a closed file before admission");
    t.check(core_is_idle(core.snapshot()), "the rejected submissions leave no core residue");
    return true;
}

bool semantic_rejections_precede_admission(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 rejection conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(4, std::byte{0});
    const NativeFileRef declared_read_only{fix.fd, sluice::FileAccess::read_only};
    auto write = ctx.submit_write(WriteOp{declared_read_only, buffer.data(), buffer.size(), 0});
    t.check(!write.has_value() && write.error().code == IoError::Code::invalid_argument,
            "a write against read-only access is rejected before admission");

    auto ranged = ctx.submit_read(ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write},
                                         buffer.data(), buffer.size(), kUnrepresentableOffset});
    t.check(!ranged.has_value() && ranged.error().code == IoError::Code::invalid_argument,
            "an unrepresentable read offset is rejected before admission");

    auto empty = ctx.submit_read(
        ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 0, 0});
    t.check(empty.has_value(), "a zero-length read is accepted as a zero-op");
    if (empty.has_value()) {
        drive_ready(ctx, req(empty));
        const auto observed = req(empty).take_result();
        t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                    observed.result.value() == 0,
                "a zero-length read completes with zero");
        t.check(observed.effect == EffectReport{0, EffectCertainty::accounted},
                "a zero-op effect is known zero");
    }
    // The zero-op defers its ready event to the next progress pass; the pass
    // also retires the delivery control and reclaims the slot.
    (void)ctx.poll();
    t.check(core_is_idle(core.snapshot()), "the rejections leave no core residue");
    return true;
}

bool setup_failures_are_explicit(Tracker& t) {
    bool threw = false;
#if !defined(SLUICE_E1_CONFORMANCE_URING)
    try {
        Backend broken(ThreadPoolConfig{0, 1});
        (void)broken;
    } catch (const std::exception&) {
        threw = true;
    }
    t.check(threw, "a zero-capacity profile refuses construction");
#endif

#if defined(SLUICE_E1_CONFORMANCE_URING)
    UringAsyncBackend::set_injected_statx_probe_failure(true);
    threw = false;
    try {
        Backend broken(UringConfig{4, 8});
        (void)broken;
    } catch (const std::exception&) {
        threw = true;
    }
    UringAsyncBackend::set_injected_statx_probe_failure(false);
    t.check(threw, "a missing metadata capability refuses construction at setup");

    UringAsyncBackend::set_injected_opcode_probe_failure(true);
    threw = false;
    try {
        Backend broken(UringConfig{4, 8});
        (void)broken;
    } catch (const std::exception&) {
        threw = true;
    }
    UringAsyncBackend::set_injected_opcode_probe_failure(false);
    t.check(threw, "a missing required request opcode refuses construction at setup");
#else
    threw = false;
    ThreadPoolBackend::set_injected_worker_spawn_failure_index(0);
    try {
        Backend broken(ThreadPoolConfig{4, 1});
        (void)broken;
    } catch (const std::exception&) {
        threw = true;
    }
    ThreadPoolBackend::set_injected_worker_spawn_failure_index(
        std::numeric_limits<std::size_t>::max());
    t.check(threw, "a worker spawn failure refuses construction at setup");
#endif

    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    t.check(ctx.outstanding() == 0, "a healthy profile still constructs after the failures");
    return true;
}

bool accepted_runtime_failure_converges_with_unknown_effect(Tracker& t) {
    const std::string path = make_temp_file("sluice e1 v15 conformance");
    if (path.empty()) {
        t.check(false, "fixture created");
        return false;
    }
    const int read_only_fd = ::open(path.c_str(), O_RDONLY);
    ::unlink(path.c_str());
    if (read_only_fd < 0) {
        t.check(false, "read-only fixture descriptor opens");
        return false;
    }
    auto backend = make_backend(4);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    const std::byte payload{std::byte{1}};
    // The declared access promises writability the description does not have:
    // the operation is admitted and must fail on the device, not in validation.
    auto written = ctx.submit_write(
        WriteOp{NativeFileRef{read_only_fd, sluice::FileAccess::read_write}, &payload, 1, 0});
    t.check(written.has_value(), "the over-declared write is admitted");
    if (!written.has_value()) {
        ::close(read_only_fd);
        return false;
    }
    drive_ready(ctx, req(written));
    const auto observed = req(written).take_result();
    t.check(observed.readiness == RequestReadiness::ready && !observed.result.has_value(),
            "the dispatched write fails on the device");
    t.check(observed.effect == EffectReport{0, EffectCertainty::unknown},
            "a failed dispatched write reports an unknown remainder, never a fabricated zero");
    t.check(core_is_idle(core.snapshot()), "the failed write leaves no core residue");
    ::close(read_only_fd);
    return true;
}

bool pre_execution_cancel_wins_with_known_zero_effect(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 pre-execution cancel conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    auto backend = make_backend(4);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    Backend::AcceptedPreDispatchPauseGate gate;
    raw->set_accepted_pre_dispatch_pause_gate(&gate);

    Completion<std::size_t> completion;
    std::vector<std::byte> buffer(8, std::byte{0});
    std::thread submitter{[&] {
        auto submitted = ctx.submit_read(
            ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 8, 0},
            completion);
        (void)submitted;
    }};
    wait_gate_paused(gate);

    // The parked submitter owns the context mutex, so the window is exercised
    // through the backend cancel seam every public cancel path maps onto.
    const auto key = raw->request_key_for_test(completion);
    t.check(key.has_value(), "the accepted request is bound while dispatch is parked");
    if (!key.has_value()) {
        gate.resume.store(true, std::memory_order_release);
        gate.resume.notify_all();
        submitter.join();
        raw->set_accepted_pre_dispatch_pause_gate(nullptr);
        return false;
    }
    const PublicCancel disposition = raw->cancel_key_for_test(*key);
    t.check(disposition == PublicCancel::won_before_execution,
            "the cancel wins inside the accepted pre-dispatch window");

    {
        GateGuard<Backend::AcceptedPreDispatchPauseGate> guard{gate};
    }
    submitter.join();
    raw->set_accepted_pre_dispatch_pause_gate(nullptr);

    while (!completion.ready())
        (void)ctx.poll();
    const Result<std::size_t> result = completion.result();
    t.check(!result.has_value() && result.error().code == IoError::Code::canceled,
            "the won cancel converges to a canceled terminal");
    const auto canceled_slot = core.observe_slot(key->slot);
    t.check(canceled_slot.has_value() && canceled_slot->terminal_chosen &&
                canceled_slot->outcome.effect ==
                    EffectReport{0, EffectCertainty::accounted},
            "the pre-dispatch cancel is a known-zero terminal at the core");
    completion.reset();
    t.check(core_is_idle(core.snapshot()), "the canceled slot reclaims completely");
    return true;
}

bool running_cancel_reports_the_backend_mechanism_fact(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 running cancel conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }

    std::vector<std::byte> buffer(8, std::byte{0});
#if !defined(SLUICE_E1_CONFORMANCE_URING)
    {
        auto backend = make_backend(4);
        Backend* raw = backend.get();
        AsyncIoContext ctx(std::move(backend));
        RequestCore& core = *ctx.context_core_for_test();

        Backend::WorkerClaimedPauseGate gate;
        raw->set_worker_claimed_pause_gate(&gate);

        auto submitted = ctx.submit_read(
            ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 8, 0});
        t.check(submitted.has_value(), "the running-window read is accepted");
        if (!submitted.has_value()) {
            raw->set_worker_claimed_pause_gate(nullptr);
            return false;
        }
        Request<std::size_t> request = std::move(submitted).value();

        wait_gate_paused(gate);
        const auto disposition = request.cancel();
        t.check(
            disposition.has_value() &&
                disposition.value() == CancelDisposition::physical_interruption_unsupported,
            "a claimed blocking syscall reports that no mechanism can interrupt it");
        {
            GateGuard<Backend::WorkerClaimedPauseGate> guard{gate};
        }
        raw->set_worker_claimed_pause_gate(nullptr);

        drive_ready(ctx, request);
        const auto observed = request.take_result();
        t.check(observed.readiness == RequestReadiness::ready,
                "the interrupted-or-completed request still converges to a terminal");
        t.check(core_is_idle(core.snapshot()), "the running cancel leaves no core residue");
    }
#else
    {
        // The real kernel may complete a tmpfs read before any cancel lands,
        // so the claimed window is held open with the deterministic submit
        // fiction: the ring consumes the SQE but no completion ever arrives
        // until the test injects it.
        UringBackendSubmitTestHooks hooks;
        hooks.submit = [](void*, ::io_uring* ring) noexcept {
            const unsigned ready = ::io_uring_sq_ready(ring);
            *ring->sq.ktail = ring->sq.sqe_tail;
            *ring->sq.khead = ring->sq.sqe_tail;
            return static_cast<int>(ready);
        };
        auto owned = std::make_unique<Backend>(UringConfig{4, 8}, hooks);
        Backend* raw = owned.get();
        AsyncIoContext ctx(std::move(owned));
        RequestCore& core = *ctx.context_core_for_test();

        auto submitted = ctx.submit_read(
            ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 8, 0});
        t.check(submitted.has_value(), "the running-window read is accepted");
        if (!submitted.has_value())
            return false;
        Request<std::size_t> request = std::move(submitted).value();

        const auto disposition = request.cancel();
        t.check(disposition.has_value() && disposition.value() == CancelDisposition::requested,
                "a claimed ring operation reports the issued cancel control");
        const auto cookie = raw->live_cookie_for_offset_for_test(0);
        t.check(cookie.has_value(), "the claimed operation holds a live cookie");
        if (!cookie.has_value())
            return false;
        (void)ctx.poll();
        raw->inject_cqe_for_test(kControlTag | *cookie, 0);
        raw->inject_cqe_for_test(*cookie, -ECANCELED);
        while (!request.ready())
            (void)ctx.poll();
        const auto observed = request.take_result();
        t.check(observed.readiness == RequestReadiness::ready && !observed.result.has_value() &&
                    observed.result.error().code == IoError::Code::canceled,
                "the cancel control converges the request to its canceled terminal");
        t.check(observed.effect == EffectReport{0, EffectCertainty::unknown},
                "a canceled in-flight attempt reports an unknown remainder");
        t.check(core_is_idle(core.snapshot()), "the running cancel leaves no core residue");
        t.check(backend_quiescent(*raw), "the ring holds no live cookies");
    }
#endif  // SLUICE_E1_CONFORMANCE_URING
    return true;
}

#if !defined(SLUICE_E1_CONFORMANCE_URING)

bool success_wins_over_a_recorded_cancel_intent(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 v14 success conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    auto backend = make_backend(4);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(8, std::byte{0});
    Backend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 8, 0});
    t.check(submitted.has_value(), "the raced read is accepted");
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted).value();

    wait_gate_paused(gate);
    const auto first = request.cancel();
    t.check(first.has_value() &&
                first.value() == CancelDisposition::physical_interruption_unsupported,
            "the intent is recorded while the worker owns the syscall");
    {
        GateGuard<Backend::WorkerClaimedPauseGate> guard{gate};
    }
    raw->set_worker_claimed_pause_gate(nullptr);

    drive_ready(ctx, request);
    const auto observed = request.try_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == 8,
            "the completed operation wins over the recorded cancel intent");
    t.check(observed.effect == EffectReport{8, EffectCertainty::accounted},
            "the success keeps its accounted effect");
    const auto late = request.cancel();
    t.check(late.has_value() && late.value() == CancelDisposition::already_terminal,
            "a cancel against a chosen terminal reports already_terminal");
    const auto request_id = request.id();
    const auto consumed = request.take_result();
    t.check(consumed.readiness == RequestReadiness::ready && consumed.result.has_value() &&
                consumed.result.value() == 8,
            "the request still carries the canonical success");
    const auto stale = ctx.cancel(request_id);
    t.check(stale.has_value() && stale.value() == CancelDisposition::not_found,
            "a cancel after consumption reports not_found");
    t.check(core_is_idle(core.snapshot()), "the raced request reclaims completely");
    return true;
}

bool unclaimed_dispatch_entry_retirement_publishes_a_wake(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 unclaimed retirement conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    auto backend = make_backend(4);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    Backend::AcceptedPreDispatchPauseGate dispatch_gate;
    Backend::WorkerUnclaimedRetirementPauseGate retirement_gate;
    raw->set_accepted_pre_dispatch_pause_gate(&dispatch_gate);
    raw->set_worker_unclaimed_retirement_pause_gate(&retirement_gate);

    Completion<std::size_t> completion;
    std::vector<std::byte> buffer(8, std::byte{0});
    std::thread submitter{[&] {
        auto submitted = ctx.submit_read(
            ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 8, 0},
            completion);
        (void)submitted;
    }};
    wait_gate_paused(dispatch_gate);

    const auto key = raw->request_key_for_test(completion);
    t.check(key.has_value(), "the accepted request is bound while dispatch is parked");
    if (!key.has_value()) {
        {
            GateGuard<Backend::AcceptedPreDispatchPauseGate> guard{dispatch_gate};
        }
        submitter.join();
        raw->set_accepted_pre_dispatch_pause_gate(nullptr);
        return false;
    }
    const auto before_cancel = ctx.progress_token_for_test();
    const PublicCancel disposition = raw->cancel_key_for_test(*key);
    t.check(disposition == PublicCancel::won_before_execution,
            "the pre-dispatch cancel wins while dispatch is parked");
    // Control: the same observer sees the wake the cancel path publishes before
    // it returns, so the observer is known to move for a published wake.
    const auto observed = ctx.progress_token_for_test();
    t.check(observed.progress != before_cancel.progress ||
                observed.progress_exhaustion != before_cancel.progress_exhaustion,
            "the cancel's own wake reaches the progress token");
    // The retirement of the entry the cancel could not remove — the submit path
    // adds it afterwards — is now the only transition left before the
    // observation below, so a silent retirement is visible as an unchanged
    // token.

    {
        GateGuard<Backend::AcceptedPreDispatchPauseGate> guard{dispatch_gate};
    }
    submitter.join();
    raw->set_accepted_pre_dispatch_pause_gate(nullptr);

    while (!completion.ready())
        (void)ctx.poll();

    wait_gate_paused(retirement_gate);
    const auto retired = ctx.progress_token_for_test();
    t.check(retired.progress != observed.progress ||
                retired.progress_exhaustion != observed.progress_exhaustion,
            "retiring an unclaimable dispatch entry publishes a progress wake");

    {
        GateGuard<Backend::WorkerUnclaimedRetirementPauseGate> guard{retirement_gate};
    }
    raw->set_worker_unclaimed_retirement_pause_gate(nullptr);
    completion.reset();
    t.check(raw->dispatch_size_for_test() == 0, "the retired entry leaves no dispatch residue");
    t.check(backend_quiescent(*raw), "the backend holds no physical residue");
    t.check(core_is_idle(core.snapshot()), "the canceled request reclaims completely");
    return true;
}

bool two_stale_entries_wake_without_premature_quiescence(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 two stale entries conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    auto backend = make_backend(4);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    Backend::AcceptedPreDispatchPauseGate dispatch_gate;
    Backend::WorkerUnclaimedRetirementPauseGate retirement_gate;
    raw->set_accepted_pre_dispatch_pause_gate(&dispatch_gate);
    raw->set_worker_unclaimed_retirement_pause_gate(&retirement_gate);

    Completion<std::size_t> first, second;
    std::vector<std::byte> buffer(8, std::byte{0});
    const ReadOp op{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 8, 0};

    std::thread submitter_one{[&] { (void)ctx.submit_read(op, first); }};
    wait_gate_paused(dispatch_gate);
    const auto first_key = raw->request_key_for_test(first);
    t.check(first_key.has_value(), "the first request is bound while dispatch is parked");
    if (!first_key.has_value()) {
        {
            GateGuard<Backend::AcceptedPreDispatchPauseGate> guard{dispatch_gate};
        }
        submitter_one.join();
        raw->set_accepted_pre_dispatch_pause_gate(nullptr);
        return false;
    }
    t.check(raw->cancel_key_for_test(*first_key) == PublicCancel::won_before_execution,
            "the first pre-dispatch cancel wins");

    // The gate latch is single-shot: release the first submitter, wait for it
    // to leave the gate, re-arm the latch, then stage the second submit so its
    // cancel lands after accept but before dispatch.
    {
        GateGuard<Backend::AcceptedPreDispatchPauseGate> guard{dispatch_gate};
    }
    submitter_one.join();
    dispatch_gate.resume.store(false, std::memory_order_release);
    std::thread submitter_two{[&] { (void)ctx.submit_read(op, second); }};
    while (dispatch_gate.exited.load(std::memory_order_acquire))
        std::this_thread::yield();
    const auto second_key = raw->request_key_for_test(second);
    t.check(second_key.has_value(), "the second request is bound while dispatch is parked");
    if (!second_key.has_value()) {
        {
            GateGuard<Backend::AcceptedPreDispatchPauseGate> guard{dispatch_gate};
        }
        submitter_two.join();
        raw->set_accepted_pre_dispatch_pause_gate(nullptr);
        return false;
    }
    t.check(raw->cancel_key_for_test(*second_key) == PublicCancel::won_before_execution,
            "the second pre-dispatch cancel wins");

    {
        GateGuard<Backend::AcceptedPreDispatchPauseGate> guard{dispatch_gate};
    }
    submitter_two.join();
    raw->set_accepted_pre_dispatch_pause_gate(nullptr);

    while (!first.ready() || !second.ready())
        (void)ctx.poll();

    // The retirement gate sits outside work_mtx_, so this read is a stable
    // snapshot: the worker has dropped the first entry and holds nothing.
    wait_gate_paused(retirement_gate);
    t.check(raw->dispatch_size_for_test() == 1,
            "the first retirement leaves the second entry pending");
    const auto before_last = ctx.progress_token_for_test();

    {
        GateGuard<Backend::WorkerUnclaimedRetirementPauseGate> guard{retirement_gate};
    }
    raw->set_worker_unclaimed_retirement_pause_gate(nullptr);

    // Only the second entry's retirement can move the token from here.
    const auto last_wake_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const auto now_token = ctx.progress_token_for_test();
        if (now_token.progress != before_last.progress ||
            now_token.progress_exhaustion != before_last.progress_exhaustion)
            break;
        if (std::chrono::steady_clock::now() > last_wake_by)
            break;
        std::this_thread::yield();
    }
    const auto after_last = ctx.progress_token_for_test();
    t.check(after_last.progress != before_last.progress ||
                after_last.progress_exhaustion != before_last.progress_exhaustion,
            "retiring the last unclaimable entry publishes a progress wake");

    while (raw->dispatch_size_for_test() != 0)
        std::this_thread::yield();

    first.reset();
    second.reset();
    t.check(raw->dispatch_size_for_test() == 0, "both retired entries leave no dispatch residue");
    t.check(backend_quiescent(*raw), "the backend holds no physical residue");
    t.check(core_is_idle(core.snapshot()), "both canceled requests reclaim completely");
    return true;
}

#endif  // !SLUICE_E1_CONFORMANCE_URING

bool post_accept_dispatch_failure_converges_and_reclaims(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 dispatch failure conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    auto backend = make_backend(4);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    Backend::DispatchFailureInjection injection;
    raw->set_dispatch_failure_injection(&injection);
    injection.armed.store(true, std::memory_order_release);

    std::vector<std::byte> buffer(8, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 8, 0});
    t.check(submitted.has_value(), "the to-be-failed read is accepted");
    if (!submitted.has_value()) {
        raw->set_dispatch_failure_injection(nullptr);
        return false;
    }
    Request<std::size_t> request = std::move(submitted).value();
    drive_ready(ctx, request);
    raw->set_dispatch_failure_injection(nullptr);

    const auto observed = request.take_result();
    t.check(observed.readiness == RequestReadiness::ready && !observed.result.has_value(),
            "the dispatch failure converges on the returned request");
    t.check(observed.effect == EffectReport{0, EffectCertainty::accounted},
            "a never-dispatched failure reports a known zero effect");
    t.check(core_is_idle(core.snapshot()), "the failed request reclaims completely");
    t.check(backend_quiescent(*raw), "the backend holds no physical residue");
    return true;
}

bool queue_saturation_reports_would_block_without_residue(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 saturation conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    auto backend = make_backend(1);
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(4, std::byte{0});
    auto first =
        ctx.submit_read(ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 4, 0});
    t.check(first.has_value(), "the first request occupies the single slot");
    if (!first.has_value())
        return false;
    auto second =
        ctx.submit_read(ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 4, 4});
    t.check(!second.has_value() && second.error().code == IoError::Code::would_block,
            "a saturated profile reports would_block instead of queueing invisibly");
    t.check(core.snapshot().accepted_live == 1,
            "the rejected submission did not disturb the accepted one");

    drive_ready(ctx, req(first));
    req(first).take_result();
    t.check(core_is_idle(core.snapshot()), "the single slot reclaims after consumption");

    auto retry =
        ctx.submit_read(ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 4, 8});
    t.check(retry.has_value(), "the profile admits again after the slot reclaims");
    if (retry.has_value()) {
        drive_ready(ctx, req(retry));
        req(retry).take_result();
    }
    return true;
}

bool terminal_consumption_retires_every_physical_trace(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 retirement conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    auto backend = make_backend(4);
    Backend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(6, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 6, 0});
    if (!submitted.has_value()) {
        t.check(false, "the read is accepted");
        return false;
    }
    Request<std::size_t> request = std::move(submitted).value();
    const auto id = request.id();
    drive_ready(ctx, request);
    const auto observed = request.try_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value(),
            "the read completes before retirement is checked");
    t.check(ctx.lookup(id) == RequestReadiness::ready,
            "the published identity still resolves before consumption");
    request.take_result();
    t.check(!request.valid(), "consumption retires the public responsibility");
    t.check(ctx.lookup(id) == RequestReadiness::empty, "the consumed identity stops resolving");
    t.check(ctx.outstanding() == 0, "the context reports no outstanding work");
    t.check(core_is_idle(core.snapshot()), "every core borrow is released");
    t.check(backend_quiescent(*raw), "the backend reports zero live cookies or workers");
    return true;
}

#if defined(SLUICE_E1_CONFORMANCE_URING)

struct AdvanceControl {
    std::atomic<bool> advance{true};
};

int controlled_submit(void* context, ::io_uring* ring) noexcept {
    auto* control = static_cast<AdvanceControl*>(context);
    if (!control->advance.load(std::memory_order_acquire)) {
        return 0;
    }
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    *ring->sq.khead = ring->sq.sqe_tail;
    return static_cast<int>(ready);
}

struct HookedBackend {
    std::unique_ptr<AsyncIoContext> ctx;
    Backend* backend = nullptr;
    std::shared_ptr<AdvanceControl> control = std::make_shared<AdvanceControl>();

    static HookedBackend create(std::size_t capacity, unsigned depth, bool advance) {
        HookedBackend made;
        UringBackendSubmitTestHooks hooks;
        hooks.submit = &controlled_submit;
        hooks.context = made.control.get();
        auto owned = std::make_unique<Backend>(UringConfig{capacity, depth}, hooks);
        made.backend = owned.get();
        made.control->advance.store(advance, std::memory_order_release);
        made.ctx = std::make_unique<AsyncIoContext>(std::move(owned));
        return made;
    }
};

struct SubmitFictionControl {
    std::atomic<int> mode{0};
    std::atomic<unsigned> consume{0};
};

int fiction_submit(void* context, ::io_uring* ring) noexcept {
    auto* control = static_cast<SubmitFictionControl*>(context);
    const int mode = control->mode.load(std::memory_order_acquire);
    if (mode == 1) {
        return 0;
    }
    if (mode == 2) {
        // The kernel's shared SQ head is a free-running consumed counter, not
        // a ring position; masking it here would alias a full ring to empty.
        *ring->sq.khead += control->consume.load(std::memory_order_acquire);
        return -EIO;
    }
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    *ring->sq.khead = ring->sq.sqe_tail;
    return static_cast<int>(ready);
}

struct FictionBackend {
    std::unique_ptr<AsyncIoContext> ctx;
    Backend* backend = nullptr;
    std::shared_ptr<SubmitFictionControl> control = std::make_shared<SubmitFictionControl>();

    static FictionBackend create(std::size_t capacity, unsigned depth) {
        FictionBackend made;
        UringBackendSubmitTestHooks hooks;
        hooks.submit = &fiction_submit;
        hooks.context = made.control.get();
        auto owned = std::make_unique<Backend>(UringConfig{capacity, depth}, hooks);
        made.backend = owned.get();
        made.ctx = std::make_unique<AsyncIoContext>(std::move(owned));
        return made;
    }
};

bool metadata_identity_agrees_across_backends(Tracker& t) {
    const std::string content = "sluice e1 cross-backend metadata";
    const std::string path = make_temp_file(content);
    if (path.empty()) {
        t.check(false, "fixture created");
        return false;
    }
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value()) {
        t.check(false, "fixture opens");
        return false;
    }
    File file = std::move(opened).value();

    AsyncIoContext tp(std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1}));
    AsyncIoContext ring(std::make_unique<Backend>(UringConfig{4, 8}));

    auto tp_info = tp.submit_file_info(FileInfoOp{NativeFileRef{file}});
    auto ring_info = ring.submit_file_info(FileInfoOp{NativeFileRef{file}});
    t.check(tp_info.has_value() && ring_info.has_value(), "both profiles accept file_info");
    if (!tp_info.has_value() || !ring_info.has_value())
        return false;
    drive_ready(tp, req(tp_info));
    drive_ready(ring, req(ring_info));
    const auto tp_observed = req(tp_info).take_result();
    const auto ring_observed = req(ring_info).take_result();
    t.check(tp_observed.result.has_value() && ring_observed.result.has_value(),
            "both profiles complete file_info");
    if (!tp_observed.result.has_value() || !ring_observed.result.has_value())
        return false;
    const FileInfo& tp_record = tp_observed.result.value();
    const FileInfo& ring_record = ring_observed.result.value();
    t.check(tp_record.kind == ring_record.kind && tp_record.size == ring_record.size &&
                tp_record.identity == ring_record.identity,
            "both profiles report the identical metadata record");

    auto tp_size = tp.submit_size(SizeOp{NativeFileRef{file}});
    auto ring_size = ring.submit_size(SizeOp{NativeFileRef{file}});
    t.check(tp_size.has_value() && ring_size.has_value(), "both profiles accept size");
    if (!tp_size.has_value() || !ring_size.has_value())
        return false;
    drive_ready(tp, req(tp_size));
    drive_ready(ring, req(ring_size));
    const auto tp_size_value = req(tp_size).take_result().result;
    const auto ring_size_value = req(ring_size).take_result().result;
    t.check(tp_size_value.has_value() && ring_size_value.has_value() &&
                tp_size_value.value() == ring_size_value.value(),
            "both profiles report the identical size");
    return true;
}

bool canceled_metadata_op_reports_the_canceled_terminal(Tracker& t) {
    const std::string content = "sluice e1 canceled metadata";
    const std::string path = make_temp_file(content);
    if (path.empty()) {
        t.check(false, "fixture created");
        return false;
    }
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value()) {
        t.check(false, "fixture opens");
        return false;
    }
    File file = std::move(opened).value();
    HookedBackend made = HookedBackend::create(4, 8, /*advance=*/true);
    RequestCore& core = *made.ctx->context_core_for_test();

    auto submitted = made.ctx->submit_file_info(FileInfoOp{NativeFileRef{file}});
    t.check(submitted.has_value(), "the to-be-canceled file_info is accepted");
    if (!submitted.has_value())
        return false;
    Request<FileInfo> request = std::move(submitted).value();

    const auto disposition = request.cancel();
    t.check(disposition.has_value() && disposition.value() == CancelDisposition::requested,
            "the cancel is requested against the claimed metadata op");
    const std::uint64_t cookie = made.backend->peek_next_cookie_for_test() - 1;
    (void)made.ctx->poll();
    made.backend->inject_cqe_for_test(kControlTag | cookie, 0);
    made.backend->inject_cqe_for_test(cookie, -ECANCELED);
    while (!request.ready())
        (void)made.ctx->poll();
    const auto observed = request.take_result();
    t.check(observed.readiness == RequestReadiness::ready && !observed.result.has_value() &&
                observed.result.error().code == IoError::Code::canceled,
            "a raced cancel of a metadata op reports the canceled terminal, not backend_error");
    t.check(observed.effect == EffectReport{0, EffectCertainty::accounted},
            "a canceled metadata op has no data effect");
    t.check(core_is_idle(core.snapshot()), "the canceled metadata op reclaims completely");
    return true;
}

bool sticky_cancel_intent_survives_sqe_exhaustion_and_is_serviced(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 sticky intent conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    HookedBackend made = HookedBackend::create(4, 1, /*advance=*/false);
    RequestCore& core = *made.ctx->context_core_for_test();

    std::vector<std::byte> buffer(8, std::byte{0});
    auto submitted = made.ctx->submit_read(
        ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 8, 0});
    t.check(submitted.has_value(), "the to-be-stranded read is accepted");
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted).value();

    const auto disposition = request.cancel();
    t.check(disposition.has_value() && disposition.value() == CancelDisposition::requested,
            "the cancel is reported as requested even when no SQE was available");
    const auto again = request.cancel();
    t.check(again.has_value() && again.value() == CancelDisposition::requested,
            "the sticky intent is never silently downgraded");

    made.control->advance.store(true, std::memory_order_release);
    (void)made.ctx->poll();
    (void)made.ctx->poll();
    const bool control_visible = made.backend->live_control_sqes_for_test() == 1;
    t.check(control_visible,
            "the serviced intent became a kernel-visible control exactly once");
    if (!control_visible)
        return false;
    const auto cookie = made.backend->live_cookie_for_offset_for_test(0);
    t.check(cookie.has_value(), "the stranded operation still holds a live cookie");
    if (!cookie.has_value())
        return false;
    made.backend->inject_cqe_for_test(kControlTag | *cookie, 0);
    made.backend->inject_cqe_for_test(*cookie, -ECANCELED);
    while (!request.ready())
        (void)made.ctx->poll();
    const auto observed = request.take_result();
    t.check(observed.readiness == RequestReadiness::ready && !observed.result.has_value() &&
                observed.result.error().code == IoError::Code::canceled,
            "the serviced control converges the request to its canceled terminal");
    t.check(observed.effect == EffectReport{0, EffectCertainty::unknown},
            "a canceled in-flight attempt reports an unknown remainder");
    t.check(core_is_idle(core.snapshot()), "the serviced cancel reclaims completely");
    t.check(backend_quiescent(*made.backend), "the ring holds no live cookies");
    return true;
}

bool success_and_cancel_converge_without_fabrication_under_the_fiction(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 uring v14 conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    HookedBackend made = HookedBackend::create(4, 8, /*advance=*/true);
    RequestCore& core = *made.ctx->context_core_for_test();

    std::vector<std::byte> buffer(8, std::byte{0});
    auto submitted = made.ctx->submit_read(
        ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 8, 0});
    t.check(submitted.has_value(), "the raced read is accepted");
    if (!submitted.has_value())
        return false;
    Request<std::size_t> request = std::move(submitted).value();

    const auto disposition = request.cancel();
    t.check(disposition.has_value() && disposition.value() == CancelDisposition::requested,
            "the cancel is requested against the claimed operation");
    const auto cookie = made.backend->live_cookie_for_offset_for_test(0);
    t.check(cookie.has_value(), "the raced operation holds a live cookie");
    if (!cookie.has_value())
        return false;
    (void)made.ctx->poll();

    made.backend->inject_cqe_for_test(*cookie, 8);
    made.backend->inject_cqe_for_test(kControlTag | *cookie, 0);
    while (!request.ready())
        (void)made.ctx->poll();
    const auto observed = request.try_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == 8,
            "the success CQE wins and is never rewritten into a cancel");
    t.check(observed.effect == EffectReport{8, EffectCertainty::accounted},
            "the winning success keeps its accounted effect");
    request.take_result();
    t.check(core_is_idle(core.snapshot()), "the raced request reclaims completely");
    return true;
}

bool hard_submit_failure_resolves_per_entry_kernel_visibility(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 partial submit visibility conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    FictionBackend made = FictionBackend::create(4, 8);
    RequestCore& core = *made.ctx->context_core_for_test();

    std::vector<std::byte> first(4, std::byte{0});
    std::vector<std::byte> second(4, std::byte{0});
    made.control->mode.store(1, std::memory_order_release);
    auto front = made.ctx->submit_read(
        ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, first.data(), 4, 0});
    auto back = made.ctx->submit_read(
        ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, second.data(), 4, 10});
    t.check(front.has_value() && back.has_value(), "both stalled reads are accepted");
    if (!front.has_value() || !back.has_value())
        return false;
    Request<std::size_t> visible = std::move(front).value();
    Request<std::size_t> invisible = std::move(back).value();
    t.check(made.backend->transport_ledger_size_for_test() == 2,
            "both prepared entries sit in the transport ledger");

    made.control->consume.store(1, std::memory_order_release);
    made.control->mode.store(2, std::memory_order_release);
    (void)made.ctx->poll();

    const auto after_poison = invisible.try_result();
    t.check(after_poison.readiness == RequestReadiness::ready && !after_poison.result.has_value() &&
                after_poison.result.error().code == IoError::Code::backend_error,
            "the provably invisible entry converges to the poison failure");
    invisible.take_result();
    t.check(visible.try_result().readiness == RequestReadiness::pending,
            "the kernel-visible entry keeps its borrow and is not settled by the poison");

    const auto cookie = made.backend->live_cookie_for_offset_for_test(0);
    t.check(cookie.has_value(), "the kernel-visible entry remains routable");
    if (!cookie.has_value())
        return false;
    made.backend->inject_cqe_for_test(*cookie, 4);
    while (!visible.ready())
        (void)made.ctx->poll();
    const auto observed = visible.take_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == 4,
            "the kernel-visible entry publishes its real outcome, never the poison error");
    t.check(observed.effect == EffectReport{4, EffectCertainty::accounted},
            "the real completion keeps its accounted effect");
    t.check(core_is_idle(core.snapshot()), "both resolutions leave no core residue");
    t.check(backend_quiescent(*made.backend), "the ring holds no live cookies after convergence");
    return true;
}

bool cancel_progress_wakes_a_parked_owner(Tracker& t) {
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        t.check(false, "pipe fixture created");
        return false;
    }
    auto owned_backend = make_backend(4);
    Backend* raw = owned_backend.get();
    AsyncIoContext ctx(std::move(owned_backend));
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::vector<std::byte> buffer(8, std::byte{0});
    auto submitted = ctx.submit_read(
        ReadOp{NativeFileRef{pipe_fds[0], sluice::FileAccess::read_only}, buffer.data(), 8, 0});
    t.check(submitted.has_value(), "the blocking pipe read is accepted");
    if (!submitted.has_value()) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return false;
    }
    Request<std::size_t> request = std::move(submitted).value();
    const auto id = request.id();

    AsyncIoContext::ProgressWaitOutcome::Kind owner_kind =
        AsyncIoContext::ProgressWaitOutcome::Kind::deadline_expired;
    std::thread owner{[&] {
        auto waited = ctx.wait_one(std::chrono::seconds(10));
        if (waited.has_value())
            owner_kind = waited.value().kind;
    }};

    const auto parked_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (prepark.load(std::memory_order_acquire) < 1) {
        if (std::chrono::steady_clock::now() > parked_by)
            break;
        std::this_thread::yield();
    }
    t.check(prepark.load(std::memory_order_acquire) >= 1, "the owner is confirmed parked");

    const auto disposition = ctx.cancel(id);
    t.check(disposition.has_value() && disposition.value() == CancelDisposition::requested,
            "the running cancel reports requested and owes its control obligation");

    owner.join();
    t.check(owner_kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress,
            "the parked owner wakes on the cancel obligation and services it");

    const auto converge_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!request.ready() && std::chrono::steady_clock::now() < converge_by)
        (void)ctx.poll();
    t.check(request.ready(), "the serviced control makes the cancel kernel-visible on the real ring");
    if (request.ready()) {
        const auto observed = request.take_result();
        t.check(observed.readiness == RequestReadiness::ready && !observed.result.has_value() &&
                    observed.result.error().code == IoError::Code::canceled,
                "the canceled in-flight read converges to its canceled terminal");
        t.check(observed.effect == EffectReport{0, EffectCertainty::unknown},
                "the canceled in-flight read reports an unknown remainder");
    }
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    t.check(ctx.outstanding() == 0, "the context holds no outstanding work");
    t.check(backend_quiescent(*raw), "the ring holds no live cookies or controls");
    return true;
}

bool full_ring_zero_consumed_hard_error_settles_every_entry(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 full-ring reconcile conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    FictionBackend made = FictionBackend::create(8, 8);
    RequestCore& core = *made.ctx->context_core_for_test();

    made.control->mode.store(1, std::memory_order_release);
    std::vector<std::byte> buffer(4, std::byte{0});
    std::vector<Request<std::size_t>> requests;
    bool all_accepted = true;
    for (unsigned offset = 0; offset < 8 && all_accepted; ++offset) {
        auto submitted = made.ctx->submit_read(
            ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 4, offset});
        all_accepted = submitted.has_value();
        if (all_accepted)
            requests.push_back(std::move(submitted).value());
    }
    t.check(all_accepted && requests.size() == 8, "a full ring of stalled reads is accepted");
    if (!all_accepted)
        return false;
    t.check(made.backend->transport_ledger_size_for_test() == 8,
            "the transport ledger holds the full ring");
    t.check(made.backend->sq_ready_for_test() == 8,
            "the kernel head has consumed none of the full ring");

    made.control->mode.store(2, std::memory_order_release);
    (void)made.ctx->poll();

    t.check(made.backend->transport_ledger_size_for_test() == 8,
            "a zero-consumed hard error retires nothing before poison classification");
    bool all_settled = false;
    for (int spin = 0; spin < 1000 && !all_settled; ++spin) {
        (void)made.ctx->poll();
        all_settled = true;
        for (auto& request : requests)
            all_settled = all_settled && request.ready();
    }
    t.check(all_settled, "every full-ring entry settles after the hard submit failure");
    if (!all_settled)
        return false;
    for (auto& request : requests) {
        const auto observed = request.take_result();
        t.check(observed.readiness == RequestReadiness::ready && !observed.result.has_value() &&
                    observed.result.error().code == IoError::Code::backend_error,
                "every proven-invisible entry converges to the poison failure");
        t.check(observed.effect == EffectReport{0, EffectCertainty::accounted},
                "a proven-invisible entry reports its known-zero effect, never an unknown remainder");
    }
    t.check(core_is_idle(core.snapshot()), "the full-ring poison reclaims every request");
    t.check(backend_quiescent(*made.backend), "the poisoned ring holds no live cookies");
    return true;
}

bool wrapped_head_partial_consumption_keeps_logical_cardinality(Tracker& t) {
    Fixture fix = make_fixture("sluice e1 wrapped-head reconcile conformance");
    if (!fix.ok()) {
        t.check(false, "fixture created");
        return false;
    }
    FictionBackend made = FictionBackend::create(4, 4);
    RequestCore& core = *made.ctx->context_core_for_test();

    std::vector<std::byte> buffer(4, std::byte{0});
    for (unsigned round = 0; round < 5; ++round) {
        auto submitted = made.ctx->submit_read(
            ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 4, 0});
        t.check(submitted.has_value(), "the wrapped-round read is accepted");
        if (!submitted.has_value())
            return false;
        Request<std::size_t> request = std::move(submitted).value();
        const auto cookie = made.backend->live_cookie_for_offset_for_test(0);
        t.check(cookie.has_value(), "the wrapped-round read holds a live cookie");
        if (!cookie.has_value())
            return false;
        made.backend->inject_cqe_for_test(*cookie, 4);
        while (!request.ready())
            (void)made.ctx->poll();
        const auto observed = request.take_result();
        t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                    observed.result.value() == 4,
                "the wrapped-round read completes through the fiction");
        if (!observed.result.has_value())
            return false;
    }

    made.control->mode.store(1, std::memory_order_release);
    auto front = made.ctx->submit_read(
        ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 4, 0});
    auto middle = made.ctx->submit_read(
        ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 4, 10});
    auto back = made.ctx->submit_read(
        ReadOp{NativeFileRef{fix.fd, sluice::FileAccess::read_write}, buffer.data(), 4, 20});
    t.check(front.has_value() && middle.has_value() && back.has_value(),
            "three stalled reads are accepted across the wrapped head");
    if (!front.has_value() || !middle.has_value() || !back.has_value())
        return false;
    Request<std::size_t> visible = std::move(front).value();
    Request<std::size_t> invisible_first = std::move(middle).value();
    Request<std::size_t> invisible_last = std::move(back).value();
    t.check(made.backend->transport_ledger_size_for_test() == 3 &&
                made.backend->sq_ready_for_test() == 3,
            "the wrapped physical positions hold three logically pending entries");

    made.control->consume.store(1, std::memory_order_release);
    made.control->mode.store(2, std::memory_order_release);
    (void)made.ctx->poll();

    const auto settled_first = invisible_first.take_result();
    t.check(settled_first.readiness == RequestReadiness::ready &&
                !settled_first.result.has_value() &&
                settled_first.result.error().code == IoError::Code::backend_error,
            "the invisible suffix settles through the poison across the wrap");
    const auto settled_last = invisible_last.take_result();
    t.check(settled_last.readiness == RequestReadiness::ready &&
                !settled_last.result.has_value() &&
                settled_last.result.error().code == IoError::Code::backend_error,
            "the invisible tail settles through the poison across the wrap");
    t.check(visible.try_result().readiness == RequestReadiness::pending,
            "the consumed prefix keeps its borrow across the wrapped head");
    t.check(made.backend->transport_ledger_size_for_test() == 2,
            "the wrap leaves exactly the unconsumed suffix in the ledger");

    const auto cookie = made.backend->live_cookie_for_offset_for_test(0);
    t.check(cookie.has_value(), "the consumed prefix remains routable across the wrap");
    if (!cookie.has_value())
        return false;
    made.backend->inject_cqe_for_test(*cookie, 4);
    while (!visible.ready())
        (void)made.ctx->poll();
    const auto observed = visible.take_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == 4,
            "the consumed prefix publishes its real outcome across the wrap");
    t.check(observed.effect == EffectReport{4, EffectCertainty::accounted},
            "the consumed prefix keeps its accounted effect across the wrap");
    t.check(core_is_idle(core.snapshot()), "the wrapped-head resolutions leave no core residue");
    t.check(backend_quiescent(*made.backend), "the ring holds no live cookies after the wrap");
    return true;
}

#endif  // SLUICE_E1_CONFORMANCE_URING

}  // namespace

int main() {
    ::setvbuf(stdout, nullptr, _IONBF, 0);
    ::alarm(180);
    struct NamedTest {
        const char* name;
        bool (*fn)(Tracker&);
    };
    const NamedTest tests[] = {
        {"read_reports_confirmed_counts_and_effect", read_reports_confirmed_counts_and_effect},
        {"write_sync_and_persistence_agree", write_sync_and_persistence_agree},
        {"file_info_and_size_match_the_kernel_record",
         file_info_and_size_match_the_kernel_record},
        {"closed_file_rejects_every_operation_before_admission",
         closed_file_rejects_every_operation_before_admission},
        {"semantic_rejections_precede_admission", semantic_rejections_precede_admission},
        {"setup_failures_are_explicit", setup_failures_are_explicit},
        {"accepted_runtime_failure_converges_with_unknown_effect",
         accepted_runtime_failure_converges_with_unknown_effect},
        {"pre_execution_cancel_wins_with_known_zero_effect",
         pre_execution_cancel_wins_with_known_zero_effect},
        {"running_cancel_reports_the_backend_mechanism_fact",
         running_cancel_reports_the_backend_mechanism_fact},
#if !defined(SLUICE_E1_CONFORMANCE_URING)
        {"success_wins_over_a_recorded_cancel_intent",
         success_wins_over_a_recorded_cancel_intent},
        {"unclaimed_dispatch_entry_retirement_publishes_a_wake",
         unclaimed_dispatch_entry_retirement_publishes_a_wake},
        {"two_stale_entries_wake_without_premature_quiescence",
         two_stale_entries_wake_without_premature_quiescence},
#else
        {"metadata_identity_agrees_across_backends", metadata_identity_agrees_across_backends},
        {"canceled_metadata_op_reports_the_canceled_terminal",
         canceled_metadata_op_reports_the_canceled_terminal},
        {"sticky_cancel_intent_survives_sqe_exhaustion_and_is_serviced",
         sticky_cancel_intent_survives_sqe_exhaustion_and_is_serviced},
        {"success_and_cancel_converge_without_fabrication_under_the_fiction",
         success_and_cancel_converge_without_fabrication_under_the_fiction},
        {"hard_submit_failure_resolves_per_entry_kernel_visibility",
         hard_submit_failure_resolves_per_entry_kernel_visibility},
        {"full_ring_zero_consumed_hard_error_settles_every_entry",
         full_ring_zero_consumed_hard_error_settles_every_entry},
        {"wrapped_head_partial_consumption_keeps_logical_cardinality",
         wrapped_head_partial_consumption_keeps_logical_cardinality},
        {"cancel_progress_wakes_a_parked_owner", cancel_progress_wakes_a_parked_owner},
#endif
        {"post_accept_dispatch_failure_converges_and_reclaims",
         post_accept_dispatch_failure_converges_and_reclaims},
        {"queue_saturation_reports_would_block_without_residue",
         queue_saturation_reports_would_block_without_residue},
        {"terminal_consumption_retires_every_physical_trace",
         terminal_consumption_retires_every_physical_trace},
    };

    int failures = 0;
    for (const NamedTest& test : tests) {
        Tracker tracker{test.name};
        test.fn(tracker);
        if (tracker.failures == 0) {
            std::printf("ok %s\n", test.name);
        } else {
            failures += tracker.failures;
        }
    }
    if (failures == 0) {
#if defined(SLUICE_E1_CONFORMANCE_URING)
        std::printf("all %zu e1 io_uring conformance tests passed\n",
                    sizeof(tests) / sizeof(tests[0]));
#else
        std::printf("all %zu e1 ThreadPool conformance tests passed\n",
                    sizeof(tests) / sizeof(tests[0]));
#endif
    } else {
        std::printf("%d e1 conformance failures\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
