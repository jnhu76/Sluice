// Direct cost ladder for read/write/metadata families, extending the #600
// read-path ladder (A0/A1/A2/A3/AX, preserved). New arms: A1x native-matched
// read_exact_at; W0/W1/W2/W1x/WX write_at + write_all_at; M0/M1/M2 for
// open+close, file_info and size. Shared-cursor composition twins: A1c/AC
// (read_exact) and W1c/WC (write_all), driven through the kernel file cursor
// of the File handle. Linked read-only against the frozen B1 archive (see
// README.md for the identity chain). Research probe for the direct-cost-ladder
// measurement branch.

#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

namespace {

using sluice::blocking::CompositionEnd;
using sluice::blocking::CompositionOutcome;
using sluice::EffectCertainty;
using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileIdentity;
using sluice::FileInfo;
using sluice::FileInitialContents;
using sluice::FileKind;
using sluice::FileOpen;
using sluice::IoError;
using sluice::Result;

constexpr std::uint64_t kMaxOff = 0x7fffffffffffffffull;
constexpr std::uint64_t kFnvBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint64_t kDefaultSeed = 0x9e3779b97f4a7c15ull;

IoError code_only(IoError::Code code) { return IoError{.code = code, .os_errno = 0}; }

std::uint64_t error_sink_value(const IoError& e) {
    return (static_cast<std::uint64_t>(e.code) << 8) |
           (static_cast<std::uint64_t>(e.os_errno) & 0xffu);
}

// Admission mirroring precheck_data_op order: closed -> access -> zero length
// -> range/transfer limit. The null-buffer check stays in each wrapper, after
// admission, matching the public functions.
enum class Admit { execute, complete_empty, reject_closed, reject_access, reject_range };

constexpr Admit admit_data(bool closed, FileAccess access, bool write_op,
                           std::uint64_t offset, std::size_t len) noexcept {
    if (closed)
        return Admit::reject_closed;
    const bool access_ok =
        write_op ? access != FileAccess::read_only : access != FileAccess::write_only;
    if (!access_ok)
        return Admit::reject_access;
    if (len == 0)
        return Admit::complete_empty;
    if (offset > kMaxOff || len - 1 > kMaxOff - offset ||
        len > static_cast<std::size_t>(0x7fffffffffffffffll))
        return Admit::reject_range;
    return Admit::execute;
}

IoError admit_rejection(Admit a) {
    if (a == Admit::reject_closed)
        return code_only(IoError::Code::invalid_state);
    return code_only(IoError::Code::invalid_argument);
}

// --- N1: hand-written native wrappers, public headers only ------------------

Result<std::size_t> n1_read_at(const File& file, std::uint64_t offset,
                               std::span<std::byte> dst) {
    const Admit a =
        admit_data(file.native_handle() < 0, file.access(), false, offset, dst.size());
    if (a == Admit::complete_empty)
        return std::size_t{0};
    if (a != Admit::execute)
        return sluice::make_unexpected<std::size_t>(admit_rejection(a));
    if (dst.data() == nullptr)
        return sluice::make_unexpected<std::size_t>(code_only(IoError::Code::invalid_argument));
    ssize_t n;
    do {
        n = ::pread(file.native_handle(), dst.data(), dst.size(), static_cast<off_t>(offset));
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return sluice::make_unexpected<std::size_t>(sluice::from_errno_value(errno));
    return static_cast<std::size_t>(n);
}

Result<std::size_t> n1_write_at(const File& file, std::uint64_t offset,
                                std::span<const std::byte> src) {
    const Admit a =
        admit_data(file.native_handle() < 0, file.access(), true, offset, src.size());
    if (a == Admit::complete_empty)
        return std::size_t{0};
    if (a != Admit::execute)
        return sluice::make_unexpected<std::size_t>(admit_rejection(a));
    if (src.data() == nullptr)
        return sluice::make_unexpected<std::size_t>(code_only(IoError::Code::invalid_argument));
    ssize_t n;
    do {
        n = ::pwrite(file.native_handle(), src.data(), src.size(), static_cast<off_t>(offset));
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return sluice::make_unexpected<std::size_t>(sluice::from_errno_value(errno));
    return static_cast<std::size_t>(n);
}

CompositionOutcome composition_stop(std::size_t confirmed, CompositionEnd end,
                                    EffectCertainty certainty, std::optional<IoError> error) {
    CompositionOutcome out;
    out.confirmed_bytes = confirmed;
    out.end = end;
    out.remaining = certainty;
    out.error = error;
    return out;
}

Result<CompositionOutcome> n1_read_exact_at(const File& file, std::uint64_t offset,
                                            std::span<std::byte> dst) {
    const Admit a =
        admit_data(file.native_handle() < 0, file.access(), false, offset, dst.size());
    if (a == Admit::complete_empty)
        return CompositionOutcome{};
    if (a != Admit::execute)
        return sluice::make_unexpected<CompositionOutcome>(admit_rejection(a));
    if (dst.data() == nullptr)
        return sluice::make_unexpected<CompositionOutcome>(
            code_only(IoError::Code::invalid_argument));
    const int fd = file.native_handle();
    CompositionOutcome out;
    while (out.confirmed_bytes < dst.size()) {
        const std::size_t remaining = dst.size() - out.confirmed_bytes;
        ssize_t n;
        do {
            n = ::pread(fd, dst.data() + out.confirmed_bytes, remaining,
                        static_cast<off_t>(offset + out.confirmed_bytes));
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            return composition_stop(out.confirmed_bytes, CompositionEnd::primitive_error,
                                    EffectCertainty::unknown, sluice::from_errno_value(errno));
        const std::size_t moved = static_cast<std::size_t>(n);
        if (moved > remaining)
            return composition_stop(out.confirmed_bytes, CompositionEnd::primitive_error,
                                    EffectCertainty::unknown,
                                    code_only(IoError::Code::invalid_state));
        out.confirmed_bytes += moved;
        if (out.confirmed_bytes == dst.size()) {
            out.end = CompositionEnd::complete;
            out.remaining = EffectCertainty::accounted;
            return out;
        }
        if (moved == 0)
            return composition_stop(out.confirmed_bytes, CompositionEnd::eof_before_full,
                                    EffectCertainty::accounted, std::nullopt);
    }
    return out;
}

Result<CompositionOutcome> n1_write_all_at(const File& file, std::uint64_t offset,
                                           std::span<const std::byte> src) {
    const Admit a =
        admit_data(file.native_handle() < 0, file.access(), true, offset, src.size());
    if (a == Admit::complete_empty)
        return CompositionOutcome{};
    if (a != Admit::execute)
        return sluice::make_unexpected<CompositionOutcome>(admit_rejection(a));
    if (src.data() == nullptr)
        return sluice::make_unexpected<CompositionOutcome>(
            code_only(IoError::Code::invalid_argument));
    const int fd = file.native_handle();
    CompositionOutcome out;
    while (out.confirmed_bytes < src.size()) {
        const std::size_t remaining = src.size() - out.confirmed_bytes;
        ssize_t n;
        do {
            n = ::pwrite(fd, src.data() + out.confirmed_bytes, remaining,
                         static_cast<off_t>(offset + out.confirmed_bytes));
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            return composition_stop(out.confirmed_bytes, CompositionEnd::primitive_error,
                                    EffectCertainty::unknown, sluice::from_errno_value(errno));
        const std::size_t moved = static_cast<std::size_t>(n);
        if (moved > remaining)
            return composition_stop(out.confirmed_bytes, CompositionEnd::primitive_error,
                                    EffectCertainty::unknown,
                                    code_only(IoError::Code::invalid_state));
        out.confirmed_bytes += moved;
        if (out.confirmed_bytes == src.size()) {
            out.end = CompositionEnd::complete;
            out.remaining = EffectCertainty::accounted;
            return out;
        }
        if (moved == 0)
            return composition_stop(out.confirmed_bytes, CompositionEnd::write_no_progress,
                                    EffectCertainty::accounted, std::nullopt);
    }
    return out;
}

Result<CompositionOutcome> n1_read_exact(const File& file, std::span<std::byte> dst) {
    const Admit a =
        admit_data(file.native_handle() < 0, file.access(), false, 0, dst.size());
    if (a == Admit::complete_empty)
        return CompositionOutcome{};
    if (a != Admit::execute)
        return sluice::make_unexpected<CompositionOutcome>(admit_rejection(a));
    if (dst.data() == nullptr)
        return sluice::make_unexpected<CompositionOutcome>(
            code_only(IoError::Code::invalid_argument));
    const int fd = file.native_handle();
    CompositionOutcome out;
    while (out.confirmed_bytes < dst.size()) {
        const std::size_t remaining = dst.size() - out.confirmed_bytes;
        ssize_t n;
        do {
            n = ::read(fd, dst.data() + out.confirmed_bytes, remaining);
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            return composition_stop(out.confirmed_bytes, CompositionEnd::primitive_error,
                                    EffectCertainty::unknown, sluice::from_errno_value(errno));
        const std::size_t moved = static_cast<std::size_t>(n);
        if (moved > remaining)
            return composition_stop(out.confirmed_bytes, CompositionEnd::primitive_error,
                                    EffectCertainty::unknown,
                                    code_only(IoError::Code::invalid_state));
        out.confirmed_bytes += moved;
        if (out.confirmed_bytes == dst.size()) {
            out.end = CompositionEnd::complete;
            out.remaining = EffectCertainty::accounted;
            return out;
        }
        if (moved == 0)
            return composition_stop(out.confirmed_bytes, CompositionEnd::eof_before_full,
                                    EffectCertainty::accounted, std::nullopt);
    }
    return out;
}

Result<CompositionOutcome> n1_write_all(const File& file, std::span<const std::byte> src) {
    const Admit a =
        admit_data(file.native_handle() < 0, file.access(), true, 0, src.size());
    if (a == Admit::complete_empty)
        return CompositionOutcome{};
    if (a != Admit::execute)
        return sluice::make_unexpected<CompositionOutcome>(admit_rejection(a));
    if (src.data() == nullptr)
        return sluice::make_unexpected<CompositionOutcome>(
            code_only(IoError::Code::invalid_argument));
    const int fd = file.native_handle();
    CompositionOutcome out;
    while (out.confirmed_bytes < src.size()) {
        const std::size_t remaining = src.size() - out.confirmed_bytes;
        ssize_t n;
        do {
            n = ::write(fd, src.data() + out.confirmed_bytes, remaining);
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            return composition_stop(out.confirmed_bytes, CompositionEnd::primitive_error,
                                    EffectCertainty::unknown, sluice::from_errno_value(errno));
        const std::size_t moved = static_cast<std::size_t>(n);
        if (moved > remaining)
            return composition_stop(out.confirmed_bytes, CompositionEnd::primitive_error,
                                    EffectCertainty::unknown,
                                    code_only(IoError::Code::invalid_state));
        out.confirmed_bytes += moved;
        if (out.confirmed_bytes == src.size()) {
            out.end = CompositionEnd::complete;
            out.remaining = EffectCertainty::accounted;
            return out;
        }
        if (moved == 0)
            return composition_stop(out.confirmed_bytes, CompositionEnd::write_no_progress,
                                    EffectCertainty::accounted, std::nullopt);
    }
    return out;
}

struct NativeFile {
    int fd = -1;
    FileAccess access = FileAccess::read_only;
};

Result<NativeFile> n1_open(const std::string& path, FileOpen mode) {
    if (path.find('\0') != std::string::npos)
        return sluice::make_unexpected<NativeFile>(code_only(IoError::Code::invalid_argument));
    if (mode.access == FileAccess::read_only && mode.contents == FileInitialContents::truncate)
        return sluice::make_unexpected<NativeFile>(code_only(IoError::Code::invalid_argument));
    int flags = O_CLOEXEC;
    switch (mode.access) {
    case FileAccess::read_only:
        flags |= O_RDONLY;
        break;
    case FileAccess::write_only:
        flags |= O_WRONLY;
        break;
    case FileAccess::read_write:
        flags |= O_RDWR;
        break;
    }
    switch (mode.existence) {
    case FileExistence::open_existing:
        break;
    case FileExistence::create_if_missing:
        flags |= O_CREAT;
        break;
    case FileExistence::create_new:
        flags |= O_CREAT | O_EXCL;
        break;
    }
    if (mode.contents == FileInitialContents::truncate)
        flags |= O_TRUNC;
    const int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0)
        return sluice::make_unexpected<NativeFile>(sluice::from_errno_value(errno));
    return NativeFile{fd, mode.access};
}

// One-shot ownership consumption: the first close attempt is terminal, also on
// EINTR; a consumed (or never-opened) handle is a no-op success.
Result<void> n1_close(NativeFile& f) noexcept {
    if (f.fd < 0)
        return {};
    const int fd = std::exchange(f.fd, -1);
    if (::close(fd) != 0)
        return sluice::make_unexpected_void(sluice::from_errno_value(errno));
    return {};
}

Result<FileInfo> n1_file_info(const NativeFile& f) {
    if (f.fd < 0)
        return sluice::make_unexpected<FileInfo>(code_only(IoError::Code::invalid_state));
    struct ::stat st {};
    int rc;
    do {
        rc = ::fstat(f.fd, &st);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
        return sluice::make_unexpected<FileInfo>(sluice::from_errno_value(errno));
    FileInfo info;
    info.kind = S_ISREG(st.st_mode) ? FileKind::regular : FileKind::other;
    info.size = static_cast<std::uint64_t>(st.st_size);
    info.identity = FileIdentity{static_cast<std::uint64_t>(st.st_dev),
                                 static_cast<std::uint64_t>(st.st_ino)};
    return info;
}

Result<std::uint64_t> n1_size(const NativeFile& f) {
    auto info = n1_file_info(f);
    if (!info.has_value())
        return sluice::make_unexpected<std::uint64_t>(info.error());
    return info.value().size;
}

// --- A3: no-syscall diagnostic stub (attribution only) ----------------------

Result<std::size_t> a3_stub_read_at(const File& file, std::uint64_t offset,
                                    std::span<std::byte> dst) {
    const Admit a =
        admit_data(file.native_handle() < 0, file.access(), false, offset, dst.size());
    if (a == Admit::complete_empty)
        return std::size_t{0};
    if (a != Admit::execute)
        return sluice::make_unexpected<std::size_t>(admit_rejection(a));
    if (dst.data() == nullptr)
        return sluice::make_unexpected<std::size_t>(code_only(IoError::Code::invalid_argument));
    return dst.size();
}

// --- shared harness ----------------------------------------------------------

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

std::uint64_t fnv1a(const std::byte* p, std::size_t n, std::uint64_t h) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i]));
        h *= kFnvPrime;
    }
    return h;
}

std::uint64_t mix_info(std::uint64_t size, std::uint64_t dev, std::uint64_t ino,
                       unsigned kind_code) {
    std::uint64_t h = size;
    h = h * 131 + dev;
    h = h * 131 + ino;
    h = h * 131 + kind_code;
    return h;
}

std::uint64_t mix_stat(const struct ::stat& st) {
    return mix_info(static_cast<std::uint64_t>(st.st_size),
                    static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino),
                    S_ISREG(st.st_mode) ? 1u : 2u);
}

std::uint64_t mix_file_info(const FileInfo& info) {
    const std::uint64_t dev = info.identity ? info.identity->device : 0;
    const std::uint64_t ino = info.identity ? info.identity->inode : 0;
    return mix_info(info.size, dev, ino, info.kind == FileKind::regular ? 1u : 2u);
}

enum class Arm {
    A0, A1, A2, A3, A1X, AX, A1C, AC,
    W0, W1, W2, W1X, WX, W1C, WC,
    M0OC, M0FI, M1OC, M1FI, M1SZ, M2OC, M2FI, M2SZ,
    AV0, AV1, AVS, WV0, WV1, WVS,
    RPL0, RPLT, RPLD,
    CP1, CP1S, CP2, CP3,
};

enum class Family {
    read, write, open_close, file_info, size,
    vectored_read, vectored_write, planner, copy,
};
enum class Pattern { seq, rand, tail };
enum class State { normal, closed, eintr };

const char* arm_name(Arm arm) {
    switch (arm) {
    case Arm::A0: return "A0_raw_pread";
    case Arm::A1: return "A1_native_read_at";
    case Arm::A2: return "A2_sluice_read_at";
    case Arm::A3: return "A3_nosyscall_stub";
    case Arm::A1X: return "A1x_native_read_exact_at";
    case Arm::AX: return "AX_sluice_read_exact_at";
    case Arm::A1C: return "A1c_native_read_exact";
    case Arm::AC: return "AC_sluice_read_exact";
    case Arm::W0: return "W0_raw_pwrite";
    case Arm::W1: return "W1_native_write_at";
    case Arm::W2: return "W2_sluice_write_at";
    case Arm::W1X: return "W1x_native_write_all_at";
    case Arm::WX: return "WX_sluice_write_all_at";
    case Arm::W1C: return "W1c_native_write_all";
    case Arm::WC: return "WC_sluice_write_all";
    case Arm::M0OC: return "M0_raw_open_close";
    case Arm::M0FI: return "M0_raw_fstat";
    case Arm::M1OC: return "M1_native_open_close";
    case Arm::M1FI: return "M1_native_file_info";
    case Arm::M1SZ: return "M1_native_size";
    case Arm::M2OC: return "M2_sluice_open_close";
    case Arm::M2FI: return "M2_sluice_file_info";
    case Arm::M2SZ: return "M2_sluice_size";
    case Arm::AV0: return "AV0_kpread_group";
    case Arm::AV1: return "AV1_preadv_group";
    case Arm::AVS: return "AVS_sluice_kread_at_group";
    case Arm::WV0: return "WV0_kpwrite_group";
    case Arm::WV1: return "WV1_pwritev_group";
    case Arm::WVS: return "WVS_sluice_kwrite_at_group";
    case Arm::RPL0: return "RPL0_unmerged_replay";
    case Arm::RPLT: return "RPLT_sweep_merge";
    case Arm::RPLD: return "RPLD_union_dedup";
    case Arm::CP1: return "CP1_pread_pwrite_loop";
    case Arm::CP1S: return "CP1S_sluice_read_write_loop";
    case Arm::CP2: return "CP2_copy_file_range";
    case Arm::CP3: return "CP3_sendfile";
    }
    return "?";
}

Family arm_family(Arm arm) {
    switch (arm) {
    case Arm::A0:
    case Arm::A1:
    case Arm::A2:
    case Arm::A3:
    case Arm::A1X:
    case Arm::AX:
    case Arm::A1C:
    case Arm::AC:
        return Family::read;
    case Arm::W0:
    case Arm::W1:
    case Arm::W2:
    case Arm::W1X:
    case Arm::WX:
    case Arm::W1C:
    case Arm::WC:
        return Family::write;
    case Arm::M0OC:
    case Arm::M1OC:
    case Arm::M2OC:
        return Family::open_close;
    case Arm::M0FI:
    case Arm::M1FI:
    case Arm::M2FI:
        return Family::file_info;
    case Arm::M1SZ:
    case Arm::M2SZ:
        return Family::size;
    case Arm::AV0:
    case Arm::AV1:
    case Arm::AVS:
        return Family::vectored_read;
    case Arm::WV0:
    case Arm::WV1:
    case Arm::WVS:
        return Family::vectored_write;
    case Arm::RPL0:
    case Arm::RPLT:
    case Arm::RPLD:
        return Family::planner;
    case Arm::CP1:
    case Arm::CP1S:
    case Arm::CP2:
    case Arm::CP3:
        return Family::copy;
    }
    return Family::size;
}

struct RunResult {
    double ns_per_op = 0;
    std::uint64_t checksum = 0;
    std::uint64_t content_hash = 0;
    int content_ok = -1; // 1 verified, 0 mismatch, -1 n/a, -2 skipped
    bool ok = false;
};

// Trailing key=value tokens accepted anywhere after <file>; they never occupy
// a positional optional slot, so existing token positions are unchanged.
struct Trailing {
    std::size_t iovcnt = 4;
    std::size_t merge = 0;
    int dedup = 0;
    std::string trace = "g4k";
    std::string dst;
};

struct Context {
    Arm arm;
    Family family;
    std::size_t block;
    std::size_t ops;
    std::size_t warmup;
    std::size_t reps;
    std::string path;
    FileOpen open_mode;
    int raw_fd = -1;
    std::optional<File> file;
    NativeFile native;
    std::uint64_t file_bytes = 0;
    std::size_t file_blocks = 1;
    std::vector<std::uint64_t> offsets;
    std::byte* buf = nullptr;
    bool closed_state = false;
    bool eintr_state = false;
    std::uint64_t cursor_pos = 0;
    bool cursor_tracked = true;
    Trailing opt;
    std::size_t group_blocks = 1;
    std::vector<std::byte> vbuf;
    struct VSlice {
        std::size_t off;
        std::byte* ptr;
        std::size_t len;
    };
    std::vector<VSlice> vio;
    std::vector<struct ::iovec> iov;
    std::vector<std::size_t> eff;
    std::uint64_t fill_len_hash = 0;
    bool timed = false;
    std::uint64_t n_sys = 0;
    std::uint64_t n_issued = 0;
    struct PReq {
        std::uint64_t off;
        std::size_t len;
    };
    std::vector<std::vector<PReq>> preqs;
    std::vector<std::size_t> porder;
    std::vector<std::byte> pdst;
    std::vector<std::byte> pref;
    std::vector<std::byte> pstaging;
    std::uint64_t req_digest = 0;
    std::uint64_t ref_digest = 0;
    std::uint64_t outcome_digest = 0;
    int dst_fd = -1;
    std::optional<File> dst_file;
};

// Shared-cursor arms reach offsets[i] through the kernel file cursor of the
// File handle. lseek runs only when the tracked cursor differs from the
// requested offset (seq streams seek-free; rand/tail reposition per op); a
// rejected op drops tracking so the next op repositions unconditionally.
void position_shared_cursor(Context& c, std::uint64_t off) {
    if (c.closed_state || (c.cursor_tracked && c.cursor_pos == off))
        return;
    (void)::lseek(c.file->native_handle(), static_cast<off_t>(off), SEEK_SET);
    c.cursor_pos = off;
    c.cursor_tracked = true;
}

std::uint64_t vectored_read_op(Context& c, std::size_t i);
std::uint64_t vectored_write_op(Context& c, std::size_t i);
std::uint64_t planner_op(Context& c, std::size_t i);
std::uint64_t copy_op(Context& c, std::size_t i);

std::uint64_t one_op(Context& c, std::size_t i) {
    const std::uint64_t off = c.offsets[i];
    const std::size_t len = c.block;
    const File& file = *c.file;
    switch (c.arm) {
    case Arm::A0:
        return static_cast<std::uint64_t>(::pread(c.raw_fd, c.buf, len, static_cast<off_t>(off)));
    case Arm::A1: {
        auto r = n1_read_at(file, off, std::span<std::byte>(c.buf, len));
        return r.has_value() ? r.value() : error_sink_value(r.error());
    }
    case Arm::A2: {
        auto r = sluice::blocking::read_at(file, off, std::span<std::byte>(c.buf, len));
        return r.has_value() ? r.value() : error_sink_value(r.error());
    }
    case Arm::A3: {
        auto r = a3_stub_read_at(file, off, std::span<std::byte>(c.buf, len));
        return r.has_value() ? r.value() : error_sink_value(r.error());
    }
    case Arm::A1X: {
        auto r = n1_read_exact_at(file, off, std::span<std::byte>(c.buf, len));
        return r.has_value() ? r.value().confirmed_bytes : error_sink_value(r.error());
    }
    case Arm::AX: {
        auto r = sluice::blocking::read_exact_at(file, off, std::span<std::byte>(c.buf, len));
        return r.has_value() ? r.value().confirmed_bytes : error_sink_value(r.error());
    }
    case Arm::A1C:
    case Arm::AC: {
        position_shared_cursor(c, off);
        auto r = c.arm == Arm::A1C
                     ? n1_read_exact(file, std::span<std::byte>(c.buf, len))
                     : sluice::blocking::read_exact(file, std::span<std::byte>(c.buf, len));
        if (r.has_value()) {
            c.cursor_pos = off + r.value().confirmed_bytes;
            return r.value().confirmed_bytes;
        }
        c.cursor_tracked = false;
        return error_sink_value(r.error());
    }
    case Arm::W0:
        return static_cast<std::uint64_t>(
            ::pwrite(c.raw_fd, c.buf, len, static_cast<off_t>(off)));
    case Arm::W1: {
        auto r = n1_write_at(file, off, std::span<const std::byte>(c.buf, len));
        return r.has_value() ? r.value() : error_sink_value(r.error());
    }
    case Arm::W2: {
        auto r = sluice::blocking::write_at(file, off, std::span<const std::byte>(c.buf, len));
        return r.has_value() ? r.value() : error_sink_value(r.error());
    }
    case Arm::W1X: {
        auto r = n1_write_all_at(file, off, std::span<const std::byte>(c.buf, len));
        return r.has_value() ? r.value().confirmed_bytes : error_sink_value(r.error());
    }
    case Arm::WX: {
        auto r = sluice::blocking::write_all_at(file, off, std::span<const std::byte>(c.buf, len));
        return r.has_value() ? r.value().confirmed_bytes : error_sink_value(r.error());
    }
    case Arm::W1C:
    case Arm::WC: {
        position_shared_cursor(c, off);
        auto r = c.arm == Arm::W1C
                     ? n1_write_all(file, std::span<const std::byte>(c.buf, len))
                     : sluice::blocking::write_all(file, std::span<const std::byte>(c.buf, len));
        if (r.has_value()) {
            c.cursor_pos = off + r.value().confirmed_bytes;
            return r.value().confirmed_bytes;
        }
        c.cursor_tracked = false;
        return error_sink_value(r.error());
    }
    case Arm::M0OC: {
        const int fd = ::open(c.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return error_sink_value(sluice::from_errno_value(errno));
        if (::close(fd) != 0)
            return error_sink_value(sluice::from_errno_value(errno));
        return 1;
    }
    case Arm::M1OC: {
        auto r = n1_open(c.path, c.open_mode);
        if (!r.has_value())
            return error_sink_value(r.error());
        NativeFile nf = r.value();
        auto cl = n1_close(nf);
        if (!cl.has_value())
            return error_sink_value(cl.error());
        return 1;
    }
    case Arm::M2OC: {
        auto r = File::open(c.path, c.open_mode);
        if (!r.has_value())
            return error_sink_value(r.error());
        File f = std::move(r).value();
        auto cl = f.close();
        if (!cl.has_value())
            return error_sink_value(cl.error());
        return 1;
    }
    case Arm::M0FI: {
        struct ::stat st {};
        if (::fstat(c.raw_fd, &st) != 0)
            return error_sink_value(sluice::from_errno_value(errno));
        return mix_stat(st);
    }
    case Arm::M1FI: {
        auto r = n1_file_info(c.native);
        return r.has_value() ? mix_file_info(r.value()) : error_sink_value(r.error());
    }
    case Arm::M2FI: {
        auto r = sluice::blocking::file_info(file);
        return r.has_value() ? mix_file_info(r.value()) : error_sink_value(r.error());
    }
    case Arm::M1SZ: {
        auto r = n1_size(c.native);
        return r.has_value() ? r.value() : error_sink_value(r.error());
    }
    case Arm::M2SZ: {
        auto r = sluice::blocking::size(file);
        return r.has_value() ? r.value() : error_sink_value(r.error());
    }
    case Arm::AV0:
    case Arm::AV1:
    case Arm::AVS:
        return vectored_read_op(c, i);
    case Arm::WV0:
    case Arm::WV1:
    case Arm::WVS:
        return vectored_write_op(c, i);
    case Arm::RPL0:
    case Arm::RPLT:
    case Arm::RPLD:
        return planner_op(c, i);
    case Arm::CP1:
    case Arm::CP1S:
    case Arm::CP2:
    case Arm::CP3:
        return copy_op(c, i);
    }
    return 0;
}

// --- M1 vectored arms: one contiguous extent split across k buffers ---------

std::byte pattern_byte(std::uint64_t pos) {
    return static_cast<std::byte>(pos * 1103515245u >> 13 & 0xff);
}

std::uint64_t fnv_byte(std::uint64_t h, std::byte b) {
    h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(b));
    return h * kFnvPrime;
}

std::uint64_t fnv_u64(std::uint64_t h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        h = fnv_byte(h, static_cast<std::byte>(v >> (8 * i)));
    return h;
}

std::uint64_t vectored_read_op(Context& c, std::size_t gi) {
    const std::uint64_t base = c.offsets[gi];
    const int fd = c.raw_fd;
    switch (c.arm) {
    case Arm::AV0: {
        std::uint64_t total = 0;
        for (std::size_t j = 0; j < c.vio.size(); ++j) {
            ssize_t n;
            do {
                n = ::pread(fd, c.vio[j].ptr, c.vio[j].len,
                            static_cast<off_t>(base + c.vio[j].off));
            } while (n < 0 && errno == EINTR);
            c.eff[j] = n > 0 ? static_cast<std::size_t>(n) : 0;
            total += static_cast<std::uint64_t>(static_cast<std::int64_t>(n));
        }
        if (c.timed) {
            c.n_sys += c.vio.size();
            c.n_issued += c.block;
        }
        return total;
    }
    case Arm::AV1: {
        ssize_t n;
        do {
            n = ::preadv(fd, c.iov.data(), static_cast<int>(c.iov.size()),
                         static_cast<off_t>(base));
        } while (n < 0 && errno == EINTR);
        const std::size_t t = n > 0 ? static_cast<std::size_t>(n) : 0;
        for (std::size_t j = 0; j < c.vio.size(); ++j)
            c.eff[j] = t > c.vio[j].off ? std::min(t - c.vio[j].off, c.vio[j].len) : 0;
        if (c.timed) {
            c.n_sys += 1;
            c.n_issued += c.block;
        }
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(n));
    }
    case Arm::AVS: {
        std::uint64_t total = 0;
        for (std::size_t j = 0; j < c.vio.size(); ++j) {
            auto r = sluice::blocking::read_at(*c.file, base + c.vio[j].off,
                                               std::span<std::byte>(c.vio[j].ptr, c.vio[j].len));
            if (r.has_value()) {
                c.eff[j] = r.value();
                total += r.value();
            } else {
                c.eff[j] = 0;
                total += error_sink_value(r.error());
            }
        }
        if (c.timed) {
            c.n_sys += c.vio.size();
            c.n_issued += c.block;
        }
        return total;
    }
    default:
        return 0;
    }
}

std::uint64_t vectored_write_op(Context& c, std::size_t gi) {
    const std::uint64_t base = c.offsets[gi];
    const int fd = c.raw_fd;
    switch (c.arm) {
    case Arm::WV0: {
        std::uint64_t total = 0;
        for (std::size_t j = 0; j < c.vio.size(); ++j) {
            ssize_t n;
            do {
                n = ::pwrite(fd, c.vio[j].ptr, c.vio[j].len,
                             static_cast<off_t>(base + c.vio[j].off));
            } while (n < 0 && errno == EINTR);
            total += static_cast<std::uint64_t>(static_cast<std::int64_t>(n));
        }
        if (c.timed) {
            c.n_sys += c.vio.size();
            c.n_issued += c.block;
        }
        return total;
    }
    case Arm::WV1: {
        ssize_t n;
        do {
            n = ::pwritev(fd, c.iov.data(), static_cast<int>(c.iov.size()),
                          static_cast<off_t>(base));
        } while (n < 0 && errno == EINTR);
        if (c.timed) {
            c.n_sys += 1;
            c.n_issued += c.block;
        }
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(n));
    }
    case Arm::WVS: {
        std::uint64_t total = 0;
        for (std::size_t j = 0; j < c.vio.size(); ++j) {
            auto r = sluice::blocking::write_at(
                *c.file, base + c.vio[j].off,
                std::span<const std::byte>(c.vio[j].ptr, c.vio[j].len));
            total += r.has_value() ? r.value() : error_sink_value(r.error());
        }
        if (c.timed) {
            c.n_sys += c.vio.size();
            c.n_issued += c.block;
        }
        return total;
    }
    default:
        return 0;
    }
}

// --- M2 planner arms: request replay with optional sort+sweep merge ---------

constexpr std::size_t kPlannerRequests = 8;

std::uint64_t lcg_step(std::uint64_t& s) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return s >> 33;
}

// Deterministic request-set generator. Every arm regenerates the identical
// stream from (trace, seed, size, file_bytes); the digest lands in the cell
// header and the equality gate refuses drift.
std::vector<std::vector<Context::PReq>> gen_requests(const std::string& trace, std::size_t ops,
                                                     std::size_t len, std::uint64_t file_bytes,
                                                     std::uint64_t seed) {
    const std::size_t k = kPlannerRequests;
    std::vector<std::vector<Context::PReq>> out(ops);
    std::uint64_t s = seed;
    std::size_t span_max = 0;
    if (trace == "g4k")
        span_max = k * len + (k - 1) * 8192;
    else if (trace == "ov30")
        span_max = 5 * len + 4 * 2048 + len / 2;
    const std::uint64_t room = file_bytes > span_max ? file_bytes - span_max : 1;
    for (std::size_t g = 0; g < ops; ++g) {
        out[g].resize(k);
        if (trace == "eofmix") {
            for (std::size_t j = 0; j < k; ++j)
                out[g][j] = {file_bytes - 2 * len + j * (len / 2), len};
            continue;
        }
        const std::uint64_t base = (static_cast<std::uint64_t>(g) * span_max) % room;
        const std::size_t gap_max = trace == "g4k" ? 8192 : 2048;
        std::uint64_t off = base;
        for (std::size_t j = 0; j < k; ++j) {
            out[g][j] = {off, len};
            if (j + 1 < k)
                off += len + lcg_step(s) % (gap_max + 1);
        }
        if (trace == "ov30") {
            out[g][5] = out[g][1];
            out[g][6] = {out[g][2].off + len / 2, len};
            out[g][7] = out[g][3];
        }
    }
    return out;
}

std::size_t planner_effective_threshold(const Context& c) {
    return c.arm == Arm::RPLD || c.opt.dedup ? 0 : c.opt.merge;
}

std::uint64_t planner_op(Context& c, std::size_t gi) {
    const auto& reqs = c.preqs[gi];
    const std::size_t k = reqs.size();
    switch (c.arm) {
    case Arm::RPL0: {
        std::uint64_t total = 0;
        for (std::size_t j = 0; j < k; ++j) {
            ssize_t n;
            do {
                n = ::pread(c.raw_fd, c.pdst.data() + j * c.block, reqs[j].len,
                            static_cast<off_t>(reqs[j].off));
            } while (n < 0 && errno == EINTR);
            c.eff[j] = n > 0 ? static_cast<std::size_t>(n) : 0;
            total += static_cast<std::uint64_t>(static_cast<std::int64_t>(n));
            if (c.timed) {
                c.n_sys += 1;
                c.n_issued += reqs[j].len;
            }
        }
        return total;
    }
    case Arm::RPLT:
    case Arm::RPLD: {
        const std::size_t t = planner_effective_threshold(c);
        c.porder.resize(k);
        for (std::size_t j = 0; j < k; ++j)
            c.porder[j] = j;
        std::sort(c.porder.begin(), c.porder.end(),
                  [&reqs](std::size_t a, std::size_t b) { return reqs[a].off < reqs[b].off; });
        std::uint64_t total = 0;
        std::size_t i = 0;
        while (i < k) {
            const std::size_t first = c.porder[i];
            std::uint64_t gs = reqs[first].off;
            std::uint64_t ge = reqs[first].off + reqs[first].len;
            std::size_t g = i + 1;
            while (g < k && reqs[c.porder[g]].off <= ge + t) {
                const auto& rq = reqs[c.porder[g]];
                ge = std::max(ge, rq.off + rq.len);
                ++g;
            }
            ssize_t n;
            do {
                n = ::pread(c.raw_fd, c.pstaging.data(), static_cast<std::size_t>(ge - gs),
                            static_cast<off_t>(gs));
            } while (n < 0 && errno == EINTR);
            if (c.timed) {
                c.n_sys += 1;
                c.n_issued += static_cast<std::uint64_t>(ge - gs);
            }
            total += static_cast<std::uint64_t>(static_cast<std::int64_t>(n));
            for (std::size_t m = i; m < g; ++m) {
                const std::size_t j = c.porder[m];
                const auto& rq = reqs[j];
                if (n <= 0 || gs + static_cast<std::uint64_t>(n) <= rq.off) {
                    c.eff[j] = 0;
                    continue;
                }
                const std::size_t moved = std::min(
                    static_cast<std::size_t>(gs + static_cast<std::uint64_t>(n) - rq.off),
                    rq.len);
                std::memcpy(c.pdst.data() + j * c.block,
                            c.pstaging.data() + (rq.off - gs), moved);
                c.eff[j] = moved;
            }
            i = g;
        }
        return total;
    }
    default:
        return 0;
    }
}

// Reference pass (serialized independent replay into a separate arena) plus
// the per-request oracle: the arm executes against poisoned destinations and
// every request is compared on count, prefix content and untouched tail.
bool planner_reference_and_verify(Context& c) {
    if (c.closed_state)
        return true;
    std::uint64_t refh = kFnvBasis;
    std::uint64_t outh = kFnvBasis;
    bool ok = true;
    std::vector<std::size_t> rn(kPlannerRequests);
    for (std::size_t gi = 0; gi < c.ops; ++gi) {
        const auto& reqs = c.preqs[gi];
        for (std::size_t j = 0; j < reqs.size(); ++j) {
            ssize_t n;
            do {
                n = ::pread(c.raw_fd, c.pref.data() + j * c.block, reqs[j].len,
                            static_cast<off_t>(reqs[j].off));
            } while (n < 0 && errno == EINTR);
            rn[j] = n > 0 ? static_cast<std::size_t>(n) : 0;
            refh = fnv_u64(refh, rn[j]);
            for (std::size_t i = 0; i < rn[j]; ++i)
                refh = fnv_byte(refh, c.pref[j * c.block + i]);
        }
        for (std::size_t j = 0; j < reqs.size(); ++j)
            for (std::size_t i = 0; i < reqs[j].len; ++i)
                c.pdst[j * c.block + i] = ~pattern_byte(reqs[j].off + i);
        (void)planner_op(c, gi);
        for (std::size_t j = 0; j < reqs.size(); ++j) {
            outh = fnv_u64(outh, c.eff[j]);
            if (c.eff[j] != rn[j])
                ok = false;
            for (std::size_t i = 0; i < reqs[j].len; ++i) {
                const std::byte b = c.pdst[j * c.block + i];
                if (i < rn[j]) {
                    outh = fnv_byte(outh, b);
                    if (b != c.pref[j * c.block + i])
                        ok = false;
                } else if (b != ~pattern_byte(reqs[j].off + i)) {
                    ok = false;
                }
            }
        }
    }
    c.ref_digest = refh;
    c.outcome_digest = outh;
    return ok;
}

// --- M3 copy arms: file-to-file transfer per 64-KiB-class chunk ------------

// sendfile has no out-offset argument: the destination position is out_fd's
// shared cursor, advanced by each transfer. Chunks run sequentially from 0, so
// the cursor lands correctly as long as it is reset to 0 between timed
// windows (prep_copy_rep runs outside every timed window).
void prep_copy_rep(Context& c) {
    if (c.arm == Arm::CP3)
        (void)::lseek(c.dst_fd, 0, SEEK_SET);
}

std::uint64_t copy_op(Context& c, std::size_t i) {
    std::uint64_t off = static_cast<std::uint64_t>(i) * c.block;
    std::uint64_t remaining = c.block;
    std::uint64_t total = 0;
    switch (c.arm) {
    case Arm::CP1:
        while (remaining > 0) {
            ssize_t n;
            do {
                n = ::pread(c.raw_fd, c.buf, remaining, static_cast<off_t>(off));
            } while (n < 0 && errno == EINTR);
            if (c.timed) {
                c.n_sys += 1;
                c.n_issued += remaining;
            }
            if (n <= 0)
                return total + static_cast<std::uint64_t>(static_cast<std::int64_t>(n));
            std::size_t written = 0;
            while (written < static_cast<std::size_t>(n)) {
                const std::size_t len = static_cast<std::size_t>(n) - written;
                ssize_t p;
                do {
                    p = ::pwrite(c.dst_fd, c.buf + written, len,
                                 static_cast<off_t>(off + written));
                } while (p < 0 && errno == EINTR);
                if (c.timed) {
                    c.n_sys += 1;
                    c.n_issued += len;
                }
                if (p <= 0)
                    return total + written +
                           static_cast<std::uint64_t>(static_cast<std::int64_t>(p));
                written += static_cast<std::size_t>(p);
            }
            total += static_cast<std::uint64_t>(n);
            off += static_cast<std::uint64_t>(n);
            remaining -= static_cast<std::uint64_t>(n);
        }
        return total;
    case Arm::CP1S:
        while (remaining > 0) {
            auto rd = sluice::blocking::read_at(*c.file, off,
                                                std::span<std::byte>(c.buf, remaining));
            if (!rd.has_value())
                return total + error_sink_value(rd.error());
            if (c.timed) {
                c.n_sys += 1;
                c.n_issued += remaining;
            }
            const std::size_t n = rd.value();
            if (n == 0)
                return total;
            std::size_t written = 0;
            while (written < n) {
                const std::size_t len = n - written;
                auto wr = sluice::blocking::write_at(
                    *c.dst_file, off + written,
                    std::span<const std::byte>(c.buf + written, len));
                if (!wr.has_value())
                    return total + written + error_sink_value(wr.error());
                if (c.timed) {
                    c.n_sys += 1;
                    c.n_issued += len;
                }
                if (wr.value() == 0)
                    return total + written;
                written += wr.value();
            }
            total += n;
            off += n;
            remaining -= n;
        }
        return total;
    case Arm::CP2: {
        off_t oi = static_cast<off_t>(off);
        off_t oo = static_cast<off_t>(off);
        while (remaining > 0) {
            ssize_t r;
            do {
                r = ::copy_file_range(c.raw_fd, &oi, c.dst_fd, &oo, remaining, 0);
            } while (r < 0 && errno == EINTR);
            if (c.timed) {
                c.n_sys += 1;
                c.n_issued += remaining;
            }
            if (r <= 0)
                return total + static_cast<std::uint64_t>(static_cast<std::int64_t>(r));
            total += static_cast<std::uint64_t>(r);
            remaining -= static_cast<std::uint64_t>(r);
        }
        return total;
    }
    case Arm::CP3: {
        off_t oi = static_cast<off_t>(off);
        while (remaining > 0) {
            ssize_t r;
            do {
                r = ::sendfile(c.dst_fd, c.raw_fd, &oi, remaining);
            } while (r < 0 && errno == EINTR);
            if (c.timed) {
                c.n_sys += 1;
                c.n_issued += remaining;
            }
            if (r <= 0)
                return total + static_cast<std::uint64_t>(static_cast<std::int64_t>(r));
            total += static_cast<std::uint64_t>(r);
            remaining -= static_cast<std::uint64_t>(r);
        }
        return total;
    }
    default:
        return 0;
    }
}

ssize_t copy_read_chunk(int fd, void* p, std::size_t n, std::uint64_t off) {
    ssize_t r;
    do {
        r = ::pread(fd, p, n, static_cast<off_t>(off));
    } while (r < 0 && errno == EINTR);
    return r;
}

// Whole-file gate: the destination must equal the source over the comparable
// region and be exactly min(chunks*size, source) long.
RunResult verify_copy(Context& c, std::uint64_t& src_hash_out) {
    RunResult out;
    out.content_ok = -2;
    struct ::stat ds {};
    if (::fstat(c.dst_fd, &ds) != 0) {
        out.content_ok = 0;
        return out;
    }
    const std::uint64_t expect =
        std::min<std::uint64_t>(static_cast<std::uint64_t>(c.block) * c.ops, c.file_bytes);
    if (static_cast<std::uint64_t>(ds.st_size) != expect) {
        out.content_ok = 0;
        return out;
    }
    std::vector<std::byte> a(1u << 20), b(1u << 20);
    std::uint64_t ha = kFnvBasis, hb = kFnvBasis;
    bool ok = true;
    std::uint64_t pos = 0;
    while (pos < expect) {
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(a.size(), expect - pos));
        const ssize_t na = copy_read_chunk(c.raw_fd, a.data(), chunk, pos);
        const ssize_t nb = copy_read_chunk(c.dst_fd, b.data(), chunk, pos);
        if (na != static_cast<ssize_t>(chunk) || nb != static_cast<ssize_t>(chunk)) {
            ok = false;
            break;
        }
        if (std::memcmp(a.data(), b.data(), chunk) != 0)
            ok = false;
        ha = fnv1a(a.data(), chunk, ha);
        hb = fnv1a(b.data(), chunk, hb);
        pos += chunk;
    }
    src_hash_out = ha;
    out.content_hash = hb;
    out.content_ok = ok ? 1 : 0;
    return out;
}

// EOF-boundary oracle: each destination byte is poisoned with the bitwise
// complement of its expected file byte before the op, so the pass checks the
// effective prefix (pattern) and the untouched tail (~pattern) exactly; a byte
// can never satisfy both probes.
bool verify_vectored_pass(Context& c, std::uint64_t& concat_out) {
    const bool arm_verifiable = !(c.eintr_state && (c.arm == Arm::AV0 || c.arm == Arm::AV1));
    const std::size_t limit = std::min(c.ops, c.group_blocks);
    std::uint64_t concat = kFnvBasis;
    std::uint64_t lens = kFnvBasis;
    bool ok = true;
    for (std::size_t gi = 0; gi < limit; ++gi) {
        const std::uint64_t base = c.offsets[gi];
        for (std::size_t j = 0; j < c.vio.size(); ++j)
            for (std::size_t i = 0; i < c.vio[j].len; ++i)
                c.vio[j].ptr[i] = ~pattern_byte(base + c.vio[j].off + i);
        (void)vectored_read_op(c, gi);
        for (std::size_t j = 0; j < c.vio.size(); ++j) {
            lens = fnv_u64(lens, c.eff[j]);
            for (std::size_t i = 0; i < c.vio[j].len; ++i) {
                const std::byte expected = pattern_byte(base + c.vio[j].off + i);
                const std::byte b = c.vio[j].ptr[i];
                concat = fnv_byte(concat, b);
                if (!arm_verifiable)
                    continue;
                if (i < c.eff[j] ? b != expected : b != static_cast<std::byte>(~expected))
                    ok = false;
            }
        }
    }
    c.fill_len_hash = lens;
    concat_out = concat;
    return ok;
}

// Rewrite the whole scratch file with the uniform source pattern so every
// block position has an arm-independent expected content. Untimed.
bool prime_write_scratch(Context& c) {
    if (c.block == 0)
        return true;
    for (std::uint64_t off = 0; off < c.file_bytes; off += c.block) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(c.block, c.file_bytes - off));
        ssize_t n;
        do {
            n = ::pwrite(c.raw_fd, c.buf, chunk, static_cast<off_t>(off));
        } while (n < 0 && errno == EINTR);
        if (n < 0 || static_cast<std::size_t>(n) != chunk)
            return false;
    }
    return true;
}

bool verify_read_pass(Context& c) {
    // The stub fabricates counts and the raw arm legitimately surfaces EINTR
    // under the itimer state, so neither carries a content oracle there.
    const bool arm_verifiable = c.arm != Arm::A3 &&
                                !(c.eintr_state &&
                                  (c.arm == Arm::A0 || c.arm == Arm::A1X || c.arm == Arm::A1C));
    const std::size_t limit = std::min(c.ops, c.file_blocks);
    for (std::size_t i = 0; i < limit; ++i) {
        const std::uint64_t k = one_op(c, i);
        if (!arm_verifiable)
            continue;
        if (k > c.block)
            return false;
        if (!verify_pattern(c.buf, static_cast<std::size_t>(k), c.offsets[i]))
            return false;
    }
    return true;
}

RunResult verify_write_scratch(Context& c) {
    RunResult out;
    out.content_ok = -2;
    if (c.block == 0 || c.closed_state)
        return out;
    std::vector<std::byte> rb(1u << 20);
    std::uint64_t h = kFnvBasis;
    bool ok = true;
    std::uint64_t pos = 0;
    while (pos < c.file_bytes) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(rb.size(), c.file_bytes - pos));
        ssize_t n;
        do {
            n = ::pread(c.raw_fd, rb.data(), chunk, static_cast<off_t>(pos));
        } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            ok = false;
            break;
        }
        for (ssize_t i = 0; i < n; ++i)
            if (rb[i] != c.buf[(pos + static_cast<std::uint64_t>(i)) % c.block])
                ok = false;
        h = fnv1a(rb.data(), static_cast<std::size_t>(n), h);
        pos += static_cast<std::uint64_t>(n);
    }
    out.content_hash = h;
    out.content_ok = ok ? 1 : 0;
    return out;
}

RunResult run_arm(Context& c) {
    RunResult out;
    std::uint64_t sink = 0;
    if (c.family == Family::copy)
        prep_copy_rep(c);
    for (std::size_t i = 0; i < c.warmup; ++i)
        sink += one_op(c, i % c.ops);
    double best = 1e30;
    for (std::size_t r = 0; r < c.reps; ++r) {
        if (c.family == Family::copy)
            prep_copy_rep(c);
        const double t0 = now_ns();
        std::uint64_t s = 0;
        c.timed = true;
        for (std::size_t i = 0; i < c.ops; ++i)
            s += one_op(c, i);
        c.timed = false;
        const double dt = now_ns() - t0;
        sink += s;
        if (dt < best)
            best = dt;
    }
    out.ns_per_op = best / static_cast<double>(c.ops);
    // The read verify pass stays inside the rusage window (its op count is in
    // the denominator), matching the #600 read-arm caliber; the write readback
    // is a whole-file scan and runs outside the window in main.
    bool verified = true;
    if (c.family == Family::read && !c.closed_state)
        verified = verify_read_pass(c);
    if (c.family == Family::vectored_read && !c.closed_state) {
        std::uint64_t concat = 0;
        verified = verify_vectored_pass(c, concat);
        out.content_hash = concat;
        out.content_ok = verified ? 1 : 0;
    }
    if (c.family == Family::planner && !c.closed_state) {
        verified = planner_reference_and_verify(c);
        out.content_hash = c.outcome_digest;
        out.content_ok = verified ? 1 : 0;
    }
    out.checksum = sink;
    out.ok = verified;
    return out;
}

std::vector<std::uint64_t> build_offsets(Pattern pat, std::size_t ops, std::size_t block,
                                         std::size_t file_blocks, std::uint64_t file_bytes,
                                         std::uint64_t seed) {
    std::vector<std::uint64_t> offs(ops);
    std::uint64_t s = seed;
    for (std::size_t i = 0; i < ops; ++i) {
        switch (pat) {
        case Pattern::seq:
            offs[i] = (i % file_blocks) * static_cast<std::uint64_t>(block);
            break;
        case Pattern::rand:
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            offs[i] = ((s >> 33) % file_blocks) * static_cast<std::uint64_t>(block);
            break;
        case Pattern::tail:
            switch (i % 3) {
            case 0:
                offs[i] = file_bytes >= block ? file_bytes - block : 0;
                break;
            case 1:
                offs[i] = file_bytes >= block / 2 ? file_bytes - block / 2 : 0;
                break;
            default:
                offs[i] = file_bytes;
                break;
            }
            break;
        }
    }
    return offs;
}

void stop_periodic_timer() {
    itimerval t {};
    ::setitimer(ITIMER_REAL, &t, nullptr);
}

bool start_periodic_timer() {
    struct ::sigaction sa {};
    sa.sa_handler = +[](int) {};
    if (::sigaction(SIGALRM, &sa, nullptr) != 0)
        return false;
    itimerval t {};
    t.it_interval.tv_usec = 3;
    t.it_value.tv_usec = 3;
    return ::setitimer(ITIMER_REAL, &t, nullptr) == 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 7 || argc > 16) {
        std::fprintf(stderr,
                     "usage: %s arm size_bytes ops warmup reps file [seq|rand|tail] [seed] "
                     "[normal|closed|eintr] [prime|noprime] [key=value ...]\n"
                     "keys: iovcnt=<k> merge=<T> dedup=<0|1> trace=<g4k|ov30|eofmix> dst=<path>\n",
                     argv[0]);
        return 2;
    }
    const std::string arm_s = argv[1];
    Arm arm;
    if (arm_s == "a0") arm = Arm::A0;
    else if (arm_s == "a1") arm = Arm::A1;
    else if (arm_s == "a2") arm = Arm::A2;
    else if (arm_s == "a3") arm = Arm::A3;
    else if (arm_s == "a1x") arm = Arm::A1X;
    else if (arm_s == "ax") arm = Arm::AX;
    else if (arm_s == "a1c") arm = Arm::A1C;
    else if (arm_s == "ac") arm = Arm::AC;
    else if (arm_s == "w0") arm = Arm::W0;
    else if (arm_s == "w1") arm = Arm::W1;
    else if (arm_s == "w2") arm = Arm::W2;
    else if (arm_s == "w1x") arm = Arm::W1X;
    else if (arm_s == "wx") arm = Arm::WX;
    else if (arm_s == "w1c") arm = Arm::W1C;
    else if (arm_s == "wc") arm = Arm::WC;
    else if (arm_s == "m0oc") arm = Arm::M0OC;
    else if (arm_s == "m0fi") arm = Arm::M0FI;
    else if (arm_s == "m1oc") arm = Arm::M1OC;
    else if (arm_s == "m1fi") arm = Arm::M1FI;
    else if (arm_s == "m1sz") arm = Arm::M1SZ;
    else if (arm_s == "m2oc") arm = Arm::M2OC;
    else if (arm_s == "m2fi") arm = Arm::M2FI;
    else if (arm_s == "m2sz") arm = Arm::M2SZ;
    else if (arm_s == "av0") arm = Arm::AV0;
    else if (arm_s == "av1") arm = Arm::AV1;
    else if (arm_s == "avs") arm = Arm::AVS;
    else if (arm_s == "wv0") arm = Arm::WV0;
    else if (arm_s == "wv1") arm = Arm::WV1;
    else if (arm_s == "wvs") arm = Arm::WVS;
    else if (arm_s == "rpl0") arm = Arm::RPL0;
    else if (arm_s == "rplt") arm = Arm::RPLT;
    else if (arm_s == "rpld") arm = Arm::RPLD;
    else if (arm_s == "cp1") arm = Arm::CP1;
    else if (arm_s == "cp1s") arm = Arm::CP1S;
    else if (arm_s == "cp2") arm = Arm::CP2;
    else if (arm_s == "cp3") arm = Arm::CP3;
    else { std::fprintf(stderr, "bad arm\n"); return 2; }

    Trailing opt;
    Pattern pat = Pattern::seq;
    std::uint64_t seed = kDefaultSeed;
    State state = State::normal;
    // noprime: for pre-primed write scratches (e.g. cold-write cells where the
    // prime pass would refill the page cache that drop_caches just evicted).
    // Precondition: the file already holds the uniform block pattern.
    bool prime = true;
    std::size_t positional = 0;
    for (int a = 7; a < argc; ++a) {
        const std::string t = argv[a];
        const auto eq = t.find('=');
        if (eq != std::string::npos) {
            const std::string key = t.substr(0, eq);
            const std::string val = t.substr(eq + 1);
            if (key == "iovcnt") {
                const unsigned long v = std::strtoul(val.c_str(), nullptr, 10);
                if (v < 1 || v > 1024) { std::fprintf(stderr, "bad iovcnt\n"); return 2; }
                opt.iovcnt = static_cast<std::size_t>(v);
            } else if (key == "merge") {
                const unsigned long long v = std::strtoull(val.c_str(), nullptr, 10);
                opt.merge = static_cast<std::size_t>(v);
            } else if (key == "dedup") {
                if (val == "0") opt.dedup = 0;
                else if (val == "1") opt.dedup = 1;
                else { std::fprintf(stderr, "bad dedup\n"); return 2; }
            } else if (key == "trace") {
                if (val != "g4k" && val != "ov30" && val != "eofmix") {
                    std::fprintf(stderr, "bad trace\n");
                    return 2;
                }
                opt.trace = val;
            } else if (key == "dst") {
                if (val.empty()) { std::fprintf(stderr, "bad dst\n"); return 2; }
                opt.dst = val;
            } else {
                std::fprintf(stderr, "bad key=value token: %s\n", t.c_str());
                return 2;
            }
            continue;
        }
        switch (positional++) {
        case 0:
            if (t == "seq") pat = Pattern::seq;
            else if (t == "rand") pat = Pattern::rand;
            else if (t == "tail") pat = Pattern::tail;
            else { std::fprintf(stderr, "bad pattern\n"); return 2; }
            break;
        case 1:
            seed = std::strtoull(t.c_str(), nullptr, 10);
            break;
        case 2:
            if (t == "normal") state = State::normal;
            else if (t == "closed") state = State::closed;
            else if (t == "eintr") state = State::eintr;
            else { std::fprintf(stderr, "bad state\n"); return 2; }
            break;
        case 3:
            if (t == "prime") prime = true;
            else if (t == "noprime") prime = false;
            else { std::fprintf(stderr, "bad prime mode\n"); return 2; }
            break;
        default:
            std::fprintf(stderr, "too many positional tokens\n");
            return 2;
        }
    }

    const Family family = arm_family(arm);
    const bool data_family = family == Family::read || family == Family::write ||
                             family == Family::vectored_read || family == Family::vectored_write ||
                             family == Family::planner || family == Family::copy;
    if (pat == Pattern::tail && family != Family::read && family != Family::vectored_read) {
        std::fprintf(stderr, "tail pattern is read-family only\n");
        return 2;
    }
    if (state == State::eintr && !data_family) {
        std::fprintf(stderr, "eintr state is data-family only\n");
        return 2;
    }
    if (state == State::closed && family == Family::open_close) {
        std::fprintf(stderr, "closed state is meaningless for open/close arms\n");
        return 2;
    }
    if (state == State::closed && family == Family::copy) {
        std::fprintf(stderr, "closed state is meaningless for copy arms\n");
        return 2;
    }
    if (family == Family::copy && opt.dst.empty()) {
        std::fprintf(stderr, "copy arms require dst=<path>\n");
        return 2;
    }

    Context c;
    c.arm = arm;
    c.family = family;
    c.block = std::strtoull(argv[2], nullptr, 10);
    c.ops = std::strtoull(argv[3], nullptr, 10);
    c.warmup = std::strtoull(argv[4], nullptr, 10);
    c.reps = std::strtoull(argv[5], nullptr, 10);
    c.path = argv[6];
    if (c.ops == 0) { std::fprintf(stderr, "ops must be nonzero\n"); return 2; }
    c.opt = opt;
    const bool write_side = family == Family::write || family == Family::vectored_write;
    c.open_mode.access = write_side ? FileAccess::read_write : FileAccess::read_only;

    auto opened = File::open(c.path, c.open_mode);
    if (!opened.has_value()) {
        std::fprintf(stderr, "open failed\n");
        return 1;
    }
    c.file = std::move(opened).value();
    const int extra = write_side ? O_RDWR : O_RDONLY;
    c.raw_fd = ::open(c.path.c_str(), extra | O_CLOEXEC);
    if (c.raw_fd < 0) {
        std::fprintf(stderr, "native open failed\n");
        return 1;
    }
    struct ::stat st {};
    if (::fstat(c.raw_fd, &st) != 0) {
        std::fprintf(stderr, "fstat failed\n");
        return 1;
    }
    c.file_bytes = static_cast<std::uint64_t>(st.st_size);
    c.file_blocks = c.file_bytes / (c.block ? c.block : 1) > 0
                        ? static_cast<std::size_t>(c.file_bytes / (c.block ? c.block : 1))
                        : 1;
    c.native = NativeFile{c.raw_fd, c.open_mode.access};

    if (family == Family::copy) {
        c.dst_fd = ::open(opt.dst.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (c.dst_fd < 0) {
            std::fprintf(stderr, "dst open failed\n");
            return 1;
        }
        FileOpen dst_mode;
        dst_mode.access = FileAccess::read_write;
        dst_mode.existence = FileExistence::create_if_missing;
        dst_mode.contents = FileInitialContents::truncate;
        auto dst_opened = File::open(opt.dst, dst_mode);
        if (!dst_opened.has_value()) {
            std::fprintf(stderr, "dst File open failed\n");
            return 1;
        }
        c.dst_file = std::move(dst_opened).value();
    }

    std::vector<std::byte> buf(c.block ? c.block : 1);
    for (std::size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<std::byte>(i * 1103515245u >> 13 & 0xff);
    c.buf = buf.data();

    if (family == Family::vectored_read || family == Family::vectored_write) {
        if (c.block < opt.iovcnt) {
            std::fprintf(stderr, "size must be at least iovcnt for vectored arms\n");
            return 2;
        }
        c.group_blocks = c.file_bytes / c.block > 0
                             ? static_cast<std::size_t>(c.file_bytes / c.block)
                             : 1;
        c.offsets = build_offsets(pat, c.ops, c.block, c.group_blocks, c.file_bytes, seed);
        if (family == Family::vectored_read) {
            c.vbuf.resize(c.block);
            c.eff.assign(opt.iovcnt, 0);
        }
        std::size_t off = 0;
        for (std::size_t j = 0; j < opt.iovcnt; ++j) {
            const std::size_t len =
                c.block / opt.iovcnt + (j < c.block % opt.iovcnt ? 1 : 0);
            std::byte* base = family == Family::vectored_read ? c.vbuf.data() : c.buf;
            c.vio.push_back(Context::VSlice{off, base + off, len});
            c.iov.push_back(::iovec{.iov_base = c.vio[j].ptr, .iov_len = len});
            off += len;
        }
    } else {
        c.offsets = build_offsets(pat, c.ops, c.block, c.file_blocks, c.file_bytes, seed);
    }

    if (family == Family::planner) {
        if (c.block == 0) { std::fprintf(stderr, "size must be nonzero\n"); return 2; }
        if (opt.trace == "eofmix" && c.file_bytes < 2 * c.block) {
            std::fprintf(stderr, "fixture too small for eofmix trace\n");
            return 2;
        }
        c.preqs = gen_requests(opt.trace, c.ops, c.block, c.file_bytes, seed);
        std::uint64_t h = kFnvBasis;
        std::size_t staging = 0;
        for (const auto& group : c.preqs) {
            std::uint64_t lo = ~0ull;
            std::uint64_t hi = 0;
            for (const auto& rq : group) {
                h = fnv_u64(h, rq.off);
                h = fnv_u64(h, rq.len);
                lo = std::min(lo, rq.off);
                hi = std::max(hi, rq.off + rq.len);
            }
            staging = std::max(staging, static_cast<std::size_t>(hi - lo));
        }
        c.req_digest = h;
        c.pdst.assign(kPlannerRequests * c.block, std::byte{0});
        c.pref.assign(kPlannerRequests * c.block, std::byte{0});
        if (arm == Arm::RPLT || arm == Arm::RPLD)
            c.pstaging.assign(staging, std::byte{0});
        c.eff.assign(kPlannerRequests, 0);
    }

    if (state == State::closed) {
        (void)c.file->close();
        if (::close(c.raw_fd) != 0) {
            std::fprintf(stderr, "pre-run close failed\n");
            return 1;
        }
        c.native.fd = -1;
        c.closed_state = true;
    } else if (write_side && prime) {
        if (!prime_write_scratch(c)) {
            std::fprintf(stderr, "scratch priming failed\n");
            return 1;
        }
    }
    if (state == State::eintr) {
        if (!start_periodic_timer()) {
            std::fprintf(stderr, "timer setup failed\n");
            return 1;
        }
        c.eintr_state = true;
    }

    rusage before {}, after {};
    getrusage(RUSAGE_SELF, &before);
    RunResult r = run_arm(c);
    getrusage(RUSAGE_SELF, &after);
    if (state == State::eintr)
        stop_periodic_timer();
    if (!r.ok) {
        std::fprintf(stderr, "VERIFY_FAIL arm=%s\n", arm_name(arm));
        return 1;
    }
    if (write_side && !c.closed_state) {
        const RunResult content = verify_write_scratch(c);
        r.content_hash = content.content_hash;
        r.content_ok = content.content_ok;
        if (content.content_ok == 0) {
            std::fprintf(stderr, "VERIFY_FAIL arm=%s content\n", arm_name(arm));
            return 1;
        }
    }
    std::uint64_t src_hash = 0;
    std::uint64_t dst_size = 0;
    if (family == Family::copy) {
        const RunResult content = verify_copy(c, src_hash);
        r.content_hash = content.content_hash;
        r.content_ok = content.content_ok;
        struct ::stat ds {};
        if (::fstat(c.dst_fd, &ds) == 0)
            dst_size = static_cast<std::uint64_t>(ds.st_size);
        if (content.content_ok != 1) {
            std::fprintf(stderr, "VERIFY_FAIL arm=%s content\n", arm_name(arm));
            return 1;
        }
    }

    std::uint64_t extra_ops = c.warmup;
    if (family == Family::read)
        extra_ops += std::min(c.ops, c.file_blocks);
    if (family == Family::vectored_read)
        extra_ops += std::min(c.ops, c.group_blocks);
    if (family == Family::planner)
        extra_ops += 2 * c.ops;
    const double total_ops =
        static_cast<double>(c.ops) * static_cast<double>(c.reps) + static_cast<double>(extra_ops);
    const double user_per_op =
        ((after.ru_utime.tv_sec - before.ru_utime.tv_sec) +
         (after.ru_utime.tv_usec - before.ru_utime.tv_usec) * 1e-6) * 1e9 / total_ops;
    const double sys_per_op =
        ((after.ru_stime.tv_sec - before.ru_stime.tv_sec) +
         (after.ru_stime.tv_usec - before.ru_stime.tv_usec) * 1e-6) * 1e9 / total_ops;
    std::printf("arm=%s size=%zu ops=%zu ns_per_op=%.3f user_ns_per_op=%.3f sys_ns_per_op=%.3f "
                "checksum=%llu content_hash=0x%016llx content_ok=%d",
                arm_name(arm), c.block, c.ops, r.ns_per_op, user_per_op, sys_per_op,
                static_cast<unsigned long long>(r.checksum),
                static_cast<unsigned long long>(r.content_hash), r.content_ok);
    if (family == Family::vectored_read || family == Family::vectored_write) {
        const std::uint64_t groups = c.ops * c.reps;
        std::printf(" iovcnt=%zu fill_len_hash=0x%016llx syscalls_per_group=%llu "
                    "issued_per_group=%llu",
                    c.opt.iovcnt, static_cast<unsigned long long>(c.fill_len_hash),
                    static_cast<unsigned long long>(groups ? c.n_sys / groups : 0),
                    static_cast<unsigned long long>(groups ? c.n_issued / groups : 0));
    }
    if (family == Family::planner) {
        const std::uint64_t groups = c.ops * c.reps;
        std::printf(" trace=%s merge=%zu dedup=%d req_digest=0x%016llx ref_digest=0x%016llx "
                    "outcome_digest=0x%016llx syscalls_per_group=%llu issued_per_group=%llu "
                    "requested_per_group=%llu",
                    c.opt.trace.c_str(), planner_effective_threshold(c),
                    c.arm == Arm::RPLD || c.opt.dedup ? 1 : 0,
                    static_cast<unsigned long long>(c.req_digest),
                    static_cast<unsigned long long>(c.ref_digest),
                    static_cast<unsigned long long>(c.outcome_digest),
                    static_cast<unsigned long long>(groups ? c.n_sys / groups : 0),
                    static_cast<unsigned long long>(groups ? c.n_issued / groups : 0),
                    static_cast<unsigned long long>(kPlannerRequests *
                                                    static_cast<std::uint64_t>(c.block)));
    }
    if (family == Family::copy) {
        const std::uint64_t chunks = c.ops * c.reps;
        std::printf(" syscalls_per_chunk=%llu issued_per_chunk=%llu src_hash=0x%016llx "
                    "dst_size=%llu",
                    static_cast<unsigned long long>(chunks ? c.n_sys / chunks : 0),
                    static_cast<unsigned long long>(chunks ? c.n_issued / chunks : 0),
                    static_cast<unsigned long long>(src_hash),
                    static_cast<unsigned long long>(dst_size));
    }
    std::printf("\n");
    if (!c.closed_state) {
        (void)c.file->close();
        if (c.dst_file)
            (void)c.dst_file->close();
    }
    return 0;
}
