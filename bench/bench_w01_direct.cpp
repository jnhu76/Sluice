// G0 W-01 workload: direct-execution File I/O through the public
// sluice::File / sluice::blocking surface, with no AsyncIoContext, worker,
// request table or progress source (root W-01 / INV-02).

#include "bench_common.hpp"

#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileOpen;

constexpr std::size_t kInstrumentedSampleCap = 200000;
constexpr std::size_t kScratchBytes = 2u << 20;
constexpr std::size_t kSetupChunk = 1u << 20;

FileOpen rw_open_existing() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::open_existing;
    return mode;
}

FileOpen rw_create_truncate() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::create_if_missing;
    mode.contents = sluice::FileInitialContents::truncate;
    return mode;
}

std::string err_name(const sluice::IoError& e) {
    return std::string(sluice::to_string(e.code)) + "(errno=" + std::to_string(e.os_errno) + ")";
}

bool write_file(const std::string& path, std::size_t size, std::string& failure) {
    auto opened = File::open(path, rw_create_truncate());
    if (!opened.has_value()) {
        failure = "create failed: " + err_name(opened.error());
        return false;
    }
    File file = std::move(opened).value();
    std::vector<std::byte> buffer(kSetupChunk);
    std::size_t written = 0;
    while (written < size) {
        const std::size_t chunk = std::min(kSetupChunk, size - written);
        bench::fill_pattern(buffer.data(), chunk, written);
        auto res = sluice::blocking::write_all_at(
            file, written, std::span<const std::byte>(buffer.data(), chunk));
        if (!res.has_value() || !res.value().complete()) {
            failure = "setup write failed at " + std::to_string(written);
            return false;
        }
        written += chunk;
    }
    if (!sluice::blocking::sync_data(file).has_value()) {
        failure = "setup sync failed";
        return false;
    }
    return true;
}

void evict_page_cache(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        ::close(fd);
    }
}

struct Fixture {
    std::string tmp_dir;
    std::string disk_dir;
    std::string small;
    std::string write_4k;
    std::string read_4k_seq;
    std::string read_4k_rand;
    std::string read_64k;
    std::string read_1m;
    std::string composition;
    std::string sync_small;
    std::string disk_64k;
    std::string disk_1m;
    bool disk_available = false;

    static constexpr std::size_t kFile4kSeqBytes = 64u << 20;
    static constexpr std::size_t kFile64kBytes = 128u << 20;
    static constexpr std::size_t kFile1mBytes = 512u << 20;
    static constexpr std::size_t kDisk64kBytes = 128u << 20;
    static constexpr std::size_t kDisk1mBytes = 512u << 20;

    bool create(const bench::EnvConfig& cfg, std::string& failure) {
        tmp_dir = cfg.tmp_dir;
        disk_dir = cfg.disk_dir;
        bench::ensure_dir(tmp_dir);
        small = tmp_dir + "/small.bin";
        write_4k = tmp_dir + "/write_4k.bin";
        read_4k_seq = tmp_dir + "/read_4k_seq.bin";
        read_4k_rand = tmp_dir + "/read_4k_rand.bin";
        read_64k = tmp_dir + "/read_64k.bin";
        read_1m = tmp_dir + "/read_1m.bin";
        composition = tmp_dir + "/composition.bin";
        sync_small = tmp_dir + "/sync_small.bin";

        if (!write_file(small, 4096, failure)) return false;
        if (!write_file(write_4k, kFile4kSeqBytes, failure)) return false;
        if (!write_file(read_4k_seq, kFile4kSeqBytes, failure)) return false;
        if (!write_file(read_4k_rand, kFile4kSeqBytes, failure)) return false;
        if (!write_file(read_64k, kFile64kBytes, failure)) return false;
        if (!write_file(read_1m, kFile1mBytes, failure)) return false;
        if (!write_file(composition, 4u << 20, failure)) return false;
        if (!write_file(sync_small, 4096, failure)) return false;

        if (cfg.include_disk) {
            bench::ensure_dir(disk_dir);
            disk_64k = disk_dir + "/read_64k.bin";
            disk_1m = disk_dir + "/read_1m.bin";
            std::string disk_failure;
            const bool ok64 = write_file(disk_64k, kDisk64kBytes, disk_failure);
            const bool ok1m = ok64 && write_file(disk_1m, kDisk1mBytes, disk_failure);
            disk_available = ok1m;
            if (!disk_available) {
                std::fprintf(stderr, "note: disk-backed fixture unavailable: %s\n",
                             disk_failure.c_str());
            }
        }
        return true;
    }
};

std::vector<std::uint64_t> random_offsets(std::size_t count, std::size_t block_size,
                                         std::size_t file_size) {
    std::mt19937_64 rng(0x5eed403u);
    const std::uint64_t blocks = static_cast<std::uint64_t>((file_size - block_size) / block_size);
    std::uniform_int_distribution<std::uint64_t> dist(0, blocks);
    std::vector<std::uint64_t> offsets(count);
    for (std::uint64_t& offset : offsets) {
        offset = dist(rng) * block_size;
    }
    return offsets;
}

// -------------------------------------------------------------------------

int case_open_close(const Fixture& fx, const bench::EnvConfig& cfg, std::size_t order) {
    bench::CaseMeta meta;
    meta.case_id = "W01-open_close";
    meta.workload = "W-01";
    meta.backend = "direct";
    meta.dataset = "4096B file, open_existing then explicit close";
    meta.cache_policy = "metadata only";
    meta.filesystem = "tmpfs";
    meta.notes = "no data traffic; cost is open(2)+close(2) per operation";
    meta.request_count = 2000;

    std::vector<std::byte> scratch(kScratchBytes);
    const std::string path = fx.small;
    return bench::run_case_reps(
        meta, cfg, order, [](std::size_t, std::string&) { return true; },
        [&](std::vector<std::byte>&, std::string&) {
            return bench::time_ops(
                meta.request_count, [&](std::size_t, std::size_t& bytes, std::string& fail) {
                    auto opened = File::open(path, rw_open_existing());
                    if (!opened.has_value()) {
                        fail = "open failed: " + err_name(opened.error());
                        return false;
                    }
                    auto closed = opened.value().close();
                    if (!closed.has_value()) {
                        fail = "close failed: " + err_name(closed.error());
                        return false;
                    }
                    bytes = 0;
                    return true;
                });
        },
        [](std::vector<std::byte>&, std::string&) { return true; },
        [&](std::vector<double>& samples, std::string& failure) {
            return bench::time_ops_perop(
                meta.request_count,
                [&](std::size_t, std::size_t& bytes, std::string& fail) {
                    auto opened = File::open(path, rw_open_existing());
                    if (!opened.has_value()) {
                        fail = "open failed";
                        return false;
                    }
                    (void)opened.value().close();
                    bytes = 0;
                    return true;
                },
                samples, failure, kInstrumentedSampleCap);
        },
        scratch);
}

int case_read_at(const bench::EnvConfig& cfg, std::size_t order, const std::string& case_id,
                 const std::string& path, std::size_t block_size, std::size_t count, bool random,
                 bool evict, std::size_t file_size, const std::string& fs) {
    bench::CaseMeta meta;
    meta.case_id = case_id;
    meta.workload = "W-01";
    meta.backend = "direct";
    meta.dataset = std::to_string(file_size >> 20) + "MiB file, " +
                   (random ? "random" : "sequential") + " offsets";
    meta.cache_policy = evict ? "page_cache_eviction_attempted(fadvise DONTNEED per repetition)"
                              : "page_cache_resident";
    meta.filesystem = fs;
    meta.notes = "blocking::read_at; throughput phase uninstrumented; oracle re-reads every offset";
    meta.block_size = block_size;
    meta.request_count = count;

    std::vector<std::byte> scratch(kScratchBytes);
    const std::vector<std::uint64_t> offsets =
        random ? random_offsets(count, block_size, file_size) : std::vector<std::uint64_t>{};
    auto offset_of = [&](std::size_t i) -> std::uint64_t {
        return random ? offsets[i]
                      : static_cast<std::uint64_t>(i % (file_size / block_size)) * block_size;
    };
    auto open = [&]() { return File::open(path, rw_open_existing()); };

    return bench::run_case_reps(
        meta, cfg, order,
        [&](std::size_t, std::string& failure) {
            if (evict) {
                evict_page_cache(path);
            }
            auto opened = open();
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            std::vector<std::byte> probe(block_size);
            const std::size_t probes = std::min<std::size_t>(count, 64);
            for (std::size_t i = 0; i < probes; ++i) {
                const std::size_t index = count > probes ? (i * (count / probes)) : i;
                auto res = sluice::blocking::read_exact_at(
                    file, offset_of(index), std::span<std::byte>(probe.data(), block_size));
                if (!res.has_value() || !res.value().complete()) {
                    failure = "fixture probe read failed";
                    return false;
                }
                const auto mismatch =
                    bench::verify_pattern(probe.data(), block_size, offset_of(index));
                if (!mismatch.ok) {
                    failure = "fixture content mismatch at offset index " + std::to_string(index);
                    return false;
                }
            }
            return true;
        },
        [&](std::vector<std::byte>& buf, std::string& failure) {
            auto opened = open();
            if (!opened.has_value()) {
                failure = "open failed";
                return bench::RunOutcome{false, failure, 0, 0, 0};
            }
            File file = std::move(opened).value();
            return bench::time_ops(count, [&](std::size_t i, std::size_t& bytes,
                                              std::string& fail) {
                auto res = sluice::blocking::read_at(
                    file, offset_of(i), std::span<std::byte>(buf.data(), block_size));
                if (!res.has_value()) {
                    fail = "read_at failed: " + err_name(res.error());
                    return false;
                }
                bytes = res.value();
                return true;
            });
        },
        [&](std::vector<std::byte>& buf, std::string& failure) {
            auto opened = open();
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            for (std::size_t i = 0; i < count; ++i) {
                auto res = sluice::blocking::read_exact_at(
                    file, offset_of(i), std::span<std::byte>(buf.data(), block_size));
                if (!res.has_value() || !res.value().complete()) {
                    failure = "oracle read failed at operation " + std::to_string(i);
                    return false;
                }
                const auto mismatch = bench::verify_pattern(buf.data(), block_size, offset_of(i));
                if (!mismatch.ok) {
                    failure = "byte mismatch at operation " + std::to_string(i) + " absolute " +
                              std::to_string(mismatch.first_bad_position) + " (" +
                              std::to_string(mismatch.bad_segments) + " segments)";
                    return false;
                }
            }
            return true;
        },
        [&](std::vector<double>& samples, std::string& failure) {
            auto opened = open();
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            return bench::time_ops_perop(
                count,
                [&](std::size_t i, std::size_t& bytes, std::string& fail) {
                    auto res = sluice::blocking::read_at(
                        file, offset_of(i), std::span<std::byte>(scratch.data(), block_size));
                    if (!res.has_value()) {
                        fail = "read_at failed";
                        return false;
                    }
                    bytes = res.value();
                    return true;
                },
                samples, failure, kInstrumentedSampleCap);
        },
        scratch);
}

int case_write_at(const Fixture& fx, const bench::EnvConfig& cfg, std::size_t order) {
    bench::CaseMeta meta;
    meta.case_id = "W01-write_at-4k-seq";
    meta.workload = "W-01";
    meta.backend = "direct";
    meta.dataset = "64MiB file, 4096B sequential write_at";
    meta.cache_policy = "page_cache_resident";
    meta.filesystem = "tmpfs";
    meta.notes = "fixture is refilled before each repetition; oracle reads back every offset";
    meta.block_size = 4096;
    meta.request_count = 100000;

    std::vector<std::byte> scratch(kScratchBytes);
    const std::string path = fx.write_4k;
    const std::size_t block = meta.block_size;
    const std::size_t count = meta.request_count;

    return bench::run_case_reps(
        meta, cfg, order,
        [&](std::size_t, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            std::vector<std::byte> block_buf(block);
            std::size_t written = 0;
            const std::size_t total = count * block;
            while (written < total) {
                const std::size_t chunk = std::min(block, total - written);
                bench::fill_pattern(block_buf.data(), chunk, written);
                auto res = sluice::blocking::write_all_at(
                    file, written, std::span<const std::byte>(block_buf.data(), chunk));
                if (!res.has_value() || !res.value().complete()) {
                    failure = "fixture refill failed";
                    return false;
                }
                written += chunk;
            }
            return true;
        },
        [&](std::vector<std::byte>& buf, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return bench::RunOutcome{false, failure, 0, 0, 0};
            }
            File file = std::move(opened).value();
            bench::fill_pattern(buf.data(), block, 0);
            return bench::time_ops(count, [&](std::size_t i, std::size_t& bytes,
                                              std::string& fail) {
                const std::uint64_t base = static_cast<std::uint64_t>(i) * block;
                auto res = sluice::blocking::write_at(
                    file, base, std::span<const std::byte>(buf.data(), block));
                if (!res.has_value()) {
                    fail = "write_at failed: " + err_name(res.error());
                    return false;
                }
                bytes = res.value();
                return true;
            });
        },
        [&](std::vector<std::byte>&, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            if (!sluice::blocking::sync_data(file).has_value()) {
                failure = "sync before readback failed";
                return false;
            }
            std::vector<std::byte> probe(block);
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint64_t base = static_cast<std::uint64_t>(i) * block;
                auto res = sluice::blocking::read_exact_at(
                    file, base, std::span<std::byte>(probe.data(), block));
                if (!res.has_value() || !res.value().complete()) {
                    failure = "readback failed at operation " + std::to_string(i);
                    return false;
                }
                const auto mismatch = bench::verify_pattern(probe.data(), block, base);
                if (!mismatch.ok) {
                    failure = "written data mismatch at operation " + std::to_string(i);
                    return false;
                }
            }
            return true;
        },
        [&](std::vector<double>& samples, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            std::vector<std::byte> buf(block);
            bench::fill_pattern(buf.data(), block, 0);
            return bench::time_ops_perop(
                count,
                [&](std::size_t i, std::size_t& bytes, std::string& fail) {
                    const std::uint64_t base = static_cast<std::uint64_t>(i) * block;
                    auto res = sluice::blocking::write_at(
                        file, base, std::span<const std::byte>(buf.data(), block));
                    if (!res.has_value()) {
                        fail = "write_at failed";
                        return false;
                    }
                    bytes = res.value();
                    return true;
                },
                samples, failure, kInstrumentedSampleCap);
        },
        scratch);
}

int case_durability(const Fixture& fx, const bench::EnvConfig& cfg, std::size_t order,
                    bool use_sync_all) {
    bench::CaseMeta meta;
    meta.case_id = use_sync_all ? "W01-sync_all-4k" : "W01-sync_data-4k";
    meta.workload = "W-01";
    meta.backend = "direct";
    meta.dataset = "4096B write followed by an explicit durability barrier";
    meta.cache_policy = "durability barrier timed alone";
    meta.filesystem = "tmpfs";
    meta.notes = "the write is followed by the barrier inside the timed operation; "
                 "durability cost is reported separately from plain read/write";
    meta.block_size = 4096;
    meta.request_count = 200;

    std::vector<std::byte> scratch(kScratchBytes);
    const std::string path = fx.sync_small;
    const std::size_t count = meta.request_count;
    std::vector<std::byte> payload(4096);
    bench::fill_pattern(payload.data(), payload.size(), 0);

    auto barrier = [&](const File& file) {
        return use_sync_all ? sluice::blocking::sync_all(file)
                            : sluice::blocking::sync_data(file);
    };

    return bench::run_case_reps(
        meta, cfg, order,
        [&](std::size_t, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            auto w = sluice::blocking::write_all_at(
                file, 0, std::span<const std::byte>(payload.data(), payload.size()));
            if (!w.has_value() || !w.value().complete()) {
                failure = "pre-write failed";
                return false;
            }
            return true;
        },
        [&](std::vector<std::byte>&, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return bench::RunOutcome{false, failure, 0, 0, 0};
            }
            File file = std::move(opened).value();
            return bench::time_ops(count, [&](std::size_t, std::size_t& op_bytes,
                                              std::string& fail) {
                auto w = sluice::blocking::write_at(
                    file, 0, std::span<const std::byte>(payload.data(), payload.size()));
                if (!w.has_value()) {
                    fail = "write failed";
                    return false;
                }
                auto s = barrier(file);
                if (!s.has_value()) {
                    fail = "barrier failed: " + err_name(s.error());
                    return false;
                }
                op_bytes = w.value();
                return true;
            });
        },
        [&](std::vector<std::byte>&, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            std::vector<std::byte> probe(payload.size());
            auto res = sluice::blocking::read_exact_at(
                file, 0, std::span<std::byte>(probe.data(), probe.size()));
            if (!res.has_value() || !res.value().complete()) {
                failure = "readback failed";
                return false;
            }
            const auto mismatch = bench::verify_pattern(probe.data(), probe.size(), 0);
            if (!mismatch.ok) {
                failure = "durability payload mismatch";
                return false;
            }
            return true;
        },
        [&](std::vector<double>& samples, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            return bench::time_ops_perop(
                count,
                [&](std::size_t, std::size_t& op_bytes, std::string& fail) {
                    auto w = sluice::blocking::write_at(
                        file, 0, std::span<const std::byte>(payload.data(), payload.size()));
                    if (!w.has_value()) {
                        fail = "write failed";
                        return false;
                    }
                    auto s = barrier(file);
                    if (!s.has_value()) {
                        fail = "barrier failed";
                        return false;
                    }
                    op_bytes = w.value();
                    return true;
                },
                samples, failure, kInstrumentedSampleCap);
        },
        scratch);
}

int case_file_info(const Fixture& fx, const bench::EnvConfig& cfg, std::size_t order) {
    bench::CaseMeta meta;
    meta.case_id = "W01-file_info";
    meta.workload = "W-01";
    meta.backend = "direct";
    meta.dataset = "4096B file, repeated minimal metadata read";
    meta.cache_policy = "metadata only";
    meta.filesystem = "tmpfs";
    meta.notes = "SEM-07 minimal metadata; also checks the reported size";
    meta.request_count = 100000;

    std::vector<std::byte> scratch(kScratchBytes);
    const std::string path = fx.small;
    return bench::run_case_reps(
        meta, cfg, order, [](std::size_t, std::string&) { return true; },
        [&](std::vector<std::byte>&, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return bench::RunOutcome{false, failure, 0, 0, 0};
            }
            File file = std::move(opened).value();
            return bench::time_ops(meta.request_count, [&](std::size_t, std::size_t& bytes,
                                                          std::string& fail) {
                auto info = sluice::blocking::file_info(file);
                if (!info.has_value()) {
                    fail = "file_info failed: " + err_name(info.error());
                    return false;
                }
                bytes = 0;
                return true;
            });
        },
        [&](std::vector<std::byte>&, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            auto info = sluice::blocking::file_info(file);
            auto size = sluice::blocking::size(file);
            if (!info.has_value() || !size.has_value()) {
                failure = "metadata call failed";
                return false;
            }
            if (info.value().size != 4096 || size.value() != 4096) {
                failure = "metadata mismatch: info.size=" + std::to_string(info.value().size) +
                          " size=" + std::to_string(size.value());
                return false;
            }
            return true;
        },
        [&](std::vector<double>& samples, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            return bench::time_ops_perop(
                meta.request_count,
                [&](std::size_t, std::size_t& bytes, std::string& fail) {
                    auto info = sluice::blocking::file_info(file);
                    if (!info.has_value()) {
                        fail = "file_info failed";
                        return false;
                    }
                    bytes = 0;
                    return true;
                },
                samples, failure, kInstrumentedSampleCap);
        },
        scratch);
}

int case_composition(const Fixture& fx, const bench::EnvConfig& cfg, std::size_t order,
                     bool use_write_all) {
    bench::CaseMeta meta;
    meta.case_id = use_write_all ? "W01-write_all_at-4k" : "W01-read_exact_at-4k";
    meta.workload = "W-01";
    meta.backend = "direct";
    meta.dataset = "4MiB file, 4096B exact composition";
    meta.cache_policy = "page_cache_resident";
    meta.filesystem = "tmpfs";
    meta.notes = "composition rule overhead on a single fully-satisfiable primitive (SEM-05)";
    meta.block_size = 4096;
    meta.request_count = 50000;

    std::vector<std::byte> scratch(kScratchBytes);
    const std::string path = fx.composition;
    const std::size_t block = meta.block_size;
    const std::size_t count = meta.request_count;
    const std::size_t file_blocks = (4u << 20) / block;

    return bench::run_case_reps(
        meta, cfg, order, [](std::size_t, std::string&) { return true; },
        [&](std::vector<std::byte>& buf, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return bench::RunOutcome{false, failure, 0, 0, 0};
            }
            File file = std::move(opened).value();
            return bench::time_ops(count, [&](std::size_t i, std::size_t& bytes,
                                              std::string& fail) {
                const std::uint64_t base = static_cast<std::uint64_t>(i % file_blocks) * block;
                if (use_write_all) {
                    auto res = sluice::blocking::write_all_at(
                        file, base, std::span<const std::byte>(buf.data(), block));
                    if (!res.has_value() || !res.value().complete()) {
                        fail = "write_all_at failed";
                        return false;
                    }
                    bytes = res.value().confirmed_bytes;
                } else {
                    auto res = sluice::blocking::read_exact_at(
                        file, base, std::span<std::byte>(buf.data(), block));
                    if (!res.has_value() || !res.value().complete()) {
                        fail = "read_exact_at failed";
                        return false;
                    }
                    bytes = res.value().confirmed_bytes;
                }
                return true;
            });
        },
        [&](std::vector<std::byte>& buf, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            if (!sluice::blocking::sync_data(file).has_value()) {
                failure = "sync failed";
                return false;
            }
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint64_t base = static_cast<std::uint64_t>(i % file_blocks) * block;
                auto res = sluice::blocking::read_exact_at(
                    file, base, std::span<std::byte>(buf.data(), block));
                if (!res.has_value() || !res.value().complete()) {
                    failure = "readback failed at operation " + std::to_string(i);
                    return false;
                }
                const auto mismatch = bench::verify_pattern(buf.data(), block, base);
                if (!mismatch.ok) {
                    failure = "content mismatch at operation " + std::to_string(i);
                    return false;
                }
            }
            return true;
        },
        [&](std::vector<double>& samples, std::string& failure) {
            auto opened = File::open(path, rw_open_existing());
            if (!opened.has_value()) {
                failure = "open failed";
                return false;
            }
            File file = std::move(opened).value();
            return bench::time_ops_perop(
                count,
                [&](std::size_t i, std::size_t& bytes, std::string& fail) {
                    const std::uint64_t base =
                        static_cast<std::uint64_t>(i % file_blocks) * block;
                    if (use_write_all) {
                        auto res = sluice::blocking::write_all_at(
                            file, base, std::span<const std::byte>(scratch.data(), block));
                        if (!res.has_value() || !res.value().complete()) {
                            fail = "write_all_at failed";
                            return false;
                        }
                        bytes = res.value().confirmed_bytes;
                    } else {
                        auto res = sluice::blocking::read_exact_at(
                            file, base, std::span<std::byte>(scratch.data(), block));
                        if (!res.has_value() || !res.value().complete()) {
                            fail = "read_exact_at failed";
                            return false;
                        }
                        bytes = res.value().confirmed_bytes;
                    }
                    return true;
                },
                samples, failure, kInstrumentedSampleCap);
        },
        scratch);
}

int case_clock_calibration(const bench::EnvConfig& cfg) {
    bench::CaseMeta meta;
    meta.case_id = "CAL-clock-overhead";
    meta.workload = "calibration";
    meta.backend = "n/a";
    meta.dataset = "one pair of steady_clock readings";
    meta.cache_policy = "n/a";
    meta.filesystem = "n/a";
    meta.notes = "per-timestamp cost of the instrumented latency pass";
    std::vector<std::byte> scratch(kScratchBytes);
    return bench::run_case_reps(
        meta, cfg, 0, [](std::size_t, std::string&) { return true; },
        [](std::vector<std::byte>&, std::string&) {
            bench::RunOutcome o;
            o.wall_s = bench::clock_overhead_ns() * 1e-9;
            o.ops = 1;
            o.bytes = 0;
            return o;
        },
        [](std::vector<std::byte>&, std::string&) { return true; },
        [](std::vector<double>& samples, std::string&) {
            samples.push_back(bench::clock_overhead_ns());
            return true;
        },
        scratch);
}

} // namespace

int main() {
    const bench::EnvConfig cfg = bench::env_config();
    std::printf("========================================================\n");
    std::printf("G0 W-01 direct-execution benchmark\n");
    std::printf("warmup=%zu measured_repetitions=%zu\n", cfg.warmup, cfg.measured);
    std::printf("tmp_dir=%s\n", cfg.tmp_dir.c_str());
    std::printf("disk_dir=%s\n", cfg.disk_dir.c_str());
    std::printf("========================================================\n");

    Fixture fx;
    std::string failure;
    if (!fx.create(cfg, failure)) {
        std::fprintf(stderr, "fixture creation failed: %s\n", failure.c_str());
        return 1;
    }

    int failed = 0;
    failed += case_clock_calibration(cfg);
    failed += case_open_close(fx, cfg, 1);
    failed += case_file_info(fx, cfg, 2);
    failed += case_read_at(cfg, 3, "W01-read_at-4k-seq", fx.read_4k_seq, 4096, 100000, false,
                           false, Fixture::kFile4kSeqBytes, "tmpfs");
    failed += case_read_at(cfg, 4, "W01-read_at-4k-rand", fx.read_4k_rand, 4096, 100000, true,
                           false, Fixture::kFile4kSeqBytes, "tmpfs");
    failed += case_read_at(cfg, 5, "W01-read_at-64k-seq", fx.read_64k, 65536, 4000, false, false,
                           Fixture::kFile64kBytes, "tmpfs");
    failed += case_read_at(cfg, 6, "W01-read_at-1m-seq", fx.read_1m, 1u << 20, 512, false, false,
                           Fixture::kFile1mBytes, "tmpfs");
    failed += case_write_at(fx, cfg, 7);
    failed += case_durability(fx, cfg, 8, false);
    failed += case_durability(fx, cfg, 9, true);
    failed += case_composition(fx, cfg, 10, false);
    failed += case_composition(fx, cfg, 11, true);

    if (fx.disk_available) {
        failed += case_read_at(cfg, 12, "W01-read_at-64k-seq-cold", fx.disk_64k, 65536, 4000,
                               false, true, Fixture::kDisk64kBytes, "ext4");
        failed += case_read_at(cfg, 13, "W01-read_at-1m-seq-cold", fx.disk_1m, 1u << 20, 512,
                               false, true, Fixture::kDisk1mBytes, "ext4");
    } else {
        std::printf("W01-read_at-64k-seq-cold: NOT_RUN (disk fixture unavailable)\n");
        std::printf("W01-read_at-1m-seq-cold: NOT_RUN (disk fixture unavailable)\n");
    }

    std::printf("\n=== W-01 complete, failing repetitions=%d ===\n", failed);
    return failed == 0 ? 0 : 1;
}
