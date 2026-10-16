#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <new>
#include <thread>
#include <unistd.h>
#include <vector>

// The sanitizer runtimes define the replaceable allocation functions
// themselves; the counting oracle therefore runs only on unsanitized builds
// and case 0 verifies the counter is live before relying on it.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define PB_ALLOC_COUNTER_DISABLED 1
#endif
#elif defined(__SANITIZE_ADDRESS__)
#define PB_ALLOC_COUNTER_DISABLED 1
#endif

namespace {

using namespace sluice::async;

std::atomic<unsigned long long> g_allocation_events{0};

} // namespace

#if !defined(PB_ALLOC_COUNTER_DISABLED)

void* allocate_counted(std::size_t n) {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr)
        throw std::bad_alloc();
    g_allocation_events.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void* operator new(std::size_t n) {
    return allocate_counted(n);
}

void* operator new[](std::size_t n) {
    return allocate_counted(n);
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

// Over-aligned allocations take the aligned overloads, not the plain ones;
// counting only the plain set would miss a container of over-aligned elements.
void* allocate_counted_aligned(std::size_t n, std::size_t alignment) {
    const std::size_t rounded =
        alignment == 0 ? n : ((n + alignment - 1) / alignment) * alignment;
    void* p = std::aligned_alloc(alignment == 0 ? 1 : alignment,
                                 rounded == 0 ? (alignment == 0 ? 1 : alignment) : rounded);
    if (p == nullptr)
        throw std::bad_alloc();
    g_allocation_events.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void* operator new(std::size_t n, std::align_val_t alignment) {
    return allocate_counted_aligned(n, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t n, std::align_val_t alignment) {
    return allocate_counted_aligned(n, static_cast<std::size_t>(alignment));
}

void operator delete(void* p, std::align_val_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::align_val_t) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}

#endif // !PB_ALLOC_COUNTER_DISABLED

namespace {

struct TmpFile {
    ::FILE* file = std::tmpfile();
    ~TmpFile() {
        if (file != nullptr)
            std::fclose(file);
    }
    int fd() const { return ::fileno(file); }
};

AsyncIoContext make_pool_context(std::size_t capacity, std::size_t workers,
                                 ThreadPoolBackend** raw_out) {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{capacity, workers});
    *raw_out = backend.get();
    return AsyncIoContext(std::move(backend));
}

bool fail_at(const char* case_name, const char* what, long long got = -1, long long want = -1) {
    if (got < 0 && want < 0)
        std::fprintf(stderr, "%s: %s\n", case_name, what);
    else
        std::fprintf(stderr, "%s: %s (got %lld, want %lld)\n", case_name, what, got, want);
    return false;
}

bool bounded_spin(const std::function<bool()>& ready) {
    for (int i = 0; i < 20000; ++i) {
        if (ready())
            return true;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return ready();
}

Request<std::size_t> submit_zero_op(AsyncIoContext& ctx, int fd, std::byte* buffer) {
    auto submitted = ctx.submit_read(
        ReadOp{NativeFileRef(fd, sluice::FileAccess::read_only), buffer, 0, 0});
    if (!submitted.has_value())
        return {};
    return std::move(submitted).value();
}

// One pass that publishes a queued physical completion and delivers queued
// zero-op owed events must run on storage the backend already owns: an
// allocation inside the pass can throw after accepted work left the
// publication queue and would strand its settlement.
bool progress_pass_performs_no_dynamic_allocation() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(8, 1, &raw);
    TmpFile file;
    if (file.file == nullptr)
        return false;
    std::vector<std::byte> buffer(8, std::byte{0});

    auto read_req = ctx.submit_read(
        ReadOp{NativeFileRef(file.fd(), sluice::FileAccess::read_only), buffer.data(), 4, 0});
    if (!read_req.has_value())
        return false;
    if (!bounded_spin(
            [&] { return raw->publication_pending_size_for_test() >= 1; }))
        return false;


    std::vector<Request<std::size_t>> zero_ops;
    for (int n = 0; n < 3; ++n) {
        auto r = submit_zero_op(ctx, file.fd(), buffer.data());
        if (!r.valid())
            return false;
        zero_ops.push_back(std::move(r));
    }

    const auto counter_probe = [] {
        const unsigned long long before = g_allocation_events.load(std::memory_order_relaxed);
        std::vector<int> probe(4, 0);
        (void)probe;
        return g_allocation_events.load(std::memory_order_relaxed) != before;
    };
#if defined(PB_ALLOC_COUNTER_DISABLED)
    std::fprintf(stderr,
                 "case0: allocation counter disabled on this runtime (the sanitizer runtime "
                 "defines the replaceable operators); allocation invariant not exercised\n");
#else
    if (!counter_probe())
        return fail_at("case0", "the allocation counter is inert although it is compiled in");
#endif

    const unsigned long long before = g_allocation_events.load(std::memory_order_relaxed);
    const auto pass = ctx.poll_progress();
    const unsigned long long after = g_allocation_events.load(std::memory_order_relaxed);

    if (!pass.has_value())
        return fail_at("case0", "progress pass reported an error");
    if (pass.value().completed != 4)
        return fail_at("case0", "pass completed the wrong number of events",
                       static_cast<long long>(pass.value().completed), 4);
    if (after != before)
        return fail_at("case0", "pass allocated after accepted work was published",
                       static_cast<long long>(after - before), 0);
    if (!read_req.value().ready())
        return fail_at("case0", "the published request is not ready after the pass");
    for (const auto& r : zero_ops) {
        if (!r.ready())
            return fail_at("case0", "a seeded zero-op request is not ready after the pass");
    }
    if (raw->sink_deliveries() != 4)
        return fail_at("case0", "delivery count mismatch",
                       static_cast<long long>(raw->sink_deliveries()), 4);
    read_req.value().discard();
    for (auto& r : zero_ops)
        r.discard();
    if (raw->outstanding() != 0 || !raw->internal_work_retired())
        return fail_at("case0", "the pass left accepted work unsettled");
    return true;
}

// A pass must deliver exactly its entry snapshot and return: completion
// events produced while it delivers belong to later passes. Each delivery
// here triggers one further zero-op submission that posts its owed event
// before the delivery returns, so a pass that rescans after delivering
// always finds fresh work and never terminates this way.
struct Rendezvous {
    std::mutex mtx;
    std::condition_variable cv;
    bool request_pending = false;
    bool done = true;
    bool stop = false;
    int produced = 0;
};

struct ProducingSink : detail::SynchronousReadySink {
    Rendezvous& rv;
    int limit;
    AsyncIoContext& ctx;
    int fd;
    std::byte* buffer;
    std::vector<Request<std::size_t>> produced_requests;

    ProducingSink(Rendezvous& r, int limit_, AsyncIoContext& c, int fd_, std::byte* b)
        : rv(r), limit(limit_), ctx(c), fd(fd_), buffer(b) {}

    void on_ready(detail::ReadyEvent) noexcept override {
        std::unique_lock<std::mutex> lk(rv.mtx);
        if (rv.produced >= limit)
            return;
        rv.request_pending = true;
        rv.done = false;
        rv.cv.notify_all();
        rv.cv.wait(lk, [&] { return rv.done; });
    }
};

bool pass_delivers_exactly_the_entry_snapshot() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(64, 2, &raw);
    TmpFile file;
    if (file.file == nullptr)
        return false;
    std::vector<std::byte> buffer(4, std::byte{0});

    constexpr int kSeed = 8;
    constexpr int kProduced = 8;

    Rendezvous rv;
    ProducingSink sink(rv, kProduced, ctx, file.fd(), buffer.data());
    ctx.set_ready_sink(&sink);

    std::vector<Request<std::size_t>> seeded;
    for (int n = 0; n < kSeed; ++n) {
        auto r = submit_zero_op(ctx, file.fd(), buffer.data());
        if (!r.valid())
            return false;
        seeded.push_back(std::move(r));
    }

    std::thread producer([&] {
        std::unique_lock<std::mutex> lk(rv.mtx);
        for (;;) {
            rv.cv.wait(lk, [&] { return rv.request_pending || rv.stop; });
            if (rv.stop && !rv.request_pending)
                return;
            rv.request_pending = false;
            auto r = submit_zero_op(ctx, sink.fd, sink.buffer);
            if (r.valid())
                sink.produced_requests.push_back(std::move(r));
            rv.produced += 1;
            rv.done = true;
            rv.cv.notify_all();
        }
    });

    const auto first = ctx.poll_progress();
    if (!first.has_value())
        return fail_at("case1", "progress pass reported an error");
    // The snapshot oracle: the pass delivers the eight seeded events, not
    // the eight events its own deliveries created, and reports the residue
    // as immediate work instead of absorbing it.
    const bool first_ok =
        first.value().completed == static_cast<std::size_t>(kSeed) &&
        first.value().immediate_work_remains;
    {
        std::lock_guard<std::mutex> lk(rv.mtx);
        rv.stop = true;
        rv.cv.notify_all();
    }
    producer.join();
    if (first.value().completed != static_cast<std::size_t>(kSeed))
        return fail_at("case1", "first pass absorbed events produced during its own deliveries",
                       static_cast<long long>(first.value().completed), kSeed);
    if (!first_ok)
        return fail_at("case1", "first pass reported no residual work after the entry snapshot");

    std::size_t total = first.value().completed;
    for (int n = 0; n < 16; ++n) {
        const auto pass = ctx.poll_progress();
        if (!pass.has_value())
            return fail_at("case1", "follow-up progress pass reported an error");
        total += pass.value().completed;
        if (pass.value().completed == 0 && !pass.value().immediate_work_remains &&
            !pass.value().accepted_work_remains)
            break;
    }
    if (total != static_cast<std::size_t>(kSeed + kProduced))
        return fail_at("case1", "follow-up passes did not deliver the deferred events",
                       static_cast<long long>(total), kSeed + kProduced);
    for (const auto& r : seeded) {
        if (!r.ready())
            return fail_at("case1", "a seeded request is not ready after the passes");
    }
    for (const auto& r : sink.produced_requests) {
        if (!r.ready())
            return fail_at("case1", "a produced request is not ready after the passes");
    }
    if (raw->outstanding() != 0 || !raw->internal_work_retired())
        return fail_at("case1", "the passes left accepted work unsettled");

    for (auto& r : seeded)
        r.discard();
    for (auto& r : sink.produced_requests)
        r.discard();
    if (raw->outstanding() != 0)
        return fail_at("case1", "a discarded request left the backend occupied");
    return true;
}

} // namespace

int main() {
    using Fn = bool (*)();
    const char* names[] = {
        "progress_pass_performs_no_dynamic_allocation",
        "pass_delivers_exactly_the_entry_snapshot",
    };
    const Fn cases[] = {
        progress_pass_performs_no_dynamic_allocation,
        pass_delivers_exactly_the_entry_snapshot,
    };
    bool all_ok = true;
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!cases[i]()) {
            std::fprintf(stderr, "threadpool_progress_pass_bound_test: case %zu (%s) failed\n",
                         i, names[i]);
            all_ok = false;
        }
    }
    return all_ok ? 0 : 1;
}
