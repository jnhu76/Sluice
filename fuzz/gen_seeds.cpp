// One-time seed corpus generator. Not part of the fuzz build or CI: compile
// once with plain clang++, run it to (re)emit deterministic binary seed files
// into fuzz/corpus/<target>/.
//
// WAL record layout (little-endian), used to synthesize valid/invalid frames:
//   magic(4) | length(4) | payload(length) | checksum(4)
//   magic = 0x57414C  ("WAL")
//   checksum = sum(payload bytes) mod 2^32
//
// copy_all_fault config layout mirrors fuzz::decode_config():
//   scratch:u32 | limit:mod3(u8) | limit_val:u32 | rshort:u32 | wshort:u32
//   rfail:mod3(u8) | rfail_thresh:u8 | wfail:mod3(u8) | wfail_thresh:u8
//   injected:mod3(u8) | strategy:mod5(u8) | broken:mod2(u8)
//   capability:mod2(u8) | buffered_prefix:u32 | consume_fail:mod2(u8)
//   writer_zero_progress:mod2(u8) | early_eof_after:u32
//   then the source payload bytes.
//
// NOTE: the mod-encoded selectors are emitted as their target VALUE (not as a
// raw byte that the harness would reduce mod N). take_mod(count) returns
// take_u8() % count, so emitting the value v directly yields v only when v <
// count, which holds for every selector below (0..count-1).
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::filesystem::path g_dir;

static void write_file(const char* name, std::span<const std::byte> bytes) {
    auto p = g_dir / name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::printf("  %s (%zu bytes)\n", name, bytes.size());
}

// Overload for the convenient write_file("name", {b(...), b(...)}) call shape,
// which does not deduce std::span from a braced-init-list.
static void write_file(const char* name, std::initializer_list<std::byte> bytes) {
    write_file(name, std::span<const std::byte>(bytes.begin(), bytes.size()));
}

static std::byte b(std::uint8_t v) { return std::byte{v}; }

static void put_le_u32(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(b(static_cast<std::uint8_t>(v & 0xFF)));
    out.push_back(b(static_cast<std::uint8_t>((v >> 8) & 0xFF)));
    out.push_back(b(static_cast<std::uint8_t>((v >> 16) & 0xFF)));
    out.push_back(b(static_cast<std::uint8_t>((v >> 24) & 0xFF)));
}

static std::uint32_t checksum_of(std::span<const std::byte> payload) {
    std::uint64_t sum = 0;
    for (auto by : payload) {
        sum += std::to_integer<unsigned>(by);
    }
    return static_cast<std::uint32_t>(sum & 0xFFFFFFFFU);
}

static std::uint32_t checksum_of(std::initializer_list<std::byte> payload) {
    return checksum_of(std::span<const std::byte>(payload.begin(), payload.size()));
}

// Append one complete WAL record (header | payload | checksum).
static void append_record(std::vector<std::byte>& out, std::span<const std::byte> payload) {
    put_le_u32(out, 0x57414C);
    put_le_u32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    put_le_u32(out, checksum_of(payload));
}

static void append_record(std::vector<std::byte>& out, std::initializer_list<std::byte> payload) {
    append_record(out, std::span<const std::byte>(payload.begin(), payload.size()));
}

// wal_read_record input = [config_byte] [stream...]. config_byte 0 => max_short=1.
static void gen_wal_read_record() {
    std::printf("wal_read_record:\n");
    g_dir = "fuzz/corpus/wal_read_record";
    fs::create_directories(g_dir);

    // empty input.
    write_file("empty", {});
    // one-byte input (config byte 0, no stream).
    write_file("one_byte", {b(0)});
    // seven-byte truncated header (config 0 + 6 header bytes).
    write_file("truncated_header", {b(0), b(0x4C), b(0x41), b(0x57), b(0x00), b(0x00), b(0x00)});
    // valid empty record (config 0 + 8-byte header length=0 + 4-byte checksum 0).
    {
        std::vector<std::byte> v;
        v.push_back(b(0));
        append_record(v, {});
        write_file("valid_empty_record", v);
    }
    // valid one-byte record (payload 0x42).
    {
        std::vector<std::byte> v;
        v.push_back(b(0));
        append_record(v, {b(0x42)});
        write_file("valid_one_byte_record", v);
    }
    // valid binary record containing 0x00 and 0xFF.
    {
        std::vector<std::byte> v;
        v.push_back(b(0));
        append_record(v, {b(0x00), b(0xFF), b(0x10), b(0x20)});
        write_file("valid_binary_record", v);
    }
    // bad magic (magic bytes flipped).
    {
        std::vector<std::byte> v;
        v.push_back(b(0));
        put_le_u32(v, 0xDEADBEEF);
        put_le_u32(v, 0); // length 0
        put_le_u32(v, 0); // checksum 0
        write_file("bad_magic", v);
    }
    // bad checksum (valid frame, corrupted checksum).
    {
        std::vector<std::byte> v;
        v.push_back(b(0));
        append_record(v, {b(0x11), b(0x22)});
        v.back() = b(0xFF); // corrupt the last checksum byte
        write_file("bad_checksum", v);
    }
    // truncated payload (length says 10, only 2 payload bytes present).
    {
        std::vector<std::byte> v;
        v.push_back(b(0));
        put_le_u32(v, 0x57414C);
        put_le_u32(v, 10);
        v.insert(v.end(), {b(0x01), b(0x02)});
        write_file("truncated_payload", v);
    }
    // truncated checksum (payload complete, checksum missing last byte).
    {
        std::vector<std::byte> v;
        v.push_back(b(0));
        put_le_u32(v, 0x57414C);
        put_le_u32(v, 1);
        v.push_back(b(0x05));            // payload
        v.push_back(b(checksum_of({b(0x05)}) & 0xFF)); // only first cksum byte
        write_file("truncated_checksum", v);
    }
    // huge declared length with no payload (length = 0xFFFFFFFF).
    {
        std::vector<std::byte> v;
        v.push_back(b(0));
        put_le_u32(v, 0x57414C);
        put_le_u32(v, 0xFFFFFFFF);
        write_file("huge_declared_length", v);
    }
    // two valid concatenated records.
    {
        std::vector<std::byte> v;
        v.push_back(b(0));
        append_record(v, {b(0xAA)});
        append_record(v, {b(0xBB), b(0xCC)});
        write_file("two_records", v);
    }
    // valid record plus trailing garbage.
    {
        std::vector<std::byte> v;
        v.push_back(b(0));
        append_record(v, {b(0x99)});
        v.insert(v.end(), {b(0xDE), b(0xAD), b(0xBE), b(0xEF)});
        write_file("record_plus_garbage", v);
    }
}

// wal_roundtrip input = raw payload.
static void gen_wal_roundtrip() {
    std::printf("wal_roundtrip:\n");
    g_dir = "fuzz/corpus/wal_roundtrip";
    fs::create_directories(g_dir);
    write_file("empty", {});
    write_file("one_byte", {b(0x42)});
    write_file("binary_00_ff", {b(0x00), b(0xFF), b(0x07), b(0x70)});
    // 256-byte counting payload.
    {
        std::vector<std::byte> v(256);
        for (std::size_t i = 0; i < v.size(); ++i) {
            v[i] = b(static_cast<std::uint8_t>(i));
        }
        write_file("counting_256", v);
    }
}

// copy_all_fault config + source. See the layout comment at the top.
//
// Selector bytes (limit, rfail, wfail, injected, strategy, broken, capability,
// consume_fail, writer_zero_progress) are emitted as their TARGET value because
// take_mod(count) returns take_u8() % count and the value is already < count.
static void append_config(std::vector<std::byte>& out, std::uint32_t scratch, std::uint8_t limit,
                          std::uint32_t limit_val, std::uint32_t rshort, std::uint32_t wshort,
                          std::uint8_t rfail, std::uint8_t rfail_t, std::uint8_t wfail,
                          std::uint8_t wfail_t, std::uint8_t injected, std::uint8_t strategy,
                          std::uint8_t broken, std::uint8_t capability,
                          std::uint32_t buffered_prefix, std::uint8_t consume_fail,
                          std::uint8_t writer_zero_progress, std::uint32_t early_eof_after) {
    put_le_u32(out, scratch);
    out.push_back(b(limit));
    put_le_u32(out, limit_val);
    put_le_u32(out, rshort);
    put_le_u32(out, wshort);
    out.push_back(b(rfail));
    out.push_back(b(rfail_t));
    out.push_back(b(wfail));
    out.push_back(b(wfail_t));
    out.push_back(b(injected));
    out.push_back(b(strategy));
    out.push_back(b(broken));
    out.push_back(b(capability));
    put_le_u32(out, buffered_prefix);
    out.push_back(b(consume_fail));
    out.push_back(b(writer_zero_progress));
    put_le_u32(out, early_eof_after);
}

// Defaults: plain reader, no buffered prefix, no consume fail, no zero-progress,
// no early EOF. Keeps existing seeds' intent while filling the new config fields
// with benign values.
static std::vector<std::byte> cfg(std::uint32_t scratch, std::uint8_t limit, std::uint32_t limit_val,
                                  std::uint32_t rshort, std::uint32_t wshort, std::uint8_t rfail = 0,
                                  std::uint8_t rfail_t = 0, std::uint8_t wfail = 0,
                                  std::uint8_t wfail_t = 0, std::uint8_t injected = 0,
                                  std::uint8_t strategy = 1 /*Auto*/, std::uint8_t broken = 0,
                                  std::uint8_t capability = 0 /*Plain*/,
                                  std::uint32_t buffered_prefix = 0,
                                  std::uint8_t consume_fail = 0,
                                  std::uint8_t writer_zero_progress = 0,
                                  std::uint32_t early_eof_after = 0) {
    std::vector<std::byte> v;
    append_config(v, scratch, limit, limit_val, rshort, wshort, rfail, rfail_t, wfail, wfail_t,
                  injected, strategy, broken, capability, buffered_prefix, consume_fail,
                  writer_zero_progress, early_eof_after);
    return v;
}

// limit encoding: 0=Unlimited, 1=Zero, 2=Bounded.
static void gen_copy_all_fault() {
    std::printf("copy_all_fault:\n");
    g_dir = "fuzz/corpus/copy_all_fault";
    fs::create_directories(g_dir);

    auto src = [&](std::vector<std::byte>& v, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            v.push_back(b(static_cast<std::uint8_t>(i & 0xFF)));
        }
    };

    // empty source (Auto, unlimited, scratch 64, short 64).
    {
        auto v = cfg(64, 0, 0, 64, 64);
        write_file("empty_source", v);
    }
    // one-byte source.
    {
        auto v = cfg(64, 0, 0, 64, 64);
        v.push_back(b(0x55));
        write_file("one_byte_source", v);
    }
    // large source relative to scratch (256 bytes, scratch 8).
    {
        auto v = cfg(8, 0, 0, 8, 8);
        src(v, 256);
        write_file("large_source", v);
    }
    // zero limit.
    {
        auto v = cfg(64, 1, 0, 64, 64);
        src(v, 100);
        write_file("zero_limit", v);
    }
    // limit smaller than source (limit 10, source 100).
    {
        auto v = cfg(64, 2, 10, 64, 64);
        src(v, 100);
        write_file("limit_smaller", v);
    }
    // limit equal to source (limit 100, source 100).
    {
        auto v = cfg(64, 2, 100, 64, 64);
        src(v, 100);
        write_file("limit_equal", v);
    }
    // limit larger than source (limit 200, source 50).
    {
        auto v = cfg(64, 2, 200, 64, 64);
        src(v, 50);
        write_file("limit_larger", v);
    }
    // empty scratch with non-zero/unlimited copy (expect invalid_state).
    {
        auto v = cfg(0, 0, 0, 64, 64);
        src(v, 100);
        write_file("empty_scratch", v);
    }
    // reader failure before first byte (rfail=AfterCalls(1), threshold 0).
    {
        auto v = cfg(64, 0, 0, 64, 64, 1, 0);
        src(v, 100);
        write_file("reader_fail_before_first", v);
    }
    // reader failure after a prefix (AfterCalls threshold 2).
    {
        auto v = cfg(64, 0, 0, 64, 64, 1, 2);
        src(v, 100);
        write_file("reader_fail_after_prefix", v);
    }
    // writer failure before first byte.
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, 1, 0);
        src(v, 100);
        write_file("writer_fail_before_first", v);
    }
    // writer failure after a prefix (AfterBytes threshold 5).
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, 2, 5);
        src(v, 100);
        write_file("writer_fail_after_prefix", v);
    }
    // one-byte short reads.
    {
        auto v = cfg(64, 0, 0, 1, 64);
        src(v, 100);
        write_file("one_byte_short_reads", v);
    }
    // one-byte short writes.
    {
        auto v = cfg(64, 0, 0, 64, 1);
        src(v, 100);
        write_file("one_byte_short_writes", v);
    }
    // broken reader over-report.
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, 0, 0, 0, 1, 1);
        src(v, 100);
        write_file("broken_reader", v);
    }
    // deferred strategy Reject (strategy 3).
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, 0, 0, 0, 3, 0);
        src(v, 100);
        write_file("deferred_reject", v);
    }
    // deferred strategy FallbackToAuto (strategy 4).
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, 0, 0, 0, 4, 0);
        src(v, 100);
        write_file("deferred_fallback", v);
    }
    // binary source containing 0x00 and 0xFF.
    {
        auto v = cfg(64, 0, 0, 64, 64);
        v.insert(v.end(), {b(0x00), b(0xFF), b(0x55), b(0xAA)});
        write_file("binary_source", v);
    }

    // --- §5: BufferedReadable coverage. ---

    // buffered prefix fully satisfies an unlimited copy (Auto).
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, 0, 0, 0, 1, 0, /*capability*/ 1,
                     /*buffered_prefix*/ 100);
        src(v, 100);
        write_file("buffered_full", v);
    }
    // buffered prefix smaller than source: drains prefix then continues through
    // scratch (exercises the buffered-to-scratch transition CB5).
    {
        auto v = cfg(8, 0, 0, 8, 8, 0, 0, 0, 0, 0, 1, 0, /*capability*/ 1,
                     /*buffered_prefix*/ 3);
        src(v, 100);
        write_file("buffered_then_scratch", v);
    }
    // buffered + bounded limit smaller than buffered prefix (limit clamp CB2;
    // expected killer for M-COPY-07).
    {
        auto v = cfg(64, 2, 10, 64, 64, 0, 0, 0, 0, 0, 1, 0, /*capability*/ 1,
                     /*buffered_prefix*/ 100);
        src(v, 100);
        write_file("buffered_limit_smaller", v);
    }
    // Scratch strategy with a buffered reader: must NOT touch the buffered
    // capability (peek/consume never called).
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, 0, 0, 0, 0 /*Scratch*/, 0,
                     /*capability*/ 1, /*buffered_prefix*/ 100);
        src(v, 100);
        write_file("buffered_scratch_strategy", v);
    }
    // BufferedFirst with a writer failure while draining buffered bytes: the
    // failed region consumes nothing and buffered bytes are preserved (CB3;
    // expected killer for M-COPY-08).
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, /*wfail AfterCalls*/ 1, /*thresh*/ 0, 0,
                     /*BufferedFirst*/ 2, 0, /*capability*/ 1, /*buffered_prefix*/ 100);
        src(v, 100);
        write_file("buffered_writer_fail", v);
    }
    // consume_buffered fails after a successful write (CB6).
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, 0, 0, 0, /*BufferedFirst*/ 2, 0,
                     /*capability*/ 1, /*buffered_prefix*/ 100, /*consume_fail*/ 1);
        src(v, 100);
        write_file("buffered_consume_fail", v);
    }

    // --- §19: source beyond the old 4096 truncation bound. Proves bytes past
    //     offset 4095 participate in the output oracle (unlimited, fault-free). ---
    {
        auto v = cfg(64, 0, 0, 64, 64);
        src(v, 5000);
        write_file("source_beyond_4096", v);
    }

    // --- §20: zero-progress writer and early clean EOF. ---

    // Zero-progress writer: copy_all returns invalid_state; target terminates.
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
                     /*writer_zero_progress*/ 1);
        src(v, 100);
        write_file("writer_zero_progress", v);
    }
    // early_eof_before_first: clean EOF before any byte is transferred. The
    // model's early-EOF predicate fires on the first read only when the source
    // is already exhausted (clean EOF), which is the genuine "EOF before first
    // byte" contract. Distinct from an injected reader error or broken-reader
    // over-report.
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0);
        // no source bytes appended -> first read returns 0 -> clean EOF
        write_file("early_eof_before_first", v);
    }
    // early_eof_after_prefix: clean EOF after a configured number of successful
    // bytes even though more source bytes remain.
    {
        auto v = cfg(64, 0, 0, 64, 64, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
                     /*early_eof_after*/ 5);
        src(v, 100);
        write_file("early_eof_after_prefix", v);
    }
}

int main() {
    std::printf("Generating seed corpora:\n");
    gen_wal_read_record();
    gen_wal_roundtrip();
    gen_copy_all_fault();
    std::printf("Done.\n");
    return 0;
}
