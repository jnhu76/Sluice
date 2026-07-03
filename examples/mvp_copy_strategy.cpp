// mvp_copy_strategy: demonstrates the CopyStrategy layer (CPPIO-CORE-007).
// Runs five scenarios against the same data, printing the decision + path bytes
// for each. NOT a performance claim — it only shows the strategy boundary is
// explicit and observable.
#include <sluice/buffer.hpp>
#include <sluice/copy.hpp>
#include <sluice/copy_strategy.hpp>
#include <sluice/fault.hpp>
#include <sluice/limit.hpp>
#include <sluice/measurement.hpp>

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

class StrReader final : public sluice::Reader {
public:
    sluice::MemoryReader mem;
    explicit StrReader(std::string_view s) : mem(sluice::MemoryReader::from_string(s)) {}
    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        return mem.read_some(dst);
    }
};

void run(const char* label, StrReader& reader, sluice::BufferedReader* buffered,
         sluice::Writer& writer, std::span<std::byte> scratch, sluice::CopyOptions opts) {
    sluice::CopyStats st;
    sluice::CopyDecision dec;
    sluice::Reader& src = buffered ? static_cast<sluice::Reader&>(*buffered)
                                  : static_cast<sluice::Reader&>(reader);
    auto res = sluice::copy_all(src, writer, scratch, opts, &st, &dec);
    std::printf("=== %s ===\n", label);
    if (!res.has_value()) {
        std::printf("  result=error(%s)\n", sluice::to_string(res.error().code).data());
    } else {
        std::printf("  copied_bytes=%llu\n", static_cast<unsigned long long>(res.value()));
    }
    std::printf("  requested_strategy=%s\n", sluice::to_string(dec.requested).data());
    std::printf("  selected_strategy=%s\n", sluice::to_string(dec.selected).data());
    std::printf("  reason=%s\n", std::string(dec.reason).c_str());
    std::printf("  unsupported_requested=%d\n", dec.unsupported_requested ? 1 : 0);
    std::printf("  buffered_fast_path_bytes=%llu\n",
                static_cast<unsigned long long>(st.buffered_fast_path_bytes));
    std::printf("  scratch_path_bytes=%llu\n",
                static_cast<unsigned long long>(st.scratch_path_bytes));
}

}  // namespace

int main() {
    const std::string_view data = "0123456789ABCDEF";
    std::vector<std::byte> scratch(8);

    // 1. Scratch strategy
    {
        StrReader r(data);
        sluice::MemoryWriter w;
        sluice::CopyOptions o; o.strategy = sluice::CopyStrategy::Scratch;
        run("Scratch", r, nullptr, w, scratch, o);
    }
    // 2. BufferedFirst strategy (prime a BufferedReader so fast path engages)
    {
        StrReader r(data);
        std::vector<std::byte> rbuf(64);
        sluice::BufferedReader br(r, rbuf);
        std::vector<std::byte> primed(4);
        (void)br.read_some(primed);  // leave 12 buffered
        sluice::MemoryWriter w;
        sluice::CopyOptions o; o.strategy = sluice::CopyStrategy::BufferedFirst;
        run("BufferedFirst", r, &br, w, scratch, o);
    }
    // 3. Auto strategy (same setup; Auto resolves to BufferedFirst)
    {
        StrReader r(data);
        std::vector<std::byte> rbuf(64);
        sluice::BufferedReader br(r, rbuf);
        std::vector<std::byte> primed(4);
        (void)br.read_some(primed);
        sluice::MemoryWriter w;
        sluice::CopyOptions o; o.strategy = sluice::CopyStrategy::Auto;
        run("Auto", r, &br, w, scratch, o);
    }
    // 4. Deferred strategy rejected (default policy)
    {
        StrReader r(data);
        sluice::MemoryWriter w;
        sluice::CopyOptions o; o.strategy = sluice::CopyStrategy::VectorDeferred;
        run("VectorDeferred (rejected)", r, nullptr, w, scratch, o);
    }
    // 5. Deferred strategy fallback to Auto
    {
        StrReader r(data);
        std::vector<std::byte> rbuf(64);
        sluice::BufferedReader br(r, rbuf);
        std::vector<std::byte> primed(4);
        (void)br.read_some(primed);
        sluice::MemoryWriter w;
        sluice::CopyOptions o;
        o.strategy = sluice::CopyStrategy::SpliceDeferred;
        o.unsupported_policy = sluice::UnsupportedStrategyPolicy::FallbackToAuto;
        run("SpliceDeferred (fallback)", r, &br, w, scratch, o);
    }
    return 0;
}
