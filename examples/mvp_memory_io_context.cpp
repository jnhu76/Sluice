// mvp_memory_io_context: the fastest way to see cppio work end-to-end with NO
// filesystem. Seeds an in-memory reader, copies through an in-memory writer via
// the IoContext boundary, prints the result. Useful as a 5-minute tour and for
// tests that want determinism without temp files. Not a performance claim.
//
// Shows CPPIO-CORE-015 ergonomics: MemoryIoContext (015C) + from_bytes (015B).
#include <cppio/copy.hpp>
#include <cppio/limit.hpp>
#include <cppio/memory_io_context.hpp>

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::vector<std::byte> to_bytes(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}
}  // namespace

int main() {
    cppio::MemoryIoContext ctx;
    // Seed a readable "file" entirely in memory.
    ctx.seed("input.txt", to_bytes("hello from the in-memory backend"));

    auto r = ctx.open_reader("input.txt");
    if (!r.has_value()) {
        std::fprintf(stderr, "open_reader failed\n");
        return 1;
    }
    auto w = ctx.open_writer("output.txt");
    if (!w.has_value()) {
        std::fprintf(stderr, "open_writer failed\n");
        return 1;
    }

    // copy_all works through the abstract Reader*/Writer* handles exactly as it
    // would for the blocking file backend — that is the point of IoContext.
    std::vector<std::byte> scratch(64);
    auto res = cppio::copy_all(*r.value(), *w.value(),
                               std::span<std::byte>(scratch),
                               cppio::CopyLimit::unlimited());
    if (!res.has_value()) {
        std::fprintf(stderr, "copy failed\n");
        return 1;
    }
    (void)w.value()->flush();

    // The MemoryWriter sink holds the copied bytes; inspect it for the demo.
    auto* mw = dynamic_cast<cppio::MemoryWriter*>(w.value().get());
    if (mw == nullptr) {
        std::fprintf(stderr, "writer was not a MemoryWriter\n");
        return 1;
    }
    std::string copied(reinterpret_cast<const char*>(mw->bytes().data()),
                       mw->bytes().size());

    std::printf("mvp_memory_io_context: copied %llu bytes via IoContext\n",
                static_cast<unsigned long long>(res.value()));
    std::printf("  bytes: \"%s\"\n", copied.c_str());
    std::printf("  (deterministic in-memory backend; no filesystem; no perf claim)\n");
    return 0;
}
