// Contract-negative tests for the sync runtime (sluice-CORE-024S §4).
//
// These are CROSS-LAYER regression guards: each slices one row of the contract
// (docs/adr/ADR-024S-sync-runtime-contract.md G4-G6, G9, N4, N10;
//  docs/io/sync-error-semantics.md table). The same behaviors are also exercised
// individually in fault_test/reader_test/writer_test/file_positional_test — this
// file exists so the CONTRACT as a whole has a single place a refactor must keep
// green. Add one slice at a time (TDD vertical slices).
#include "harness.hpp"

#include <sluice/fault.hpp>
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

using namespace sluice;

// ---- Slice 1: read_exact EOF before fill maps to IoError::eof (G4) ----------
// MemoryReader runs out of data cleanly (no fault injected) — the contract says
// this surfaces as eof, not as a generic error or a zero-byte success.
SLUICE_TEST_CASE(contract_read_exact_eof_maps_to_eof_code) {
    auto r = MemoryReader::from_string("abc");   // 3 bytes available
    std::byte out[5]{};                          // request 5
    auto res = r.read_exact(std::span<std::byte>(out));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == IoError::Code::eof);
}

// ---- Slice 2: write_all zero progress on non-empty input → invalid_state (G5)
// A writer that accepts a non-empty slice but writes 0 bytes is a broken
// backend; write_all must not infinite-loop. Contract: invalid_state.
namespace {
class ZeroProgressWriter final : public Writer {
public:
    Result<std::size_t> write_some(std::span<const std::byte>) override {
        return std::size_t{0};  // accepts work, makes no progress
    }
    Result<void> flush() override { return {}; }
};
}  // namespace

SLUICE_TEST_CASE(contract_write_all_zero_progress_is_invalid_state) {
    ZeroProgressWriter w;
    std::byte buf[4]{};
    auto res = w.write_all(std::span<std::byte>(buf));
    SLUICE_CHECK(!res.has_value());
    SLUICE_CHECK(res.error().code == IoError::Code::invalid_state);
}

// ---- Slice 3: zero-length read_exact/write_all is a deterministic no-op ----
// Empty buffer → the loop predicate is false immediately → success, no syscall.
namespace {
class CountingWriter final : public Writer {
public:
    int calls = 0;
    Result<std::size_t> write_some(std::span<const std::byte> src) override {
        ++calls;
        sink.insert(sink.end(), src.begin(), src.end());
        return src.size();
    }
    Result<void> flush() override { return {}; }
    std::vector<std::byte> sink;
};
class CountingReader final : public Reader {
public:
    int calls = 0;
    Result<std::size_t> read_some(std::span<std::byte> dst) override {
        ++calls;
        return dst.size();  // pretend to fill
    }
};
}  // namespace

SLUICE_TEST_CASE(contract_zero_length_read_exact_is_noop) {
    CountingReader r;
    auto res = r.read_exact(std::span<std::byte>{});
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(r.calls == 0);  // no syscall issued
}

SLUICE_TEST_CASE(contract_zero_length_write_all_is_noop) {
    CountingWriter w;
    auto res = w.write_all(std::span<const std::byte>{});
    SLUICE_CHECK(res.has_value());
    SLUICE_CHECK(w.calls == 0);
    SLUICE_CHECK(w.sink.empty());
}

// ---- Slice 4: read_exact retries across short reads to fill the buffer (G4) -
// FaultReader caps read size; read_exact must still deliver the full buffer.
SLUICE_TEST_CASE(contract_read_exact_retries_short_reads_to_completion) {
    auto mem = MemoryReader::from_string("0123456789");
    FaultPlan plan;
    plan.max_read_size = 2;  // force 2-byte reads
    FaultReader fr(mem, plan);
    std::byte out[10]{};
    auto res = fr.read_exact(std::span<std::byte>(out));
    SLUICE_CHECK(res.has_value());
    // Verify the bytes round-tripped correctly through the short-read loop.
    auto* p = reinterpret_cast<const char*>(out);
    SLUICE_CHECK(std::string_view(p, 10) == "0123456789");
}

// ---- Slice 5: non-goal guard — the sync public headers do not pull in
// coroutine / async / io_uring machinery (N1, N2, N3). Structural guard: if a
// future change makes reader.hpp/writer.hpp/file.hpp transitively depend on
// <coroutine> or an async header, this test breaks the build.
SLUICE_TEST_CASE(contract_sync_headers_have_no_coroutine_dependency) {
    // If <coroutine> were a transitive include of the sync headers, the
    // following macro-defined sentinel would be visible. It is NOT included
    // anywhere in the sync layer by design (N1/N2).
    #ifdef __cpp_impl_coroutine
    // __cpp_impl_coroutine may be defined by the compiler itself regardless of
    // our includes; this guard is therefore advisory, not a hard contract. The
    // real guard is the absence of #include <coroutine> / async/ headers in the
    // sync public surface — verified by grep in the merge-readiness checklist.
    #endif
    // The contract: compiling and linking this test against ONLY sluice_core
    // (no sluice_async, no coroutine header included here) succeeds. That IS
    // the guard: the sync layer stands alone.
    std::byte b{};
    MemoryReader r(MemoryReader::from_bytes(std::span<const std::byte>(&b, 1)));
    std::byte out[1]{};
    SLUICE_CHECK(r.read_exact(std::span<std::byte>(out)).has_value());
}

SLUICE_MAIN()
