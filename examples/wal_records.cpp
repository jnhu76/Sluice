// wal_records: write multiple WAL records to a file, then read them back.
#include <sluice/file.hpp>
#include <sluice/wal.hpp>

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> bytes_of(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

} // namespace

int main(int argc, char** argv) {
    std::string path = argc >= 2 ? argv[1] : "/tmp/sluice_wal_demo.tmp";

    {
        sluice::FileWriter w(path);
        if (!w.opened()) {
            std::fprintf(stderr, "cannot open %s for writing\n", path.c_str());
            return 1;
        }
        for (const char* rec : {"alpha", "beta", "gamma"}) {
            auto payload = bytes_of(rec);
            auto r = sluice::wal::write_record(w, std::span<const std::byte>(payload));
            if (!r.has_value()) {
                std::fprintf(stderr, "write_record failed: %s\n",
                             sluice::to_string(r.error().code).data());
                return 1;
            }
        }
    }

    sluice::FileReader r(path);
    if (!r.opened()) {
        std::fprintf(stderr, "cannot open %s for reading\n", path.c_str());
        return 1;
    }
    int count = 0;
    std::size_t total_bytes = 0;
    while (true) {
        auto res = sluice::wal::read_record(r);
        if (!res.has_value()) {
            if (res.error().code != sluice::IoError::Code::eof) {
                std::fprintf(stderr, "read_record error: %s\n",
                             sluice::to_string(res.error().code).data());
                return 1;
            }
            break;
        }
        ++count;
        total_bytes += res.value().size();
    }
    std::printf("wal_records: read back %d records, %zu payload bytes\n", count, total_bytes);
    return 0;
}
