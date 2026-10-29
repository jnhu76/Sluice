// AC-3 closure probe: explicit-offset copy_file_range admission, stop,
// error, type and offset semantics on regular files (R8 checklist items
// 1,2,3,4,5,6,8,9,10,13,14). Every cell prints one observation line; no
// cell mutates anything outside the caller-provided scratch directories.
// Build: g++ -O2 -std=c++20 ac3-sem.cpp -o ac3-sem
// Subcommands: sem <dirA> <dirB> | efbig <dir> | conc <dir> <total-bytes>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace {

constexpr std::size_t kSrcBytes = 8192;
constexpr std::size_t kLen = 4096;
constexpr std::uint64_t kOff = 1024;

std::byte pat(std::uint64_t pos) {
    return static_cast<std::byte>(pos * 1103515245u >> 13 & 0xff);
}

int open_src(const std::string& dir) {
    const std::string path = dir + "/src.bin";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    std::vector<std::byte> buf(kSrcBytes);
    for (std::size_t i = 0; i < kSrcBytes; ++i)
        buf[i] = pat(i);
    if (::pwrite(fd, buf.data(), buf.size(), 0) != static_cast<ssize_t>(kSrcBytes)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int open_dst(const std::string& dir, const char* name, int extra_flags) {
    const std::string path = dir + "/" + name;
    return ::open(path.c_str(),
                  O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | extra_flags, 0644);
}

void cell(const char* name, ssize_t ret, int err, const char* extra = "") {
    std::printf("cell=%s ret=%zd errno=%d(%s)%s%s\n", name, ret, err,
                ret < 0 ? ::strerror(err) : "ok", *extra ? " " : "", extra);
}

template <typename F>
void cellf(const char* name, F&& f, const char* extra = "") {
    errno = 0;
    const ssize_t r = f();
    const int e = errno;
    cell(name, r, e, extra);
}

void kv(const char* name, const char* k, long long v) {
    std::printf("cell=%s %s=%lld\n", name, k, v);
}

bool range_matches(int src, int dst, std::uint64_t dst_off, std::size_t len,
                   std::uint64_t src_off) {
    std::vector<std::byte> a(len), b(len);
    if (::pread(src, a.data(), len, static_cast<off_t>(src_off)) !=
        static_cast<ssize_t>(len))
        return false;
    if (::pread(dst, b.data(), len, static_cast<off_t>(dst_off)) !=
        static_cast<ssize_t>(len))
        return false;
    return std::memcmp(a.data(), b.data(), len) == 0;
}

ssize_t cfr(int s, std::int64_t* so, int d, std::int64_t* dom,
            std::size_t len) {
    off_t a = so ? static_cast<off_t>(*so) : 0;
    off_t b = dom ? static_cast<off_t>(*dom) : 0;
    off_t* pa = so ? &a : nullptr;
    off_t* pb = dom ? &b : nullptr;
    errno = 0;
    const ssize_t r = ::copy_file_range(s, pa, d, pb, len, 0);
    if (so) *so = static_cast<std::int64_t>(a);
    if (dom) *dom = static_cast<std::int64_t>(b);
    return r;
}

off_t pos(int fd) { return ::lseek(fd, 0, SEEK_CUR); }

// --- item 1: two-descriptor admission cells --------------------------------

void admission(const std::string& dir) {
    const int s = open_src(dir);
    const int d = open_dst(dir, "dst-prec.bin", 0);
    const int d_ro = ::open((dir + "/dst-prec-ro.bin").c_str(),
                            O_RDONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (s < 0 || d < 0 || d_ro < 0) {
        cell("admission_open_failed", -1, errno);
        return;
    }
    std::int64_t o = kOff;
    cellf("prec_src_closed_dst_valid",
          [&] { return cfr(-1, nullptr, d, &o, kLen); });
    o = kOff;
    cellf("prec_src_valid_dst_closed",
          [&] { return cfr(s, &o, -1, nullptr, kLen); });
    std::int64_t neg = -1;
    o = kOff;
    cellf("prec_src_closed_and_src_neg_off",
          [&] { return cfr(-1, &neg, d, &o, kLen); });
    neg = -1;
    o = kOff;
    cellf("prec_src_neg_off", [&] { return cfr(s, &neg, d, &o, kLen); });
    neg = -1;
    o = kOff;
    cellf("prec_dst_neg_off", [&] { return cfr(s, &o, d, &neg, kLen); });
    o = kOff;
    cellf("prec_dst_rdonly", [&] { return cfr(s, &o, d_ro, &o, kLen); });
    std::int64_t neg2 = -1;
    cellf("prec_dst_rdonly_and_src_neg_off",
          [&] { return cfr(s, &neg2, d_ro, &o, kLen); });
    cellf("prec_both_closed",
          [&] { return cfr(-1, nullptr, -1, nullptr, kLen); });
    std::int64_t huge = static_cast<std::int64_t>(0x7fffffffffffffffull) - 50;
    std::int64_t o2 = kOff;
    cellf("prec_off_len_overflow",
          [&] { return cfr(s, &huge, d, &o2, 100); });
    const int d_wr = ::open((dir + "/dst-prec-wr.bin").c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (d_wr >= 0) {
        o = kOff;
        cellf("prec_dst_wronly", [&] { return cfr(s, &o, d_wr, &o, kLen); });
        ::close(d_wr);
    }
    ::close(s);
    ::close(d);
    ::close(d_ro);
}

// --- items 2 + 4: EOF stop reason and short copy ----------------------------

void eof_stop(const std::string& dir) {
    const int s = open_src(dir);
    const int d = open_dst(dir, "dst-eof.bin", 0);
    if (s < 0 || d < 0) {
        cell("eof_open_failed", -1, errno);
        return;
    }
    std::int64_t si = 0, di = 0;
    si = static_cast<std::int64_t>(kSrcBytes - kLen);
    di = 0;
    cellf("eof_exact_full", [&] { return cfr(s, &si, d, &di, kLen); });

    si = static_cast<std::int64_t>(kSrcBytes - 2048);
    di = 0;
    ssize_t r = cfr(s, &si, d, &di, kLen);
    int e = errno;
    char extra[128];
    std::snprintf(extra, sizeof extra, "short_by=%zu src_off_after=%lld",
                  kLen - static_cast<std::size_t>(r > 0 ? r : 0),
                  static_cast<long long>(si));
    cell("eof_partial_short", r, e, extra);
    if (r > 0)
        kv("eof_partial_short", "content_match",
           range_matches(s, d, 0, static_cast<std::size_t>(r),
                         kSrcBytes - 2048));

    si = static_cast<std::int64_t>(kSrcBytes);
    di = 0;
    cellf("eof_at_boundary_zero", [&] { return cfr(s, &si, d, &di, kLen); });
    si = static_cast<std::int64_t>(kSrcBytes + 8192);
    di = 0;
    cellf("eof_beyond_zero", [&] { return cfr(s, &si, d, &di, kLen); });
    si = static_cast<std::int64_t>(kSrcBytes - 1);
    di = 4096;
    cellf("eof_last_byte_full", [&] { return cfr(s, &si, d, &di, 1); });

    std::uint64_t total = 0, soff = 0, doff = 0;
    ssize_t last = 1;
    int chunks = 0;
    while (last > 0 && chunks < 64) {
        std::int64_t a = static_cast<std::int64_t>(soff);
        std::int64_t b = static_cast<std::int64_t>(doff);
        errno = 0;
        last = cfr(s, &a, d, &b, kLen);
        if (last < 0)
            break;
        soff += static_cast<std::uint64_t>(last);
        doff += static_cast<std::uint64_t>(last);
        total += static_cast<std::uint64_t>(last);
        ++chunks;
    }
    char ex[128];
    std::snprintf(ex, sizeof ex, "total=%llu chunks=%d src_size=%zu",
                  static_cast<unsigned long long>(total), chunks, kSrcBytes);
    cell("eof_loop_total", last, errno, ex);
    ::close(s);
    ::close(d);
}

// --- item 3 (access class): destination-side failure before first attempt ---

void dest_failure(const std::string& dir) {
    const int s = open_src(dir);
    const int d_ro = ::open((dir + "/dst-df-ro.bin").c_str(),
                            O_RDONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (s < 0 || d_ro < 0) {
        cell("dest_failure_open_failed", -1, errno);
        return;
    }
    std::int64_t si = 0, di = 0;
    cellf("dest_rdonly_first_attempt",
          [&] { return cfr(s, &si, d_ro, &di, kLen); });
    const off_t sz = ::lseek(d_ro, 0, SEEK_END);
    kv("dest_rdonly", "dst_size_after", static_cast<long long>(sz));
    ::close(s);
    ::close(d_ro);
}

// --- item 5 + 14: EXDEV cross-filesystem, caller-explicit fallback ----------

void cross_fs(const std::string& dir_a, const std::string& dir_b) {
    for (int dir_i = 0; dir_i < 2; ++dir_i) {
        const std::string& sdir = dir_i == 0 ? dir_a : dir_b;
        const std::string& ddir = dir_i == 0 ? dir_b : dir_a;
        const int s = open_src(sdir);
        const int d = open_dst(ddir, "dst-xdev.bin", 0);
        if (s < 0 || d < 0) {
            cell("cross_open_failed", -1, errno);
            continue;
        }
        std::int64_t si = kOff, di = kOff;
        ssize_t r;
        int e;
        errno = 0;
        r = cfr(s, &si, d, &di, kLen);
        e = errno;
        char name[64];
        std::snprintf(name, sizeof name, "crossfs_cfr_%s_to_%s",
                      dir_i == 0 ? "a" : "b", dir_i == 0 ? "b" : "a");
        cell(name, r, e);
        const off_t dsz = ::lseek(d, 0, SEEK_END);
        kv(name, "dst_size_after", static_cast<long long>(dsz));

        if (r < 0) {
            std::uint64_t total = 0;
            std::vector<std::byte> buf(kLen);
            ssize_t rr = 1;
            while (rr > 0) {
                do {
                    rr = ::pread(s, buf.data(), kLen,
                                 static_cast<off_t>(total + kOff));
                } while (rr < 0 && errno == EINTR);
                if (rr <= 0)
                    break;
                ssize_t wr = 0;
                bool werr = false;
                while (wr < rr) {
                    const ssize_t w = ::pwrite(
                        d, buf.data() + wr, static_cast<std::size_t>(rr) - wr,
                        static_cast<off_t>(kOff + total + wr));
                    if (w < 0) {
                        if (errno == EINTR)
                            continue;
                        werr = true;
                        break;
                    }
                    wr += w;
                }
                if (werr)
                    break;
                total += static_cast<std::uint64_t>(rr);
            }
            const bool ok = range_matches(s, d, kOff, kLen, kOff);
            char ex[128];
            std::snprintf(ex, sizeof ex,
                          "fallback=explicit_caller_loop copied=%llu match=%d",
                          static_cast<unsigned long long>(total), ok ? 1 : 0);
            cell("crossfs_explicit_fallback", 0, 0, ex);
        }
        ::close(s);
        ::close(d);
    }
    const int s = open_src(dir_a);
    const int d = open_dst(dir_b, "dst-xdev-sf.bin", 0);
    if (s >= 0 && d >= 0) {
        ::lseek(d, static_cast<off_t>(kOff), SEEK_SET);
        off_t oi = static_cast<off_t>(kOff);
        cellf("crossfs_sendfile_a_to_b",
              [&] { return ::sendfile(d, s, &oi, kLen); });
    }
    if (s >= 0) ::close(s);
    if (d >= 0) ::close(d);
}

// --- item 8: non-regular file types -----------------------------------------

void non_regular(const std::string& dir) {
    const int s = open_src(dir);
    const int d = open_dst(dir, "dst-type.bin", 0);
    const int devnull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    const int devzero = ::open("/dev/zero", O_RDONLY | O_CLOEXEC);
    int pfd[2];
    const bool have_pipe = ::pipe2(pfd, O_CLOEXEC) == 0;
    const int dirfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    std::int64_t si = kOff, di = kOff;
    if (s >= 0 && devnull >= 0)
        cellf("type_dst_devnull",
              [&] { return cfr(s, &si, devnull, &di, kLen); });
    si = kOff;
    di = kOff;
    if (devzero >= 0 && d >= 0)
        cellf("type_src_devzero",
              [&] { return cfr(devzero, &si, d, &di, kLen); });
    if (have_pipe && d >= 0) {
        si = kOff;
        di = kOff;
        cellf("type_src_pipe", [&] { return cfr(pfd[0], &si, d, &di, kLen); });
        si = kOff;
        di = kOff;
        cellf("type_dst_pipe", [&] { return cfr(s, &si, pfd[1], &di, kLen); });
    }
    if (s >= 0 && dirfd >= 0) {
        si = kOff;
        di = kOff;
        cellf("type_dst_dirfd", [&] { return cfr(s, &si, dirfd, &di, kLen); });
    }
    if (dirfd >= 0 && d >= 0) {
        si = kOff;
        di = kOff;
        cellf("type_src_dirfd", [&] { return cfr(dirfd, &si, d, &di, kLen); });
    }
    if (s >= 0) ::close(s);
    if (d >= 0) ::close(d);
    if (devnull >= 0) ::close(devnull);
    if (devzero >= 0) ::close(devzero);
    if (have_pipe) { ::close(pfd[0]); ::close(pfd[1]); }
    if (dirfd >= 0) ::close(dirfd);
}

// --- item 9: destination O_APPEND -------------------------------------------

void o_append(const std::string& dir) {
    const int s = open_src(dir);
    const int d = open_dst(dir, "dst-ap.bin", O_APPEND);
    if (s < 0 || d < 0) {
        cell("oappend_open_failed", -1, errno);
        return;
    }
    std::int64_t si = kOff, di = kOff;
    cellf("oappend_dst_cfr", [&] { return cfr(s, &si, d, &di, kLen); });
    ::lseek(d, static_cast<off_t>(kOff), SEEK_SET);
    off_t oi = static_cast<off_t>(kOff);
    cellf("oappend_dst_sendfile",
          [&] { return ::sendfile(d, s, &oi, kLen); });
    const off_t sz = ::lseek(d, 0, SEEK_END);
    kv("oappend_dst", "dst_size_after", static_cast<long long>(sz));
    ::close(s);
    ::close(d);
}

// --- item 10: same-file and overlapping ranges -------------------------------

void same_file(const std::string& dir) {
    const int fd = open_src(dir);
    if (fd < 0) {
        cell("same_file_open_failed", -1, errno);
        return;
    }
    std::int64_t a = 0, b = 0;
    cellf("same_exact_overlap", [&] { return cfr(fd, &a, fd, &b, kLen); });

    a = 0;
    b = 2048;
    cellf("same_partial_overlap", [&] { return cfr(fd, &a, fd, &b, kLen); });

    a = 0;
    b = kLen;
    ssize_t r;
    int e;
    errno = 0;
    r = cfr(fd, &a, fd, &b, kLen);
    e = errno;
    cell("same_adjacent", r, e);
    if (r == static_cast<ssize_t>(kLen))
        kv("same_adjacent", "content_match",
           range_matches(fd, fd, kLen, kLen, 0));

    a = 0;
    b = 6000;
    errno = 0;
    r = cfr(fd, &a, fd, &b, kLen);
    e = errno;
    cell("same_gap", r, e);
    if (r == static_cast<ssize_t>(kLen))
        kv("same_gap", "content_match", range_matches(fd, fd, 6000, kLen, 0));

    a = kOff;
    b = static_cast<std::int64_t>(kSrcBytes + 4096);
    errno = 0;
    r = cfr(fd, &a, fd, &b, kLen);
    e = errno;
    char ex[64];
    std::snprintf(ex, sizeof ex, "dst_off_after=%lld", static_cast<long long>(b));
    cell("same_self_extend", r, e, ex);

    ::close(fd);
}

// --- item 13: shared cursors under explicit-offset form ----------------------

void cursors(const std::string& dir) {
    const int s = open_src(dir);
    const int d = open_dst(dir, "dst-cur.bin", 0);
    if (s < 0 || d < 0) {
        cell("cursors_open_failed", -1, errno);
        return;
    }
    ::lseek(s, 777, SEEK_SET);
    ::lseek(d, 888, SEEK_SET);
    std::int64_t si = kOff, di = kOff;
    ssize_t r;
    int e;
    errno = 0;
    r = cfr(s, &si, d, &di, kLen);
    e = errno;
    char ex[192];
    std::snprintf(ex, sizeof ex,
                  "src_pos_before=777 dst_pos_before=888 src_pos_after=%ld "
                  "dst_pos_after=%ld off_arg_after=(%lld,%lld)",
                  static_cast<long>(pos(s)), static_cast<long>(pos(d)),
                  static_cast<long long>(si), static_cast<long long>(di));
    cell("cursors_explicit_untouched", r, e, ex);

    ::lseek(s, 777, SEEK_SET);
    ::lseek(d, 888, SEEK_SET);
    errno = 0;
    const ssize_t r2 = cfr(s, nullptr, d, nullptr, kLen);
    e = errno;
    std::snprintf(ex, sizeof ex, "src_pos_after=%ld dst_pos_after=%ld",
                  static_cast<long>(pos(s)), static_cast<long>(pos(d)));
    cell("cursors_null_offset_consumed", r2, e, ex);
    ::close(s);
    ::close(d);
}

// --- item 3 (EFBIG class): mid-copy destination failure via RLIMIT_FSIZE -----

void efbig(const std::string& dir) {
    ::signal(SIGXFSZ, SIG_IGN);
    const int s = open_src(dir);
    if (s < 0) {
        cell("efbig_src_open_failed", -1, errno);
        return;
    }
    struct ::rlimit rl {}, saved {};
    ::getrlimit(RLIMIT_FSIZE, &saved);
    rl = saved;
    rl.rlim_cur = 4096;
    if (::setrlimit(RLIMIT_FSIZE, &rl) != 0) {
        cell("efbig_setrlimit_failed", -1, errno);
        ::close(s);
        return;
    }
    const int d = open_dst(dir, "dst-efbig.bin", 0);
    if (d < 0) {
        cell("efbig_dst_open_failed", -1, errno);
        ::close(s);
        ::setrlimit(RLIMIT_FSIZE, &saved);
        return;
    }
    std::uint64_t confirmed = 0;
    ssize_t last = 1;
    int last_err = 0;
    int attempted = 0;
    while (confirmed < kSrcBytes && attempted < 8) {
        std::int64_t a = static_cast<std::int64_t>(confirmed);
        std::int64_t b = static_cast<std::int64_t>(confirmed);
        errno = 0;
        last = cfr(s, &a, d, &b, kLen);
        last_err = errno;
        ++attempted;
        if (last < 0)
            break;
        confirmed += static_cast<std::uint64_t>(last);
    }
    const off_t dsz = ::lseek(d, 0, SEEK_END);
    char ex[192];
    std::snprintf(ex, sizeof ex,
                  "confirmed_before_fail=%llu attempts=%d dst_size_after=%lld "
                  "partial_effect=%d",
                  static_cast<unsigned long long>(confirmed), attempted,
                  static_cast<long long>(dsz), dsz > 0 ? 1 : 0);
    cell("efbig_midcopy", last, last_err, ex);
    ::close(s);
    ::close(d);
    ::setrlimit(RLIMIT_FSIZE, &saved);
}

// --- item 11: concurrent source modification disclosure cell -----------------

struct conc_result {
    ssize_t last = 0;
    int err = 0;
    std::uint64_t confirmed = 0;
    int chunks = 0;
};

conc_result copy_loop_cfr(int s, int d, std::size_t len, std::size_t total) {
    conc_result r;
    while (r.confirmed < total && r.chunks < 8192) {
        std::int64_t a = static_cast<std::int64_t>(r.confirmed);
        std::int64_t b = static_cast<std::int64_t>(r.confirmed);
        errno = 0;
        const ssize_t got = cfr(s, &a, d, &b, len);
        ++r.chunks;
        if (got < 0) {
            r.last = -1;
            r.err = errno;
            break;
        }
        r.last = got;
        r.confirmed += static_cast<std::uint64_t>(got);
        if (got == 0)
            break;
    }
    return r;
}

conc_result copy_loop_rw(int s, int d, std::size_t len, std::size_t total) {
    conc_result r;
    std::vector<std::byte> buf(len);
    while (r.confirmed < total && r.chunks < 8192) {
        ssize_t got;
        do {
            got = ::pread(s, buf.data(), len, static_cast<off_t>(r.confirmed));
        } while (got < 0 && errno == EINTR);
        ++r.chunks;
        if (got < 0) {
            r.last = -1;
            r.err = errno;
            break;
        }
        if (got == 0) {
            r.last = 0;
            break;
        }
        ssize_t wr = 0;
        bool werr = false;
        while (wr < got) {
            const ssize_t w = ::pwrite(d, buf.data() + wr,
                                       static_cast<std::size_t>(got) - wr,
                                       static_cast<off_t>(r.confirmed + wr));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                r.last = -1;
                r.err = errno;
                werr = true;
                break;
            }
            wr += w;
        }
        if (werr)
            break;
        r.last = got;
        r.confirmed += static_cast<std::uint64_t>(got);
    }
    return r;
}

off_t size_of(int fd) { return ::lseek(fd, 0, SEEK_END); }

void concurrent(const std::string& dir, std::size_t total) {
    const std::size_t len = 65536;
    for (int scenario = 0; scenario < 3; ++scenario) {
        for (int engine = 0; engine < 2; ++engine) {
            const std::string spath = dir + "/src-conc.bin";
            {
                const int w = ::open(spath.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                                     0644);
                if (w < 0) {
                    cell("conc_src_write_failed", -1, errno);
                    return;
                }
                std::vector<std::byte> buf(len);
                for (std::size_t off = 0; off < total; off += len) {
                    for (std::size_t i = 0; i < len; ++i)
                        buf[i] = pat(off + i);
                    if (::pwrite(w, buf.data(), len,
                                 static_cast<off_t>(off)) !=
                        static_cast<ssize_t>(len)) {
                        cell("conc_src_write_failed", -1, errno);
                        ::close(w);
                        return;
                    }
                }
                ::close(w);
            }
            const std::string dpath = dir + "/dst-conc.bin";
            const int s = ::open(spath.c_str(), O_RDONLY | O_CLOEXEC);
            const int d = ::open(dpath.c_str(),
                                 O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            const int wfd = ::open(spath.c_str(), O_RDWR | O_CLOEXEC);
            if (s < 0 || d < 0 || wfd < 0) {
                cell("conc_open_failed", -1, errno);
                return;
            }
            std::atomic<bool> go{false};
            std::thread writer([&] {
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                // interleave mid-copy: act once the destination has grown
                // past a quarter of the request (approximate window)
                while (size_of(d) < static_cast<off_t>(total / 4) &&
                       size_of(s) == static_cast<off_t>(total))
                    std::this_thread::yield();
                if (scenario == 0) {
                    (void)::ftruncate(wfd, static_cast<off_t>(total / 2));
                } else if (scenario == 1) {
                    std::vector<std::byte> buf(len);
                    for (std::size_t i = 0; i < len; ++i)
                        buf[i] =
                            pat(~static_cast<std::uint64_t>(i) * 2654435761ull);
                    for (std::size_t off = 0; off < total / 4; off += len)
                        (void)::pwrite(wfd, buf.data(), len,
                                       static_cast<off_t>(off));
                } else {
                    std::vector<std::byte> buf(64);
                    (void)::pwrite(wfd, buf.data(), 64,
                                   static_cast<off_t>(total));
                }
            });
            go.store(true, std::memory_order_release);
            conc_result r = engine == 0 ? copy_loop_cfr(s, d, len, total)
                                        : copy_loop_rw(s, d, len, total);
            writer.join();
            const off_t ssz = ::lseek(s, 0, SEEK_END);
            const off_t dsz = ::lseek(d, 0, SEEK_END);
            char ex[384];
            std::snprintf(
                ex, sizeof ex,
                "engine=%s scenario=%s confirmed=%llu chunks=%d "
                "src_size_now=%lld dst_size_now=%lld",
                engine == 0 ? "cfr" : "pread_pwrite",
                scenario == 0 ? "src_truncate_half"
                              : scenario == 1 ? "src_overwrite_quarter"
                                              : "src_append_beyond",
                static_cast<unsigned long long>(r.confirmed), r.chunks,
                static_cast<long long>(ssz), static_cast<long long>(dsz));
            if (scenario == 1) {
                // disclose the torn state: classify destination bytes in the
                // overwritten quarter as old-pattern / new-pattern / other
                std::uint64_t old_b = 0, new_b = 0, other_b = 0;
                std::vector<std::byte> got(total / 4);
                const ssize_t rd = ::pread(d, got.data(), got.size(), 0);
                if (rd == static_cast<ssize_t>(got.size())) {
                    for (std::size_t i = 0; i < got.size(); ++i) {
                        const bool o = got[i] == pat(i);
                        const bool n =
                            got[i] ==
                            pat(~static_cast<std::uint64_t>(i) * 2654435761ull);
                        old_b += o;
                        new_b += n;
                        other_b += !o && !n;
                    }
                }
                char mx[160];
                std::snprintf(mx, sizeof mx,
                              " old_bytes=%llu new_bytes=%llu other_bytes=%llu",
                              static_cast<unsigned long long>(old_b),
                              static_cast<unsigned long long>(new_b),
                              static_cast<unsigned long long>(other_b));
                std::strcat(ex, mx);
            }
            cell("concurrent_modification", r.last, r.err, ex);
            ::close(s);
            ::close(d);
            ::close(wfd);
            ::unlink(spath.c_str());
            ::unlink(dpath.c_str());
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s sem <dirA> <dirB> | efbig <dir> | conc <dir> <total>\n",
                     argv[0]);
        return 2;
    }
    struct ::utsname u {};
    if (::uname(&u) == 0)
        std::printf("kernel=%s\n", u.release);
    const std::string cmd = argv[1];
    if (cmd == "sem" && argc == 4) {
        const std::string a = argv[2], b = argv[3];
        std::printf("fs=a dir=%s\n", a.c_str());
        admission(a);
        eof_stop(a);
        dest_failure(a);
        non_regular(a);
        o_append(a);
        same_file(a);
        cursors(a);
        std::printf("fs=b dir=%s\n", b.c_str());
        admission(b);
        eof_stop(b);
        dest_failure(b);
        non_regular(b);
        o_append(b);
        same_file(b);
        cursors(b);
        cross_fs(a, b);
        return 0;
    }
    if (cmd == "efbig" && argc == 3) {
        efbig(argv[2]);
        return 0;
    }
    if (cmd == "conc" && argc == 4) {
        concurrent(argv[2],
                   static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10)));
        return 0;
    }
    std::fprintf(stderr, "unknown subcommand\n");
    return 2;
}
