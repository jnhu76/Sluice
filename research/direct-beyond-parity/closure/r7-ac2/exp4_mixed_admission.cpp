// EXP-4 (R7 AC-2): zero-length and illegal-offset members mixed in a batch —
// per-member admission oracle established through the ACTUAL public read_at
// (master bdfd0d8d tree). The gathered candidate's contract says admission
// follows SEM-03 precedence PER MEMBER; the oracle table below is exactly
// what each member must produce independently, and the batch must not let
// any member's verdict change another's (M2 §2.5-c error-fusion ban).
//
// Members exercised, each through its own read_at call on the same File:
//   a  zero-length dst at offset = UINT64_MAX (unrepresentable) -> success 0,
//      no offset/range check (SEM-03 step 3; mirrors
//      blocking_read_at_empty_buffer_ignores_unrepresentable_offset)
//   b  zero-length dst at a valid offset                        -> success 0
//   c  len=8 at offset = UINT64_MAX (range unrepresentable)     -> invalid_argument
//   d  len=8 at a valid offset                                  -> count 8
//   g  zero-length with dst.data()==nullptr                     -> success 0
//      (nullptr is rejected only for nonempty buffers)
//   e  zero-length on a CLOSED file                             -> invalid_state
//      (precedence: closed wins over the zero-length no-op, SEM-03 steps 1<3)
//   f  len=8 on the closed file                                 -> invalid_state
//
// The member table IS the oracle a gathered call must reproduce per member.
#include <sluice/blocking/file.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileInitialContents;
using sluice::FileOpen;
using sluice::IoError;
using sluice::blocking::read_at;
using sluice::blocking::write_at;

namespace {

const char* code_name(IoError::Code c) {
    switch (c) {
        case IoError::Code::invalid_argument: return "invalid_argument";
        case IoError::Code::invalid_state: return "invalid_state";
        default: return "other";
    }
}

int failures = 0;

void expect(const char* member, bool cond, const std::string& detail) {
    std::printf("%-40s %s%s\n", member, cond ? "OK  " : "FAIL", detail.c_str());
    if (!cond) ++failures;
}

}  // namespace

int main() {
    const std::string path = "/tmp/r7ac2-exp4.bin";
    {
        auto s = File::open(path, FileOpen{FileAccess::write_only,
                                           FileExistence::create_if_missing,
                                           FileInitialContents::truncate});
        if (!s) { std::printf("seed open failed\n"); return 2; }
        const std::byte pat[8] = {};
        if (!write_at(s.value(), 7000, pat)) { std::printf("seed write failed\n"); return 2; }
    }
    auto f = File::open(path, FileOpen{FileAccess::read_write});
    if (!f) { std::printf("open failed\n"); return 2; }
    File file = std::move(f.value());

    constexpr std::uint64_t MAX = ~std::uint64_t{0};
    std::byte buf[8];

    auto a = read_at(file, MAX, std::span<std::byte>{});
    expect("a zero-len @UINT64_MAX", a.has_value() && a.value() == 0,
           a ? " -> success 0 (offset skipped)" : " -> " + std::string(code_name(a.error().code)));

    auto b = read_at(file, 7000, std::span<std::byte>{});
    expect("b zero-len @valid", b.has_value() && b.value() == 0,
           b ? " -> success 0" : " -> " + std::string(code_name(b.error().code)));

    auto c = read_at(file, MAX, buf);
    expect("c len8 @UINT64_MAX", !c.has_value() && c.error().code == IoError::Code::invalid_argument,
           c ? " -> unexpected success" : " -> " + std::string(code_name(c.error().code)));

    auto d = read_at(file, 7000, buf);
    expect("d len8 @valid", d.has_value() && d.value() == 8,
           d ? " -> count 8" : " -> " + std::string(code_name(d.error().code)));

    auto g = read_at(file, MAX, std::span<std::byte>{static_cast<std::byte*>(nullptr), 0});
    expect("g zero-len nullptr @UINT64_MAX", g.has_value() && g.value() == 0,
           g ? " -> success 0" : " -> " + std::string(code_name(g.error().code)));

    if (!file.close()) { std::printf("close failed\n"); return 2; }
    auto e = read_at(file, MAX, std::span<std::byte>{});
    expect("e zero-len on CLOSED file", !e.has_value() && e.error().code == IoError::Code::invalid_state,
           e ? " -> unexpected success" : " -> " + std::string(code_name(e.error().code)));
    auto ff = read_at(file, 7000, buf);
    expect("f len8 on CLOSED file", !ff.has_value() && ff.error().code == IoError::Code::invalid_state,
           ff ? " -> unexpected success" : " -> " + std::string(code_name(ff.error().code)));

    std::printf("EXP4_GATE=%s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
    std::remove(path.c_str());
    return failures == 0 ? 0 : 1;
}
