// g1_control_copy_x0_bench — COPY-X0 research-only falsification harness
// (G1-Control Candidate 2, research/g1-control-copy-x0/).
//
// RESEARCH ONLY. This binary exists to falsify or support T-COPY-X0 under the
// FROZEN preregistration (COPY-X0-PREREGISTRATION.md). It does not modify
// production semantics and must not grow into a framework (prereg M6).
//
// Arms (prereg §4):
//   B0 raw pread/pwrite buffered loop   (no Sluice code)
//   B1 production sluice::copy_all      (sluice_core, untouched)
//   B2 raw copy_file_range loop         (no Sluice code)
//   B3 thin research-only Copy boundary (this file, prereg §3/§4)
//
// The buffered and cfr loops are function templates over a progress source so
// the SELFTEST mode can drive them with deterministic fake partial/zero
// progress (prereg §5 rows 7/8; the kernel cannot be forced to return 0
// mid-file deterministically). This seam is research-bench-internal; no
// production seam exists or is implied.
//
// Every emitted JSONL row carries the fail-closed witnesses the validator
// re-derives: mechanism_executed, op counts, byte checksums, offsets,
// fallback state (prereg §11).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64

#include <sluice/copy.hpp>
#include <sluice/error.hpp>
#include <sluice/file.hpp>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr std::uint64_t kMiB = 1024 * 1024;
constexpr std::uint64_t kFnvBasis = 0xCBF29CE484222325ULL;

[[noreturn]] void die(const char* what, int err = errno) {
    std::fprintf(stderr, "copy-x0-bench: %s: %s\n", what, std::strerror(err));
    std::exit(2);
}

bool errno_is_unsupported(int e) {
    // Under the frozen usage (flags=0, distinct regular files, no aliasing),
    // these errnos mean the environment refuses the mechanism (rows 11/12).
    return e == EOPNOTSUPP || e == ENOSYS || e == EXDEV || e == EINVAL;
}

// ---------------------------------------------------------------------------
// Deterministic pattern + checksums
// ---------------------------------------------------------------------------

std::uint64_t splitmix64(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void word_fill(std::byte* buf, std::size_t n, std::uint64_t& state) {
    for (std::size_t off = 0; off < n; off += 8) {
        std::uint64_t w = splitmix64(state);
        for (int b = 0; b < 8 && off + static_cast<std::size_t>(b) < n; b++) {
            buf[off + static_cast<std::size_t>(b)] =
                static_cast<std::byte>((w >> (8 * b)) & 0xFF);
        }
    }
}

// Pattern byte at absolute index i of the stream generated from seed.
std::uint64_t fnv_append(const std::byte* buf, std::size_t n, std::uint64_t h) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint8_t>(buf[i]);
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

std::string hex64(std::uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
    return b;
}

// Checksum of the pattern stream [byte_index, byte_index+len) generated from
// seed (matches mk_pattern's sequential generation).
std::uint64_t pattern_checksum_from(std::uint64_t seed, std::uint64_t byte_index,
                                    std::uint64_t len) {
    std::uint64_t state = seed;
    std::uint64_t skip_words = byte_index / 8;
    for (std::uint64_t i = 0; i < skip_words; ++i) (void)splitmix64(state);
    std::uint64_t h = kFnvBasis;
    std::vector<std::byte> buf(len > 1 * kMiB ? 1 * kMiB : (len ? len : 1));
    std::uint64_t done = 0;
    while (done < len) {
        std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(buf.size(), len - done));
        word_fill(buf.data(), want, state);
        h = fnv_append(buf.data(), want, h);
        done += want;
    }
    return h;
}

std::uint64_t checksum_range(int fd, std::uint64_t off, std::uint64_t len) {
    std::vector<std::byte> buf(1 * kMiB);
    std::uint64_t h = kFnvBasis;
    std::uint64_t pos = off;
    while (pos < off + len) {
        std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(buf.size(), off + len - pos));
        ssize_t r;
        do {
            r = ::pread(fd, buf.data(), want, static_cast<off_t>(pos));
        } while (r < 0 && errno == EINTR);
        if (r < 0) die("pread(checksum)");
        if (r == 0) die("pread(checksum): unexpected EOF");
        h = fnv_append(buf.data(), static_cast<std::size_t>(r), h);
        pos += static_cast<std::uint64_t>(r);
    }
    return h;
}

// ---------------------------------------------------------------------------
// Arm result + loop cores (templates for the selftest seam)
// ---------------------------------------------------------------------------

struct OpCounts {
    std::uint64_t xfer_calls = 0;     // transfer-phase calls issued
    std::uint64_t xfer_bytes = 0;     // bytes confirmed moved by transfer phase
    std::uint64_t partial_events = 0; // calls that returned less than asked
    std::uint64_t sync_calls = 0;     // must stay 0 (prereg §5 row 14)
};

struct ArmResult {
    bool ok = false;
    std::uint64_t bytes_moved = 0;
    int err = 0;                // errno (B0/B2/B3) or IoError::Code (B1)
    int os_errno = 0;           // preserved OS errno where available
    const char* err_class = ""; // source_error|dest_error|transfer_error|
                                // zero_progress|unsupported|precondition|internal
    OpCounts counts;
};

// Buffered loop core: rd/wr return bytes moved (>=0) or -errno.
// EOF (read progress 0 with remaining>0) is a CLEAN stop (row 6).
// Zero progress on a non-empty write is a deterministic error (row 8).
template <class ReadFn, class WriteFn>
ArmResult buffered_loop(ReadFn rd, WriteFn wr, std::uint64_t n, std::size_t chunk,
                        OpCounts& c) {
    ArmResult r;
    std::vector<std::byte> buf(chunk ? chunk : 1);
    while (r.bytes_moved < n) {
        std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(chunk, n - r.bytes_moved));
        ++c.xfer_calls;
        ssize_t got = rd(buf.data(), want);
        if (got < 0) {
            r.err = static_cast<int>(-got);
            r.err_class = "source_error";
            return r;
        }
        if (static_cast<std::uint64_t>(got) > want) {
            r.err_class = "internal";
            return r;
        }
        if (got == 0) {
            r.ok = true; // EOF before limit: clean success
            return r;
        }
        if (static_cast<std::size_t>(got) < want) ++c.partial_events;
        std::size_t written = 0;
        while (written < static_cast<std::size_t>(got)) {
            ssize_t w = wr(buf.data() + written, static_cast<std::size_t>(got) - written);
            if (w < 0) {
                r.err = static_cast<int>(-w);
                r.err_class = "dest_error";
                return r;
            }
            if (w == 0) {
                r.err = EIO;
                r.err_class = "zero_progress";
                return r;
            }
            if (static_cast<std::size_t>(w) < static_cast<std::size_t>(got) - written) {
                ++c.partial_events;
            }
            written += static_cast<std::size_t>(w);
            c.xfer_bytes += static_cast<std::uint64_t>(w);
        }
        r.bytes_moved += static_cast<std::uint64_t>(got);
    }
    r.ok = true;
    return r;
}

// copy_file_range loop core. available = bytes that can exist in the source
// for THIS copy (S - src_off, clamped). A 0 return is EOF only when the moved
// total has reached `available`; otherwise 0-without-error is the frozen
// zero-progress error (row 8).
template <class CfrFn>
ArmResult cfr_loop(CfrFn cfr, std::uint64_t n, std::size_t chunk,
                   std::uint64_t available, OpCounts& c) {
    ArmResult r;
    while (r.bytes_moved < n) {
        std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(chunk, n - r.bytes_moved));
        ++c.xfer_calls;
        ssize_t got = cfr(want);
        if (got < 0) {
            r.err = static_cast<int>(-got);
            r.err_class = errno_is_unsupported(r.err) ? "unsupported" : "transfer_error";
            return r;
        }
        if (got == 0) {
            if (r.bytes_moved >= available) {
                r.ok = true; // EOF exactly at source end
                return r;
            }
            r.err = EIO;
            r.err_class = "zero_progress";
            return r;
        }
        if (static_cast<std::uint64_t>(got) > want) {
            r.err_class = "internal";
            return r;
        }
        if (static_cast<std::size_t>(got) < want) ++c.partial_events;
        r.bytes_moved += static_cast<std::uint64_t>(got);
        c.xfer_bytes += static_cast<std::uint64_t>(got);
    }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// B0 — raw POSIX buffered copy (positional)
// ---------------------------------------------------------------------------

ArmResult run_b0(int src_fd, std::uint64_t src_off, int dst_fd, std::uint64_t dst_off,
                 std::uint64_t n, std::size_t chunk, OpCounts& c) {
    std::uint64_t src_pos = src_off;
    std::uint64_t dst_pos = dst_off;
    return buffered_loop(
        [&](std::byte* buf, std::size_t want) -> ssize_t {
            ssize_t r;
            do {
                r = ::pread(src_fd, buf, want, static_cast<off_t>(src_pos));
            } while (r < 0 && errno == EINTR);
            if (r > 0) src_pos += static_cast<std::uint64_t>(r);
            return r < 0 ? -errno : r;
        },
        [&](const std::byte* buf, std::size_t len) -> ssize_t {
            ssize_t w;
            do {
                w = ::pwrite(dst_fd, buf, len, static_cast<off_t>(dst_pos));
            } while (w < 0 && errno == EINTR);
            if (w > 0) dst_pos += static_cast<std::uint64_t>(w);
            return w < 0 ? -errno : w;
        },
        n, chunk, c);
}

// ---------------------------------------------------------------------------
// B2 — raw copy_file_range loop (positional, non-NULL offsets)
// ---------------------------------------------------------------------------

ArmResult run_b2(int src_fd, std::uint64_t src_off, int dst_fd, std::uint64_t dst_off,
                 std::uint64_t n, std::size_t chunk, std::uint64_t src_size,
                 OpCounts& c) {
    std::uint64_t src_pos = src_off;
    std::uint64_t dst_pos = dst_off;
    std::uint64_t available =
        src_off < src_size ? src_size - src_off : 0; // bytes obtainable now
    return cfr_loop(
        [&](std::size_t want) -> ssize_t {
            loff_t in = static_cast<loff_t>(src_pos);
            loff_t out = static_cast<loff_t>(dst_pos);
            ssize_t r;
            do {
                r = ::copy_file_range(src_fd, &in, dst_fd, &out, want, 0);
            } while (r < 0 && errno == EINTR);
            if (r > 0) {
                src_pos = static_cast<std::uint64_t>(in);
                dst_pos = static_cast<std::uint64_t>(out);
            }
            return r < 0 ? -errno : r;
        },
        n, chunk, available, c);
}

// ---------------------------------------------------------------------------
// B3 — thin research-only Copy boundary (prereg §3/§4; must stay thin, M6)
// ---------------------------------------------------------------------------

enum class Mechanism { Buffered, FileRange };
enum class UnsupportedPolicy { Fail, FallbackToBuffered };

struct CopyDecisionX0 {
    Mechanism requested = Mechanism::Buffered;
    Mechanism selected = Mechanism::Buffered;
    const char* reason = "";
    const char* mechanism_executed = ""; // buffered_read_write|copy_file_range|none
    bool fallback_occurred = false;
};

std::string decision_json(const CopyDecisionX0& dec) {
    std::string s = "{\"requested\":\"";
    s += dec.requested == Mechanism::Buffered ? "buffered" : "file_range";
    s += "\",\"selected\":\"";
    s += dec.selected == Mechanism::Buffered ? "buffered" : "file_range";
    s += "\",\"reason\":\"";
    s += dec.reason;
    s += "\",\"mechanism_executed\":\"";
    s += dec.mechanism_executed;
    s += "\",\"fallback_occurred\":";
    s += dec.fallback_occurred ? "true" : "false";
    s += "}";
    return s;
}
// The ONLY composed surface under study. The regular-file precondition is a
// declared-contract check (prereg §3), not a TOCTOU precheck of the copy op:
// the kernel result stays authoritative for every executed byte.
ArmResult run_b3(int src_fd, std::uint64_t src_off, int dst_fd, std::uint64_t dst_off,
                 std::uint64_t n, std::size_t chunk, Mechanism mech,
                 UnsupportedPolicy policy, CopyDecisionX0& dec, OpCounts& c) {
    ArmResult r;
    dec.requested = mech;
    dec.selected = mech;
    struct stat sst {}, dst {};
    bool src_regular = ::fstat(src_fd, &sst) == 0 && S_ISREG(sst.st_mode);
    bool dst_regular = ::fstat(dst_fd, &dst) == 0 && S_ISREG(dst.st_mode);
    if (!src_regular || !dst_regular) {
        dec.reason = "precondition_regular_file";
        dec.mechanism_executed = "none";
        r.err = EINVAL;
        r.err_class = "precondition";
        return r;
    }
    std::uint64_t src_size = static_cast<std::uint64_t>(sst.st_size);

    if (mech == Mechanism::Buffered) {
        dec.reason = "buffered_selected";
        dec.mechanism_executed = "buffered_read_write";
        return run_b0(src_fd, src_off, dst_fd, dst_off, n, chunk, c);
    }
    r = run_b2(src_fd, src_off, dst_fd, dst_off, n, chunk, src_size, c);
    if (r.ok || r.err_class != std::string_view("unsupported")) {
        dec.reason = r.ok ? "file_range_selected" : "file_range_error";
        dec.mechanism_executed = "copy_file_range";
        return r;
    }
    if (policy == UnsupportedPolicy::Fail) {
        dec.reason = "file_range_unsupported_fail_closed";
        dec.mechanism_executed = "none";
        return r; // error propagates; nothing silently retried
    }
    // Explicitly requested fallback: runs ONLY recorded (prereg §12). The
    // refused attempt stays visible in the accumulated counts.
    dec.selected = Mechanism::Buffered;
    dec.reason = "file_range_fallback_to_buffered";
    dec.fallback_occurred = true;
    dec.mechanism_executed = "buffered_read_write";
    return run_b0(src_fd, src_off, dst_fd, dst_off, n, chunk, c);
}

// ---------------------------------------------------------------------------
// B1 — production sluice::copy_all over adopted-fd FileReader/FileWriter
// ---------------------------------------------------------------------------

struct B1Result {
    ArmResult arm;
    std::uint64_t copy_bytes_read = 0;
    std::uint64_t copy_bytes_written = 0;
    std::uint64_t scratch_calls = 0;
    std::uint64_t fast_path_calls = 0;
};

B1Result run_b1(int src_fd, std::uint64_t src_off, int dst_fd, std::uint64_t dst_off,
                std::uint64_t n, std::size_t chunk) {
    B1Result out;
    // copy_all is non-positional (audit F-1): it advances the shared file
    // offsets from wherever the fds are positioned. Pre-position via lseek
    // (pipes cannot seek: with src_off==0 a pipe source proceeds unseeked —
    // the one fixture where this matters is S5, fixture S5 src_off is 0).
    if (::lseek(src_fd, static_cast<off_t>(src_off), SEEK_SET) < 0 &&
        !(src_off == 0 && errno == ESPIPE)) {
        out.arm.err = errno;
        out.arm.err_class = "internal";
        return out;
    }
    if (::lseek(dst_fd, static_cast<off_t>(dst_off), SEEK_SET) < 0) {
        out.arm.err = errno;
        out.arm.err_class = "internal";
        return out;
    }
    // FileReader/FileWriter adopt fds and close them on destruction; dup so
    // the harness keeps its own descriptors for post-verification (the dup
    // shares the file description, so offset advances are observable).
    int sfd = ::dup(src_fd);
    int dfd = ::dup(dst_fd);
    if (sfd < 0 || dfd < 0) die("dup");
    {
        sluice::FileReader reader{sfd};
        sluice::FileWriter writer{dfd};
        std::vector<std::byte> scratch(chunk ? chunk : 1);
        sluice::CopyOptions opts;
        opts.limit = sluice::CopyLimit::bytes(n);
        opts.strategy = sluice::CopyStrategy::Auto; // current library default
        sluice::CopyStats stats{};
        sluice::CopyDecision dec{};
        auto res =
            sluice::copy_all(reader, writer, std::span<std::byte>(scratch), opts, &stats, &dec);
        out.copy_bytes_read = stats.bytes_read;
        out.copy_bytes_written = stats.bytes_written;
        out.scratch_calls = stats.scratch_path_calls;
        out.fast_path_calls = stats.buffered_fast_path_calls;
        out.arm.counts.xfer_calls = stats.scratch_path_calls;
        out.arm.counts.xfer_bytes = stats.bytes_written;
        if (res.has_value()) {
            out.arm.ok = true;
            out.arm.bytes_moved = res.value();
        } else {
            out.arm.err = static_cast<int>(res.error().code);
            out.arm.os_errno = res.error().os_errno;
            out.arm.err_class = "transfer_error";
            // Production copy_all discards partial progress in its error
            // result (audit F-3); CopyStats.bytes_written is the honest
            // witness of what actually landed.
            out.arm.bytes_moved = stats.bytes_written;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// JSON row emission
// ---------------------------------------------------------------------------

void emit(const std::string& row) {
    std::fputs(row.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

struct Row {
    std::string s;
    Row& operator()(const std::string& kv) {
        s += s.empty() ? "{" : ",";
        s += kv;
        return *this;
    }
    Row& num(const char* k, std::uint64_t v) {
        char b[128];
        std::snprintf(b, sizeof(b), "\"%s\":%" PRIu64, k, v);
        return (*this)(b);
    }
    Row& real(const char* k, double v) {
        char b[128];
        std::snprintf(b, sizeof(b), "\"%s\":%.9f", k, v);
        return (*this)(b);
    }
    Row& str(const char* k, const char* v) {
        char b[256];
        std::snprintf(b, sizeof(b), "\"%s\":\"%s\"", k, v);
        return (*this)(b);
    }
    Row& boolean(const char* k, bool v) {
        char b[64];
        std::snprintf(b, sizeof(b), "\"%s\":%s", k, v ? "true" : "false");
        return (*this)(b);
    }
    // Splice an already-serialized JSON object/array as the value of key k.
    Row& raw(const char* k, const std::string& json_value) {
        s += s.empty() ? "{" : ",";
        s += "\"";
        s += k;
        s += "\":";
        s += json_value;
        return *this;
    }
    void done() {
        s += "}";
        emit(s);
    }
};

void row_add_result(Row& r, const ArmResult& a) {
    r.boolean("ok", a.ok);
    r.num("bytes_moved", a.bytes_moved);
    r.num("err", static_cast<std::uint64_t>(a.err < 0 ? 0 : a.err));
    r.num("os_errno", static_cast<std::uint64_t>(a.os_errno < 0 ? 0 : a.os_errno));
    if (a.err_class && a.err_class[0]) r.str("err_class", a.err_class);
    r.num("xfer_calls", a.counts.xfer_calls);
    r.num("xfer_bytes", a.counts.xfer_bytes);
    r.num("partial_events", a.counts.partial_events);
    r.num("sync_calls", a.counts.sync_calls);
}

// ---------------------------------------------------------------------------
// File preparation helpers
// ---------------------------------------------------------------------------

std::string work_path(const std::string& root, const std::string& name) {
    return root + (root.back() == '/' ? "" : "/") + name;
}

void ensure_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) die("mkdir");
}

void write_all_raw(int fd, const std::byte* buf, std::size_t n) {
    std::size_t done = 0;
    while (done < n) {
        ssize_t w = ::write(fd, buf + done, n - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            die("write(raw)");
        }
        done += static_cast<std::size_t>(w);
    }
}

void pwrite_all_raw(int fd, const std::byte* buf, std::size_t n, std::uint64_t off) {
    std::size_t done = 0;
    while (done < n) {
        ssize_t w = ::pwrite(fd, buf + done, n - done, static_cast<off_t>(off + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            die("pwrite(raw)");
        }
        done += static_cast<std::size_t>(w);
    }
}

void mk_pattern(const std::string& path, std::uint64_t size, std::uint64_t seed) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open(mk_pattern)");
    std::vector<std::byte> buf(1 * kMiB);
    std::uint64_t state = seed;
    std::uint64_t pos = 0;
    while (pos < size) {
        std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), size - pos));
        word_fill(buf.data(), want, state);
        write_all_raw(fd, buf.data(), want);
        pos += want;
    }
    if (::close(fd) != 0) die("close(mk_pattern)");
}

constexpr std::uint64_t kSentinelSeed = 0xA5A5A5A5A5A5A5A5ULL;

// Sparse file: hole [0, data_off), data [data_off, data_off+data_len), hole to total.
void mk_sparse(const std::string& path, std::uint64_t total, std::uint64_t data_off,
               std::uint64_t data_len, std::uint64_t seed) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open(mk_sparse)");
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) die("ftruncate");
    std::vector<std::byte> buf(1 * kMiB);
    std::uint64_t state = seed;
    std::uint64_t filled = 0;
    while (filled < data_len) {
        std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), data_len - filled));
        word_fill(buf.data(), want, state);
        pwrite_all_raw(fd, buf.data(), want, data_off + filled);
        filled += want;
    }
    if (::close(fd) != 0) die("close(mk_sparse)");
}

std::string extents_json(int fd, std::uint64_t size) {
    std::string s = "[";
    std::uint64_t pos = 0;
    int n = 0;
    while (pos < size && n < 16) {
        off_t d = ::lseek(fd, static_cast<off_t>(pos), SEEK_DATA);
        if (d < 0 || static_cast<std::uint64_t>(d) >= size) break;
        off_t h = ::lseek(fd, d, SEEK_HOLE);
        if (h < 0) h = static_cast<off_t>(size);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s[%" PRIu64 ",%" PRIu64 "]", n ? "," : "",
                      static_cast<std::uint64_t>(d), static_cast<std::uint64_t>(h));
        s += buf;
        pos = static_cast<std::uint64_t>(h);
        ++n;
    }
    s += "]";
    return s;
}

std::uint64_t file_size(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) die("stat");
    return static_cast<std::uint64_t>(st.st_size);
}

std::uint64_t cur_offset(int fd) {
    off_t o = ::lseek(fd, 0, SEEK_CUR);
    return o < 0 ? ~0ULL : static_cast<std::uint64_t>(o);
}

std::string fstype_of(const std::string& path) {
    struct statfs st {};
    if (::statfs(path.c_str(), &st) != 0) die("statfs");
    std::uint64_t magic = static_cast<std::uint64_t>(st.f_type);
    const char* name = "unknown";
    if (magic == 0x01021994) name = "tmpfs";
    else if (magic == 0xEF53) name = "ext4";
    char out[64];
    std::snprintf(out, sizeof(out), "%s:0x%llx", name, static_cast<unsigned long long>(magic));
    return out;
}

// ---------------------------------------------------------------------------
// Fixture engine
// ---------------------------------------------------------------------------

struct FixtureCtx {
    std::string root;        // where the destination is created
    std::string substrate;   // label recorded into the row
    std::string fixture;
    std::uint64_t seed = 0;
    std::string src_path;    // "(pipe)" when pipe
    std::uint64_t src_off = 0;
    std::uint64_t dst_off = 0;
    std::uint64_t n = 0;
    std::uint64_t src_size = 0;
    std::uint64_t dst_pre_size = 0;
    bool is_pipe_src = false;
    int pipe_fd = -1;
    std::uint64_t pipe_ck = 0; // expected checksum of pipe payload
};

const char* arm_name(int i) {
    static const char* names[] = {"B0", "B1", "B2", "B3"};
    return names[i];
}

void run_fixture_arm(int arm_idx, FixtureCtx& cx, std::size_t chunk) {
    const char* arm = arm_name(arm_idx);
    std::string dst =
        work_path(cx.root, "dst-" + std::string(cx.fixture) + "-" + arm + ".bin");
    if (cx.dst_pre_size > 0) {
        mk_pattern(dst, cx.dst_pre_size, kSentinelSeed);
    } else {
        int fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) ::close(fd);
        else die("open(fixture dst create)");
    }

    int src_fd = -1;
    std::uint64_t src_range_ck = 0;
    std::uint64_t src_full_ck_before = 0;
    if (cx.is_pipe_src) {
        src_fd = cx.pipe_fd;
        src_range_ck = cx.pipe_ck;
    } else {
        src_fd = ::open(cx.src_path.c_str(), O_RDONLY);
        if (src_fd < 0) die("open(fixture src)");
        // Source checksum covers the copyable span: EOF fixtures (S3) hold
        // fewer bytes than requested; success moves exactly this many.
        std::uint64_t ck_len = cx.src_off < cx.src_size
                                   ? std::min(cx.n, cx.src_size - cx.src_off)
                                   : 0;
        src_range_ck = checksum_range(src_fd, cx.src_off, ck_len);
        src_full_ck_before = checksum_range(src_fd, 0, cx.src_size);
    }
    int dst_fd = ::open(dst.c_str(), O_WRONLY);
    if (dst_fd < 0) die("open(fixture dst)");

    std::uint64_t src_off_before = cur_offset(src_fd);
    std::uint64_t dst_off_before = cur_offset(dst_fd);

    ArmResult r;
    CopyDecisionX0 dec;
    if (arm_idx == 0) {
        r = run_b0(src_fd, cx.src_off, dst_fd, cx.dst_off, cx.n, chunk, r.counts);
    } else if (arm_idx == 1) {
        B1Result b1 = run_b1(src_fd, cx.src_off, dst_fd, cx.dst_off, cx.n, chunk);
        r = b1.arm;
    } else if (arm_idx == 2) {
        r = run_b2(src_fd, cx.src_off, dst_fd, cx.dst_off, cx.n, chunk, cx.src_size,
                   r.counts);
    } else {
        r = run_b3(src_fd, cx.src_off, dst_fd, cx.dst_off, cx.n, chunk,
                   Mechanism::FileRange, UnsupportedPolicy::Fail, dec, r.counts);
    }

    std::uint64_t src_off_after = cur_offset(src_fd);
    std::uint64_t dst_off_after = cur_offset(dst_fd);
    std::uint64_t src_full_ck_after =
        cx.is_pipe_src ? src_full_ck_before : checksum_range(src_fd, 0, cx.src_size);

    Row row;
    row.str("id", ("sem|" + cx.fixture + "|" + cx.substrate + "|" + arm).c_str());
    row.str("phase", "semantic");
    row.str("fixture", cx.fixture.c_str());
    row.str("arm", arm);
    row.str("label", cx.substrate.c_str());
    row.num("chunk", chunk);
    row.num("n", cx.n);
    row.num("src_off", cx.src_off);
    row.num("dst_off", cx.dst_off);
    row_add_result(row, r);

    if (r.ok) {
        // dst_fd is write-only; verification reads through a separate fd.
        int vfd = ::open(dst.c_str(), O_RDONLY);
        if (vfd < 0) die("open(fixture dst verify)");
        std::uint64_t dst_ck = checksum_range(vfd, cx.dst_off, r.bytes_moved);
        std::uint64_t dsize = file_size(dst);
        std::uint64_t expect_size =
            std::max(cx.dst_pre_size, cx.dst_off + r.bytes_moved);
        row.str("dst_ck", hex64(dst_ck).c_str());
        row.boolean("dest_bytes_ok", dst_ck == src_range_ck);
        row.boolean("dest_size_ok", dsize == expect_size);
        bool outside_ok = true;
        if (cx.dst_pre_size > cx.dst_off + r.bytes_moved) {
            std::uint64_t tail_len =
                cx.dst_pre_size - (cx.dst_off + r.bytes_moved);
            std::uint64_t tail_ck = checksum_range(
                vfd, cx.dst_off + r.bytes_moved, tail_len);
            std::uint64_t tail_ref = pattern_checksum_from(
                kSentinelSeed, cx.dst_off + r.bytes_moved, tail_len);
            outside_ok = tail_ck == tail_ref;
        }
        row.boolean("outside_range_ok", outside_ok);
        ::close(vfd);
    } else {
        row.str("verify", "skipped_error");
    }
    row.boolean("src_unchanged_ok", src_full_ck_after == src_full_ck_before);
    row.num("src_off_before", src_off_before);
    row.num("src_off_after", src_off_after);
    row.num("dst_off_before", dst_off_before);
    row.num("dst_off_after", dst_off_after);
    row.raw("decision", decision_json(dec));
    row.boolean("pipe", cx.is_pipe_src);
    row.done();

    if (!cx.is_pipe_src) ::close(src_fd);
    ::close(dst_fd);
}

// ---------------------------------------------------------------------------
// Fixtures (prereg §7)
// ---------------------------------------------------------------------------

std::size_t g_chunk = 256 * 1024;

void fixture_S1(const std::string& root, const std::string& sub, std::uint64_t seed) {
    std::string src = work_path(root, "s1-src.bin");
    mk_pattern(src, 1 * kMiB, seed + 1);
    FixtureCtx cx{root, sub, "S1", seed, src, 0, 0, 1 * kMiB, 1 * kMiB, 0};
    for (int a = 0; a < 4; ++a) run_fixture_arm(a, cx, g_chunk);
}

void fixture_S2(const std::string& root, const std::string& sub, std::uint64_t seed) {
    std::string src = work_path(root, "s2-src.bin");
    mk_pattern(src, 1 * kMiB, seed + 2);
    FixtureCtx cx{root, sub, "S2", seed, src, 4096, 8192, 64 * 1024, 1 * kMiB, 1 * kMiB};
    for (int a = 0; a < 4; ++a) run_fixture_arm(a, cx, g_chunk);
}

void fixture_S3(const std::string& root, const std::string& sub, std::uint64_t seed) {
    std::string src = work_path(root, "s3-src.bin");
    mk_pattern(src, 16 * 1024, seed + 3);
    FixtureCtx cx{root, sub, "S3", seed, src, 0, 0, 64 * 1024, 16 * 1024, 0};
    for (int a = 0; a < 4; ++a) run_fixture_arm(a, cx, g_chunk);
}

void fixture_S4(const std::string& root, const std::string& sub, std::uint64_t seed) {
    std::string src = work_path(root, "s4-src.bin");
    mk_pattern(src, 64 * kMiB, seed + 4);
    FixtureCtx cx{root, sub, "S4", seed, src, 0, 0, 64 * kMiB, 64 * kMiB, 0};
    for (int a = 0; a < 4; ++a) run_fixture_arm(a, cx, g_chunk);
}

void fixture_S5(const std::string& root, const std::string& sub, std::uint64_t seed) {
    // Pipe source: violates CopyRange's declared regular-file precondition.
    // Expected (row 11): B0 pread fails ESPIPE (source_error); B1 (abstract
    // Reader/Writer surface, no such precondition declared) succeeds; B2
    // fails with kernel EINVAL (unsupported); B3 fails closed naming the
    // precondition. Each arm gets its own pipe with identical payload.
    std::vector<std::byte> pat(4 * 1024);
    {
        std::uint64_t st = seed + 5;
        word_fill(pat.data(), pat.size(), st);
    }
    std::uint64_t ck = fnv_append(pat.data(), pat.size(), kFnvBasis);
    for (int a = 0; a < 4; ++a) {
        int q[2];
        if (::pipe(q) != 0) die("pipe");
        write_all_raw(q[1], pat.data(), pat.size());
        ::close(q[1]); // data then EOF for the reader
        FixtureCtx cx{root, sub, "S5", seed, "(pipe)", 0, 0, 4 * 1024, 4 * 1024, 0};
        cx.is_pipe_src = true;
        cx.pipe_fd = q[0];
        cx.pipe_ck = ck;
        run_fixture_arm(a, cx, g_chunk);
        ::close(q[0]);
    }
}

void fixture_S7(const std::string& root, const std::string& sub, std::uint64_t seed) {
    std::string src = work_path(root, "s7-src.bin");
    mk_pattern(src, 1 * kMiB, seed + 7);
    FixtureCtx cx{root, sub, "S7", seed, src, 0, 8192, 64 * 1024, 1 * kMiB, 1 * kMiB};
    for (int a = 0; a < 4; ++a) run_fixture_arm(a, cx, g_chunk);
}

void fixture_S8(const std::string& root, const std::string& sub, std::uint64_t seed) {
    std::string src = work_path(root, "s8-src.bin");
    mk_sparse(src, 8 * kMiB, 2 * kMiB, 1 * kMiB, seed + 8);
    FixtureCtx cx{root, sub, "S8", seed, src, 0, 0, 8 * kMiB, 8 * kMiB, 0};
    for (int a = 0; a < 4; ++a) run_fixture_arm(a, cx, g_chunk);
    // Layout witness (row 13): dest extents per arm — recorded, not adjudicated.
    for (int a = 0; a < 4; ++a) {
        std::string dst =
            work_path(root, "dst-S8-" + std::string(arm_name(a)) + ".bin");
        int fd = ::open(dst.c_str(), O_RDONLY);
        if (fd < 0) die("open(S8 witness)");
        Row row;
        row.str("id", ("sem|S8ext|" + sub + "|" + arm_name(a)).c_str());
        row.str("phase", "semantic");
        row.str("fixture", "S8ext");
        row.str("arm", arm_name(a));
        row.str("label", sub.c_str());
        row.raw("extents", extents_json(fd, 8 * kMiB));
        row.done();
        ::close(fd);
    }
}

// S6 cross-filesystem: driven from main() (needs both roots).

// ---------------------------------------------------------------------------
// Timed bench mode (perf rows)
// ---------------------------------------------------------------------------

struct TimedRun {
    ArmResult r;
    CopyDecisionX0 dec;
    double wall;
    double cpu;
};

TimedRun timed_arm(const char* arm, int src_fd, int dst_fd, std::uint64_t size,
                   std::size_t chunk, std::uint64_t src_size) {
    TimedRun t;
    struct rusage ru0 {}, ru1 {};
    struct timespec t0 {}, t1 {};
    getrusage(RUSAGE_SELF, &ru0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (arm[1] == '0') {
        t.r = run_b0(src_fd, 0, dst_fd, 0, size, chunk, t.r.counts);
        t.dec.mechanism_executed = "buffered_read_write";
    } else if (arm[1] == '1') {
        B1Result b1 = run_b1(src_fd, 0, dst_fd, 0, size, chunk);
        t.r = b1.arm;
        t.dec.mechanism_executed = "buffered_read_write";
    } else if (arm[1] == '2') {
        t.r = run_b2(src_fd, 0, dst_fd, 0, size, chunk, src_size, t.r.counts);
        t.dec.mechanism_executed = "copy_file_range";
    } else if (arm[1] == '3') {
        t.r = run_b3(src_fd, 0, dst_fd, 0, size, chunk, Mechanism::FileRange,
                     UnsupportedPolicy::Fail, t.dec, t.r.counts);
    } else {
        die("arm");
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    getrusage(RUSAGE_SELF, &ru1);
    t.wall = static_cast<double>(t1.tv_sec - t0.tv_sec) +
             1e-9 * static_cast<double>(t1.tv_nsec - t0.tv_nsec);
    t.cpu = static_cast<double>(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) +
            1e-6 * static_cast<double>(ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) +
            static_cast<double>(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) +
            1e-6 * static_cast<double>(ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec);
    return t;
}

void bench_mode(const char* arm, const std::string& src, const std::string& dst,
                std::uint64_t size, std::size_t chunk, const std::string& id) {
    int sfd = ::open(src.c_str(), O_RDONLY);
    if (sfd < 0) die("open(bench src)");
    std::string src_ck_hex = hex64(checksum_range(sfd, 0, size));
    struct stat sst {};
    if (::fstat(sfd, &sst) != 0) die("fstat");
    std::uint64_t src_size = static_cast<std::uint64_t>(sst.st_size);
    ::close(sfd);

    int src_fd = ::open(src.c_str(), O_RDONLY);
    int dst_fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (src_fd < 0 || dst_fd < 0) die("open(bench)");

    TimedRun t = timed_arm(arm, src_fd, dst_fd, size, chunk, src_size);

    // dst_fd is write-only; verification reads through a separate fd.
    int vfd = ::open(dst.c_str(), O_RDONLY);
    if (vfd < 0) die("open(bench dst verify)");
    std::uint64_t dst_ck = t.r.ok ? checksum_range(vfd, 0, t.r.bytes_moved) : 0;
    ::close(vfd);
    bool bytes_ok =
        t.r.ok && hex64(dst_ck) == src_ck_hex && t.r.bytes_moved == size;
    bool size_ok = t.r.ok ? file_size(dst) == size : true;
    const char* mech = !t.r.ok ? "unsupported_error" : t.dec.mechanism_executed;

    Row row;
    row.str("id", id.c_str());
    row.str("phase", "perf");
    row.str("arm", arm);
    row.num("size", size);
    row.num("chunk", chunk);
    row.real("wall_sec", t.wall);
    row.real("cpu_sec", t.cpu);
    row.str("mechanism_executed", mech);
    row.boolean("bytes_ok", bytes_ok);
    row.boolean("size_ok", size_ok);
    row_add_result(row, t.r);
    row.raw("decision", decision_json(t.dec));
    row.done();

    ::close(src_fd);
    ::close(dst_fd);
}

// ---------------------------------------------------------------------------
// selftest
// ---------------------------------------------------------------------------

int g_selftest_failures = 0;

void selftest_check(const char* name, bool pass) {
    Row row;
    row.str("id", (std::string("selftest|") + name).c_str());
    row.str("phase", "selftest");
    row.boolean("pass", pass);
    row.done();
    if (!pass) ++g_selftest_failures;
}

void run_selftest() {
    { // partial reads/writes accounted; EOF clean (rows 6/7).
        OpCounts c;
        int rdc = 0;
        auto rd = [&](std::byte*, std::size_t) -> ssize_t {
            static const int seq[] = {3, 1, 0};
            return seq[rdc++ % 3];
        };
        int wrc = 0;
        auto wr = [&](const std::byte*, std::size_t len) -> ssize_t {
            ++wrc;
            return static_cast<ssize_t>(std::min<std::size_t>(len, wrc % 2 ? 2 : 1));
        };
        ArmResult r = buffered_loop(rd, wr, 4, 8, c);
        selftest_check("buffered_partial_eof_accounting",
                       r.ok && r.bytes_moved == 4 && c.xfer_bytes == 4);
    }
    { // zero-progress write = deterministic error (row 8).
        OpCounts c;
        auto rd = [](std::byte*, std::size_t) -> ssize_t { return 4; };
        auto wr = [](const std::byte*, std::size_t) -> ssize_t { return 0; };
        ArmResult r = buffered_loop(rd, wr, 8, 8, c);
        selftest_check("buffered_zero_progress_error",
                       !r.ok && std::string_view(r.err_class) == "zero_progress");
    }
    { // cfr partials then clean EOF at source end (rows 6/7).
        OpCounts c;
        int calls = 0;
        auto cfr = [&](std::size_t want) -> ssize_t {
            (void)want;
            ++calls;
            return calls == 1 ? 3 : calls == 2 ? 1 : 0;
        };
        ArmResult r = cfr_loop(cfr, 10, 8, 4, c);
        selftest_check("cfr_partial_eof_accounting",
                       r.ok && r.bytes_moved == 4 && c.xfer_bytes == 4 &&
                           c.partial_events >= 1);
    }
    { // cfr zero progress before source end = error (row 8).
        OpCounts c;
        auto cfr = [](std::size_t) -> ssize_t { return 0; };
        ArmResult r = cfr_loop(cfr, 10, 8, 8, c);
        selftest_check("cfr_zero_progress_error",
                       !r.ok && std::string_view(r.err_class) == "zero_progress");
    }
    { // EOPNOTSUPP classified unsupported (rows 11/12).
        OpCounts c;
        auto cfr = [](std::size_t) -> ssize_t { return -EOPNOTSUPP; };
        ArmResult r = cfr_loop(cfr, 8, 8, 8, c);
        selftest_check("cfr_unsupported_classified",
                       !r.ok && std::string_view(r.err_class) == "unsupported" &&
                           r.err == EOPNOTSUPP);
    }
    { // B3 precondition failure must NOT trigger fallback (prereg §12).
        int p[2];
        if (::pipe(p) != 0) die("pipe");
        int nfd = ::open("/dev/null", O_WRONLY);
        if (nfd < 0) die("open(/dev/null)");
        CopyDecisionX0 dec;
        OpCounts c;
        ArmResult r = run_b3(p[0], 0, nfd, 0, 8, 8, Mechanism::FileRange,
                             UnsupportedPolicy::FallbackToBuffered, dec, c);
        selftest_check("b3_precondition_no_fallback",
                       !r.ok && std::string_view(r.err_class) == "precondition" &&
                           !dec.fallback_occurred &&
                           std::string(dec.reason) == "precondition_regular_file");
        ::close(p[0]);
        ::close(p[1]);
        ::close(nfd);
    }
    if (g_selftest_failures > 0) {
        std::fprintf(stderr, "copy-x0-bench: selftest FAILED (%d)\n", g_selftest_failures);
        std::exit(1);
    }
}

// ---------------------------------------------------------------------------
// probe mode (prereg §10.2)
// ---------------------------------------------------------------------------

std::string label_of_root(const std::string& p) {
    return fstype_of(p).rfind("tmpfs", 0) == 0 ? "tmpfs" : "ext4";
}

void run_probe(const std::string& root_a, const std::string& root_b) {
    std::string dirs[2] = {root_a, root_b};
    for (auto& d : dirs) {
        ensure_dir(work_path(d, "probe"));
        mk_pattern(work_path(d, "probe/src.bin"), 1 * kMiB, 0xC0FFEE);
        mk_pattern(work_path(d, "probe/src64.bin"), 64 * kMiB, 0xC0FFEE64);
    }
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            std::string src = work_path(dirs[i], "probe/src.bin");
            std::string dst = work_path(dirs[j], "probe/dst.bin");
            int sfd = ::open(src.c_str(), O_RDONLY);
            int dfd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (sfd < 0 || dfd < 0) die("open(probe)");
            loff_t in = 0, out = 0;
            errno = 0;
            ssize_t r = ::copy_file_range(sfd, &in, dfd, &out, 4096, 0);
            int e = r < 0 ? errno : 0;
            Row row;
            row.str("id",
                    ("probe|" + label_of_root(dirs[i]) + "-" + label_of_root(dirs[j]))
                        .c_str());
            row.str("phase", "probe");
            row.str("from", label_of_root(dirs[i]).c_str());
            row.str("to", label_of_root(dirs[j]).c_str());
            row.str("fstype_from", fstype_of(dirs[i]).c_str());
            row.str("fstype_to", fstype_of(dirs[j]).c_str());
            row.num("ret", static_cast<std::uint64_t>(r < 0 ? 0 : r));
            row.num("errno", static_cast<std::uint64_t>(e < 0 ? 0 : e));
            row.boolean("ok", r >= 0);
            row.done();
            ::close(sfd);
            ::close(dfd);
        }
    }
    { // single-call ceiling observation on root_a
        std::string src = work_path(root_a, "probe/src64.bin");
        std::string dst = work_path(root_a, "probe/dst64.bin");
        int sfd = ::open(src.c_str(), O_RDONLY);
        int dfd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sfd < 0 || dfd < 0) die("open(probe64)");
        loff_t in = 0, out = 0;
        errno = 0;
        ssize_t r = ::copy_file_range(sfd, &in, dfd, &out,
                                      static_cast<size_t>(4ULL * 1024 * 1024 * 1024), 0);
        Row row;
        row.str("id", "probe|single-call-cap");
        row.str("phase", "probe");
        row.num("ret", static_cast<std::uint64_t>(r < 0 ? 0 : r));
        row.num("errno", static_cast<std::uint64_t>(r < 0 ? errno : 0));
        row.str("fstype", fstype_of(root_a).c_str());
        row.done();
        ::close(sfd);
        ::close(dfd);
    }
}

// ---------------------------------------------------------------------------
// S6 driver (cross-filesystem)
// ---------------------------------------------------------------------------

void s6_mode(const std::string& rt, const std::string& re, std::uint64_t seed) {
    ensure_dir(rt);
    ensure_dir(re);
    for (std::uint64_t size : {std::uint64_t{4} * 1024, std::uint64_t{1} * kMiB}) {
        std::string tsrc = work_path(rt, "s6-src.bin");
        std::string esrc = work_path(re, "s6-src.bin");
        mk_pattern(tsrc, size, seed + 60);
        mk_pattern(esrc, size, seed + 61);
        struct Dir {
            const char* from;
            const char* to;
            std::string src;
        };
        Dir dirs[] = {
            {"tmpfs", "tmpfs", tsrc},
            {"ext4", "ext4", esrc},
            {"tmpfs", "ext4", tsrc},
            {"ext4", "tmpfs", esrc},
        };
        for (auto& d : dirs) {
            std::string dst_root = std::string(d.to) == "tmpfs" ? rt : re;
            for (int a = 0; a < 4; ++a) {
                FixtureCtx cx{dst_root, std::string(d.from) + "-to-" + d.to, "S6",
                              seed,     d.src,
                              0,        0,
                              size,     size,
                              0};
                run_fixture_arm(a, cx, g_chunk);
            }
            bool cross = std::string(d.from) != std::string(d.to);
            if (cross) {
                // Explicit-fallback variant (prereg §12) on the cross pair.
                std::string dst = work_path(dst_root, "dst-S6fb-B3.bin");
                int fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) ::close(fd);
                int sfd = ::open(d.src.c_str(), O_RDONLY);
                int dfd = ::open(dst.c_str(), O_WRONLY);
                if (sfd < 0 || dfd < 0) die("open(S6fb)");
                CopyDecisionX0 dec;
                OpCounts c;
                ArmResult r =
                    run_b3(sfd, 0, dfd, 0, size, g_chunk, Mechanism::FileRange,
                           UnsupportedPolicy::FallbackToBuffered, dec, c);
                std::uint64_t src_ck = checksum_range(sfd, 0, size);
                // dfd is write-only; verification reads through a separate fd.
                int vfd = ::open(dst.c_str(), O_RDONLY);
                if (vfd < 0) die("open(S6fb verify)");
                std::uint64_t dck = r.ok ? checksum_range(vfd, 0, r.bytes_moved) : 0;
                ::close(vfd);
                Row row;
                row.str("id", ("sem|S6fb|" + std::string(d.from) + "-to-" + d.to + "|B3")
                                  .c_str());
                row.str("phase", "semantic");
                row.str("fixture", "S6fb");
                row.str("arm", "B3");
                row.str("label", (std::string(d.from) + "-to-" + d.to).c_str());
                row.num("n", size);
                row_add_result(row, r);
                row.str("dst_ck", hex64(dck).c_str());
                row.boolean("dest_bytes_ok", r.ok && dck == src_ck && r.bytes_moved == size);
                std::string djson = decision_json(dec);
                row.s += ",\"decision\":" + djson;
                row.done();
                ::close(sfd);
                ::close(dfd);
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s selftest | probe <rootA> <rootB> | mk <path> <size> <seed> "
                     "| fixture <S1|S2|S3|S4|S5|S7|S8> <root> <substrate> <seed> [--chunk N] "
                     "| s6 <rootTmp> <rootExt4> <seed> [--chunk N] "
                     "| bench <B0|B1|B2|B3> <src> <dst> <size> <chunk> <id>\n",
                     argv[0]);
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "selftest") {
        run_selftest();
        return 0;
    }
    if (mode == "probe") {
        if (argc < 4) die("probe args");
        run_probe(argv[2], argv[3]);
        return 0;
    }
    if (mode == "mk") {
        if (argc < 5) die("mk args");
        mk_pattern(argv[2], std::strtoull(argv[3], nullptr, 10),
                   std::strtoull(argv[4], nullptr, 10));
        return 0;
    }
    auto parse_chunk = [&](int from) {
        if (from + 1 < argc && std::string(argv[from]) == "--chunk") {
            g_chunk = static_cast<std::size_t>(std::strtoull(argv[from + 1], nullptr, 10));
        }
    };
    if (mode == "fixture") {
        if (argc < 6) die("fixture args");
        std::string name = argv[2];
        std::string root = argv[3];
        std::string sub = argv[4];
        std::uint64_t seed = std::strtoull(argv[5], nullptr, 10);
        parse_chunk(6);
        ensure_dir(root);
        if (name == "S1") fixture_S1(root, sub, seed);
        else if (name == "S2") fixture_S2(root, sub, seed);
        else if (name == "S3") fixture_S3(root, sub, seed);
        else if (name == "S4") fixture_S4(root, sub, seed);
        else if (name == "S5") fixture_S5(root, sub, seed);
        else if (name == "S7") fixture_S7(root, sub, seed);
        else if (name == "S8") fixture_S8(root, sub, seed);
        else die("unknown fixture");
        return 0;
    }
    if (mode == "s6") {
        if (argc < 5) die("s6 args");
        parse_chunk(5);
        s6_mode(argv[2], argv[3], std::strtoull(argv[4], nullptr, 10));
        return 0;
    }
    if (mode == "bench") {
        if (argc < 8) die("bench args");
        bench_mode(argv[2], argv[3], argv[4], std::strtoull(argv[5], nullptr, 10),
                   static_cast<std::size_t>(std::strtoull(argv[6], nullptr, 10)), argv[7]);
        return 0;
    }
    die("unknown mode");
}
