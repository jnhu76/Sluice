// Tests for UringStats wiring (CPPIO-CORE-013E). Without liburing the stats
// stay zero (no ops run); with liburing a successful write bumps the counters.
#include "harness.hpp"

#include <cppio/experimental/uring_io_context.hpp>
#include <cppio/measurement.hpp>

#if defined(CPPIO_HAS_LIBURING)
#include <filesystem>
#include <fstream>
#endif

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#if !defined(CPPIO_HAS_LIBURING)
CPPIO_TEST_CASE(uring_stats_nullptr_changes_nothing_stub) {
    cppio::experimental::UringWriteBatch batch(8);
    batch.set_stats(nullptr);  // no crash, no counting
    cppio::UringStats st{};
    batch.set_stats(&st);
    std::vector<std::byte> buf(4, std::byte{0});
    (void)batch.write_all(/*fd=*/1, std::span<const std::byte>(buf), 0);
    // Stub returns before any op, so no counters move.
    CPPIO_CHECK(st.submit_calls == 0);
    CPPIO_CHECK(st.completed_ops == 0);
}
#else
namespace {
struct TempPath {
    std::filesystem::path p;
    TempPath() {
        p = std::filesystem::temp_directory_path() /
            ("cppio_uring_stats_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tmp");
    }
    ~TempPath() {
        try { std::filesystem::remove(p); } catch (...) {}
    }
    std::string str() const { return p.string(); }
};
}  // namespace

CPPIO_TEST_CASE(uring_stats_increment_on_successful_write) {
    TempPath tp;
    cppio::experimental::UringIoContext ctx(8);
    cppio::UringStats st{};
    ctx.set_stats(&st);
    std::string payload = "uring stats";
    std::vector<std::byte> buf(payload.begin(), payload.end());
    auto r = ctx.write_file_all(tp.str(), std::span<const std::byte>(buf));
    CPPIO_CHECK(r.has_value());
    CPPIO_CHECK(st.queue_init_calls >= 1);
    CPPIO_CHECK(st.submitted_ops >= 1);
    CPPIO_CHECK(st.completed_ops >= 1);
    CPPIO_CHECK(st.bytes_completed == payload.size());
    CPPIO_CHECK(st.completion_errors == 0);
}
#endif

CPPIO_MAIN()
