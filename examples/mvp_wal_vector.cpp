// mvp_wal_vector: write multiple WAL records via wal::write_record_vec, then
// read them back with the existing wal::read_record. Verifies payloads. This is
// NOT a vector-I/O performance claim — it only shows the vector path composes
// end-to-end with the scalar reader.
#include <sluice/file.hpp>
#include "support/temp_path.hpp"
#include <sluice/wal.hpp>

using sluice::bench::TempPath;

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> bytes_of(std::string_view s) {
    auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

} // namespace

int main() {
    TempPath tp;
    const std::vector<std::string> records = {"alpha-record", "beta-record", "gamma-record"};

    {
        sluice::FileWriter w(tp.str());
        if (!w.opened()) {
            std::fprintf(stderr, "cannot open %s for writing\n", tp.str().c_str());
            return 1;
        }
        for (const auto& rec : records) {
            auto payload = bytes_of(rec);
            auto r = sluice::wal::write_record_vec(w, std::span<const std::byte>(payload));
            if (!r.has_value()) {
                std::fprintf(stderr, "write_record_vec failed: %s\n",
                             sluice::to_string(r.error().code).data());
                return 1;
            }
        }
    }

    sluice::FileReader r(tp.str());
    if (!r.opened()) {
        std::fprintf(stderr, "cannot open %s for reading\n", tp.str().c_str());
        return 1;
    }

    int count = 0;
    std::size_t payload_bytes = 0;
    bool mismatch = false;
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
        if (count < static_cast<int>(records.size())) {
            const auto& expected = records[count];
            if (res.value().size() != expected.size() ||
                std::memcmp(res.value().data(), expected.data(), expected.size()) != 0) {
                mismatch = true;
            }
        }
        ++count;
        payload_bytes += res.value().size();
    }

    if (count != static_cast<int>(records.size()) || mismatch) {
        std::fprintf(stderr, "record count/content mismatch: read %d records\n", count);
        return 1;
    }

    std::printf("mvp_wal_vector: wrote+read %d records, %zu payload bytes\n", count, payload_bytes);
    return 0;
}
