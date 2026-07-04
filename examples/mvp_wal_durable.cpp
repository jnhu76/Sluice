// mvp_wal_durable: demonstrates the WAL durability boundary (CPPIO-CORE-008).
// Writes several WAL records through a WalWriter backed by a FileWriter, flushes,
// syncs, and prints written/flushed/durable LSN while verifying the invariant.
// Does NOT claim durability beyond what OS sync provides.
#include <sluice/file.hpp>
#include <sluice/sync.hpp>
#include <sluice/wal.hpp>

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct TempPath {
    std::filesystem::path p;
    TempPath() {
        std::ostringstream oss;
        oss << "sluice_wal_dur_" << std::hex << reinterpret_cast<std::uintptr_t>(this) << ".tmp";
        p = std::filesystem::temp_directory_path() / oss.str();
    }
    ~TempPath() {
        try {
            std::filesystem::remove(p);
        } catch (...) {}
    }
    std::string str() const { return p.string(); }
};

std::vector<std::byte> bytes_of(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

} // namespace

int main() {
    TempPath tp;
    const std::vector<std::string> records = {"rec-one", "rec-two", "rec-three"};

    sluice::SyncStats sync_stats{};
    sluice::FileWriter fw(tp.str(), nullptr, nullptr, &sync_stats);
    if (!fw.opened()) {
        std::fprintf(stderr, "cannot open %s\n", tp.str().c_str());
        return 1;
    }
    // FileWriter implements SyncableWriter, so sync() can reach fdatasync.
    sluice::SyncableWriter* syncable = &fw;
    sluice::wal::WalWriter wal(fw, syncable);

    // 1. Write several records. Only written_lsn should advance.
    for (const auto& r : records) {
        auto payload = bytes_of(r);
        auto res = wal.write_record(std::span<const std::byte>(payload));
        if (!res.has_value()) {
            std::fprintf(stderr, "write_record failed: %s\n",
                         sluice::to_string(res.error().code).data());
            return 1;
        }
    }
    std::printf("after write:  written=%llu flushed=%llu durable=%llu\n",
                static_cast<unsigned long long>(wal.written_lsn()),
                static_cast<unsigned long long>(wal.flushed_lsn()),
                static_cast<unsigned long long>(wal.durable_lsn()));

    // 2. Flush. flushed_lsn should catch up to written_lsn.
    auto fr = wal.flush();
    if (!fr.has_value()) {
        std::fprintf(stderr, "flush failed\n");
        return 1;
    }
    std::printf("after flush:  written=%llu flushed=%llu durable=%llu\n",
                static_cast<unsigned long long>(wal.written_lsn()),
                static_cast<unsigned long long>(wal.flushed_lsn()),
                static_cast<unsigned long long>(wal.durable_lsn()));

    // 3. Sync (flush + fdatasync). durable_lsn should catch up.
    auto sr = wal.sync();
    if (!sr.has_value()) {
        std::fprintf(stderr, "sync failed: %s\n", sluice::to_string(sr.error().code).data());
        return 1;
    }
    std::printf("after sync:   written=%llu flushed=%llu durable=%llu\n",
                static_cast<unsigned long long>(wal.written_lsn()),
                static_cast<unsigned long long>(wal.flushed_lsn()),
                static_cast<unsigned long long>(wal.durable_lsn()));
    std::printf("sync_data_calls=%llu\n",
                static_cast<unsigned long long>(sync_stats.sync_data_calls));

    // 4. Verify invariant: durable <= flushed <= written.
    if (!(wal.durable_lsn() <= wal.flushed_lsn() && wal.flushed_lsn() <= wal.written_lsn())) {
        std::fprintf(stderr, "LSN invariant violated\n");
        return 1;
    }

    // 5. Avoid claiming durability beyond OS sync semantics.
    std::printf("mvp_wal_durable: %zu records written, LSN invariant holds\n", records.size());
    std::printf("  (durable_lsn reflects an fdatasync request; actual persistence\n");
    std::printf("   depends on OS/filesystem/disk behavior)\n");
    return 0;
}
