// #600 E1/E3 Direct cost ladder: A0 raw pread, A1 matched native wrapper,
// A2 shipping sluice::blocking::read_at, A3 no-syscall diagnostic stub,
// AX sluice::blocking::read_exact_at composition.
// Research probe: lives in the #600 workspace, not the repository.

#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::Result;

constexpr std::uint64_t kMaxOff = 0x7fffffffffffffffull;

// --- A1: semantically matched native wrapper -------------------------------
// Same public contract as sluice::blocking::read_at on the legal supported
// domain of this probe (open read-capable File, nonzero length, validated
// range): closed -> invalid_state; write_only -> invalid_argument; len 0 -> 0;
// bad range/offset -> invalid_argument; EINTR retried; errno mapped; count
// returned in Result<size_t>. Hand-written minimal body, separate TU boundary,
// no sluice detail headers.
Result<std::size_t> a1_read_at(const File& file, std::uint64_t offset,
                               std::span<std::byte> dst) {
    if (file.native_handle() < 0)
        return sluice::make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_state});
    if (file.access() == FileAccess::write_only)
        return sluice::make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_argument});
    const std::size_t len = dst.size();
    if (len == 0)
        return std::size_t{0};
    if (offset > kMaxOff || len - 1 > kMaxOff - offset ||
        len > static_cast<std::size_t>(0x7fffffffffffffffll))
        return sluice::make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_argument});
    if (dst.data() == nullptr)
        return sluice::make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_argument});
    ssize_t n;
    do {
        n = ::pread(file.native_handle(), dst.data(), len, static_cast<off_t>(offset));
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return sluice::make_unexpected<std::size_t>(sluice::from_errno_value(errno));
    return static_cast<std::size_t>(n);
}

// --- A3: no-syscall diagnostic stub -----------------------------------------
// Same validation and return shape as A1, but the read itself is modeled as a
// full-count success without entering the kernel. Attribution only: never a
// performance or correctness oracle.
Result<std::size_t> a3_stub_read_at(const File& file, std::uint64_t offset,
                                    std::span<std::byte> dst) {
    if (file.native_handle() < 0)
        return sluice::make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_state});
    if (file.access() == FileAccess::write_only)
        return sluice::make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_argument});
    const std::size_t len = dst.size();
    if (len == 0)
        return std::size_t{0};
    if (offset > kMaxOff || len - 1 > kMaxOff - offset ||
        len > static_cast<std::size_t>(0x7fffffffffffffffll))
        return sluice::make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_argument});
    if (dst.data() == nullptr)
        return sluice::make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_argument});
    return len;
}

double now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
}


bool verify_pattern(const std::byte* p, std::size_t n, std::uint64_t base) {
    for (std::size_t i = 0; i < n; ++i)
        if (p[i] != static_cast<std::byte>((base + i) * 1103515245u >> 13 & 0xff))
            return false;
    return true;
}

struct RunResult {
    double ns_per_op = 0;
    std::uint64_t checksum = 0;
    bool ok = false;
};

enum class Arm { A0, A1, A2, A3, AX };

const char* arm_name(Arm arm) {
    switch (arm) {
    case Arm::A0: return "A0_raw_pread";
    case Arm::A1: return "A1_native_matched";
    case Arm::A2: return "A2_sluice_read_at";
    case Arm::A3: return "A3_nosyscall_stub";
    case Arm::AX: return "AX_sluice_read_exact_at";
    }
    return "?";
}

RunResult run_arm(Arm arm, int fd, const File& file, std::size_t block,
                  std::size_t ops, std::size_t file_blocks, std::byte* buf,
                  std::size_t warmup_ops, std::size_t reps) {
    RunResult out;
    std::uint64_t sink = 0;
    auto one = [&](std::size_t i) -> std::uint64_t {
        const std::uint64_t off = (i % file_blocks) * static_cast<std::uint64_t>(block);
        switch (arm) {
        case Arm::A0:
            return static_cast<std::uint64_t>(
                ::pread(fd, buf, block, static_cast<off_t>(off)));
        case Arm::A1: {
            auto r = a1_read_at(file, off, std::span<std::byte>(buf, block));
            return r.has_value() ? r.value() : 0xdeadull;
        }
        case Arm::A2: {
            auto r = sluice::blocking::read_at(file, off, std::span<std::byte>(buf, block));
            return r.has_value() ? r.value() : 0xdeadull;
        }
        case Arm::A3: {
            auto r = a3_stub_read_at(file, off, std::span<std::byte>(buf, block));
            return r.has_value() ? r.value() : 0xdeadull;
        }
        case Arm::AX: {
            auto r = sluice::blocking::read_exact_at(file, off, std::span<std::byte>(buf, block));
            return r.has_value() ? r.value().confirmed_bytes : 0xdeadull;
        }
        }
        return 0;
    };
    for (std::size_t i = 0; i < warmup_ops; ++i)
        sink += one(i);
    // correctness gate: full pattern verify over the first pass
    for (std::size_t i = 0; i < ops && i < file_blocks; ++i) {
        const std::uint64_t off = i * static_cast<std::uint64_t>(block);
        (void)one(i);
        if (arm != Arm::A3 && !verify_pattern(buf, block, off)) {
            std::fprintf(stderr, "VERIFY_FAIL arm=%s i=%zu\n", arm_name(arm), i);
            return out;
        }
    }
    double best = 1e30;
    for (std::size_t r = 0; r < reps; ++r) {
        const double t0 = now_ns();
        std::uint64_t s = 0;
        for (std::size_t i = 0; i < ops; ++i)
            s += one(i);
        const double dt = now_ns() - t0;
        sink += s;
        if (dt < best)
            best = dt;
    }
    out.ns_per_op = best / static_cast<double>(ops);
    out.checksum = sink;
    out.ok = true;
    (void)fd;
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr,
                     "usage: %s arm size_bytes ops warmup reps file\n"
                     "arm: a0|a1|a2|a3|ax\n",
                     argv[0]);
        return 2;
    }
    const std::string arm_s = argv[1];
    Arm arm;
    if (arm_s == "a0") arm = Arm::A0;
    else if (arm_s == "a1") arm = Arm::A1;
    else if (arm_s == "a2") arm = Arm::A2;
    else if (arm_s == "a3") arm = Arm::A3;
    else if (arm_s == "ax") arm = Arm::AX;
    else { std::fprintf(stderr, "bad arm\n"); return 2; }
    const std::size_t block = std::strtoull(argv[2], nullptr, 10);
    const std::size_t ops = std::strtoull(argv[3], nullptr, 10);
    const std::size_t warmup = std::strtoull(argv[4], nullptr, 10);
    const std::size_t reps = std::strtoull(argv[5], nullptr, 10);
    const std::string path = argv[6];

    FileOpen mode;
    mode.access = FileAccess::read_only;
    auto opened = File::open(path, mode);
    if (!opened.has_value()) {
        std::fprintf(stderr, "open failed\n");
        return 1;
    }
    File file = std::move(opened).value();
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "native open failed\n");
        return 1;
    }
    struct ::stat st {};
    ::fstat(fd, &st);
    const std::size_t file_blocks =
        st.st_size / (block ? block : 1) > 0 ? st.st_size / (block ? block : 1) : 1;

    std::vector<std::byte> buf(block ? block : 1);
    rusage before {}, after {};
    getrusage(RUSAGE_SELF, &before);
    const RunResult r = run_arm(arm, fd, file, block, ops, file_blocks, buf.data(), warmup, reps);
    getrusage(RUSAGE_SELF, &after);
    if (!r.ok)
        return 1;
    const double total_ops = static_cast<double>(ops) * reps + static_cast<double>(warmup) +
                             static_cast<double>(ops < file_blocks ? ops : file_blocks);
    const double user_per_op =
        ((after.ru_utime.tv_sec - before.ru_utime.tv_sec) +
         (after.ru_utime.tv_usec - before.ru_utime.tv_usec) * 1e-6) * 1e9 / total_ops;
    const double sys_per_op =
        ((after.ru_stime.tv_sec - before.ru_stime.tv_sec) +
         (after.ru_stime.tv_usec - before.ru_stime.tv_usec) * 1e-6) * 1e9 / total_ops;
    std::printf("arm=%s size=%zu ops=%zu ns_per_op=%.3f user_ns_per_op=%.3f sys_ns_per_op=%.3f "
                "checksum=%llu\n",
                arm_name(arm), block, ops, r.ns_per_op, user_per_op, sys_per_op,
                static_cast<unsigned long long>(r.checksum));
    ::close(fd);
    return 0;
}
