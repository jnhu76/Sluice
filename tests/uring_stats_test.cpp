// Tests for UringStats wiring (CPPIO-CORE-013E). Without liburing the stats
// stay zero (no ops run); with liburing a successful write bumps the counters.
#include "harness.hpp"

#include <sluice/experimental/uring_io_context.hpp>
#include <sluice/measurement.hpp>

#if defined(SLUICE_HAS_LIBURING)
#include <filesystem>
#include <fstream>
#include <sstream>
#endif

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#if !defined(SLUICE_HAS_LIBURING)
SLUICE_TEST_CASE(uring_stats_nullptr_changes_nothing_stub) {
    sluice::experimental::UringWriteBatch batch(8);
    batch.set_stats(nullptr);  // no crash, no counting
    sluice::UringStats st{};
    batch.set_stats(&st);
    std::vector<std::byte> buf(4, std::byte{0});
    (void)batch.write_all(/*fd=*/1, std::span<const std::byte>(buf), 0);
    // Stub returns before any op, so no counters move.
    SLUICE_CHECK(st.submit_calls == 0);
    SLUICE_CHECK(st.completed_ops == 0);
}
#else
namespace {
struct TempPath {
    std::filesystem::path p;
    TempPath() {
        std::ostringstream oss;
        oss << "sluice_uring_stats_" << std::hex << reinterpret_cast<std::uintptr_t>(this) << ".tmp";
        p = std::filesystem::temp_directory_path() / oss.str();
    }
    ~TempPath() {
        try { std::filesystem::remove(p); } catch (...) {}
    }
    std::string str() const { return p.string(); }
};
}  // namespace

SLUICE_TEST_CASE(uring_stats_increment_on_successful_write) {
    TempPath tp;
    sluice::experimental::UringIoContext ctx(8);
    sluice::UringStats st{};
    ctx.set_stats(&st);
    std::string payload = "uring stats";
    std::vector<std::byte> buf(payload.begin(), payload.end());
    auto r = ctx.write_file_all(tp.str(), std::span<const std::byte>(buf));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(st.queue_init_calls >= 1);
    SLUICE_CHECK(st.submitted_ops >= 1);
    SLUICE_CHECK(st.completed_ops >= 1);
    SLUICE_CHECK(st.bytes_completed == payload.size());
    SLUICE_CHECK(st.completion_errors == 0);
}
#endif

SLUICE_MAIN()
