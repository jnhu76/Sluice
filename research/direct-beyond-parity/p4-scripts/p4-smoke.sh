#!/usr/bin/env bash
# P4 preparation smoke gate (P4 plan §3.1 items 2-4, §4.1; smoke precedents
# p2-raw/kit/smoke-ladder.txt and p3-scripts/p3-smoke.sh).
# 1. identity gate: frozen archives, the three pinned kit binaries, kit
#    branch/HEAD/kit-source — mismatch stops the gate (§0 rule 2);
# 2. build gate: each pinned binary is reproduced byte-exact from the one
#    common kit source 2364be4a by a scratch git-archive build (Amendment 1);
#    installed binaries are never overwritten;
# 3. full arm inventory (23 arms x 3 binaries) with cross-arm verifiable
#    group checksum assertions and cross-binary same-arm equality;
# 4. Layer C resolution per workload shape: W01 single-span collapse is a
#    preregistered structural fact (plan §2) — no kit cell arm exists; the
#    P3 preadv/planner/copy_file_range probe arms stay runnable as
#    diagnostics (ladder-4455d98d, excluded from every P4 cell);
# 5. Parquet minimal smoke: one frozen-trace replay through the existing
#    R1 arm path, read-only (frozen artifacts re-hashed after the run).
# Smoke runs are gates, never a timing source (no pairing, no claims). Raw
# log lands in $P4_RAW_DIR/kit-smoke.txt; identity in $P4_RAW_DIR/identity.txt.
set -uo pipefail

RESEARCH_DIR=$(cd "$(dirname "$0")/.." && pwd)
RAW_DIR=${P4_RAW_DIR:-$RESEARCH_DIR/p4-raw}
SMOKE_LOG=$RAW_DIR/kit-smoke.txt
IDENTITY_TXT=$RAW_DIR/identity.txt

KIT_DIR=/home/jnhu/Source/Sluice-direct-cost-ladder/research/direct-cost-ladder
KIT_REPO=/home/jnhu/Source/Sluice-direct-cost-ladder
KIT_SOURCE_COMMIT=2364be4acf1790baa030afe1730d00ca9222f280
KIT_SOURCE_LADDER_SHA=75c19bcc4f2df35e00ac8ea67fb552b007a3eeba62f64d256a4dedf10a61b2fb

WT_B1=/home/jnhu/Source/Sluice-600-wt
WT_C1=/home/jnhu/Source/Sluice-direct-read-compose-prevalidated-steps
WT_C2=/home/jnhu/Source/Sluice-direct-write-compose-exit-staging
AR_B1_SHA=d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0
AR_C1_SHA=07d0680307c71cc412dfd021fd47c3098de57b580283df189f66dcdc2052049e
AR_C2_SHA=672510281abadba1cbda0a0e89f15e1dbb2c69f5a15bceb306b70196b420a807
HEAD_B1=77e5399a45998dea396699fa2e063c94a665945a
HEAD_C1=deaf03014173e13633a43f4db405c9a9e9860f23
HEAD_C2=476bd9ce9780594163350228e066a86b554c157d

BIN_LADDER_SHA=8945036746d03389e6f44df7a6b26ae5599f2f1bddda8825da28ebd11dc010e8
BIN_C1_SHA=8305d22ca4ff74db0017a24fd7867b85f4a48ec767fbfc5d49563402ce870381
BIN_C2_SHA=8bb6473b0f2455c2b092d94cc55ccaec20fe72ff40103bd9fb5c9151c7740065
LADDER_4455_SHA=5a53a9ad1ea00a9a26dee98543ef473ca15dcdfda86c2aa647b59cca4ec05507

DUDUDU=/home/jnhu/Source/dududu
PQ_BENCH=$DUDUDU/build/cmake-parquet/experiments/parquet/parquet_bench
PQ_BENCH_SHA=e24a6f5b993b95691d53f73730b3014f55f76b92299536516d4cdf9398f91d3a
PQ_TRACE_SHA=a059e58fbc8771d78b23cd74afc3e1e7d26a09e230165b474dfc5b5cf1b53724
PQ_TRACE_P2_SHA=4c67562626853ec75a9d48cb9626f283342db07c968bb8b7d893dd08a966950d
PQ_DATASET_SHA=6116e69cf20a89b7ca8a23e4d4dbd7d3759225a68e379d4776b3727bf3c05af6

READ_FIX=/tmp/sluice600/data64m.bin
READ_FIX_SHA=bc020fd15a622e081793b0fb8ff2383838e27f6d1cb2135f204aad8383b2993a
COLD_FIX_DIR=/var/tmp/sluice600-p1
SCRATCH=/tmp/sluice600-p4-smoke

FAILURES=0
CHECKS=0

mkdir -p "$RAW_DIR"
: > "$SMOKE_LOG"

log() { printf '%s\n' "$*" | tee -a "$SMOKE_LOG"; }
fail() { FAILURES=$((FAILURES + 1)); log "FAIL: $*"; }
check_eq() {
    local what=$1 a=$2 b=$3
    CHECKS=$((CHECKS + 1))
    if [ "$a" = "$b" ]; then
        log "ok: $what = $a"
    else
        fail "$what: '$a' != '$b'"
    fi
}

field() {
    printf '%s\n' "$1" | sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p"
}

sha_of() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

# run_ladder <binary> <args...>: echoes the arm= line on success.
run_ladder() {
    local bin=$1; shift
    local out rc
    out=$("$bin" "$@" 2>>"$SMOKE_LOG")
    rc=$?
    CHECKS=$((CHECKS + 1))
    if [ $rc -ne 0 ]; then
        fail "$(basename "$bin") $* exited $rc"
        return 1
    fi
    case $out in
    arm=*) ;;
    *)
        fail "$(basename "$bin") $* produced no arm= line: $out"
        return 1
        ;;
    esac
    printf '%s\n' "$out" | tee -a "$SMOKE_LOG"
}

log "=== P4 preparation smoke gate $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
log "kit=$KIT_DIR"
log "kernel=$(uname -r)"
log "log=$SMOKE_LOG"

# --- 0. environment -----------------------------------------------------------

check_eq "fstype /tmp" "$(findmnt -no FSTYPE /tmp)" "tmpfs"
check_eq "fstype /var/tmp" "$(findmnt -T /var/tmp -no FSTYPE)" "btrfs"

# --- 1. identity gate (plan §0 rule 2, §3.1 item 3) ---------------------------

log "--- identity gate ---"
check_eq "B1 archive sha256" "$(sha_of "$WT_B1/build/linux/x86_64/release/libsluice_core.a")" "$AR_B1_SHA"
check_eq "P2C1 archive sha256" "$(sha_of "$WT_C1/build/linux/x86_64/release/libsluice_core.a")" "$AR_C1_SHA"
check_eq "P2C2 archive sha256" "$(sha_of "$WT_C2/build/linux/x86_64/release/libsluice_core.a")" "$AR_C2_SHA"
check_eq "B1 worktree HEAD" "$(git -C "$WT_B1" rev-parse HEAD)" "$HEAD_B1"
check_eq "P2C1 worktree HEAD" "$(git -C "$WT_C1" rev-parse HEAD)" "$HEAD_C1"
check_eq "P2C2 worktree HEAD" "$(git -C "$WT_C2" rev-parse HEAD)" "$HEAD_C2"

check_eq "kit branch" "$(git -C "$KIT_REPO" branch --show-current)" "direct-cost-ladder"
KIT_HEAD=$(git -C "$KIT_REPO" rev-parse HEAD)
log "kit HEAD=$KIT_HEAD (measurement binaries descend from kit source $KIT_SOURCE_COMMIT, Amendment 1)"
check_eq "kit source ladder.cpp sha256 (2364be4a)" \
    "$(git -C "$KIT_REPO" show $KIT_SOURCE_COMMIT:research/direct-cost-ladder/ladder.cpp | sha256sum | cut -d' ' -f1)" \
    "$KIT_SOURCE_LADDER_SHA"

check_eq "ladder sha256" "$(sha_of "$KIT_DIR/ladder")" "$BIN_LADDER_SHA"
check_eq "ladder-c1 sha256" "$(sha_of "$KIT_DIR/ladder-c1")" "$BIN_C1_SHA"
check_eq "ladder-c2 sha256" "$(sha_of "$KIT_DIR/ladder-c2")" "$BIN_C2_SHA"
check_eq "ladder-4455d98d sha256 (P3-era diagnostic build)" "$(sha_of "$KIT_DIR/ladder-4455d98d")" "$LADDER_4455_SHA"
check_eq "remote ref origin/direct-cost-ladder absent" \
    "$(git -C "$KIT_REPO" branch -r --list origin/direct-cost-ladder | wc -l)" "0"

if [ "$FAILURES" -ne 0 ]; then
    log "identity gate FAILED — stop before any build or run (§0 rule 2)"
    log "SMOKE_FAIL"
    exit 1
fi

{
    echo "# P4 identity record — $(date -u +%Y-%m-%dT%H:%M:%SZ) host $(hostname) kernel $(uname -r) (P4 plan §3.1 item 3; regenerated by p4-scripts/p4-smoke.sh)"
    echo "kit branch: direct-cost-ladder (local-only; 0 remote refs)"
    echo "kit HEAD: $KIT_HEAD"
    echo "kit source commit (all three binaries): $KIT_SOURCE_COMMIT  ladder.cpp sha256 $KIT_SOURCE_LADDER_SHA"
    echo "B1  worktree $WT_B1  HEAD $(git -C "$WT_B1" rev-parse HEAD)  libsluice_core.a $AR_B1_SHA"
    echo "P2C1 worktree $WT_C1  HEAD $(git -C "$WT_C1" rev-parse HEAD)  libsluice_core.a $AR_C1_SHA"
    echo "P2C2 worktree $WT_C2  HEAD $(git -C "$WT_C2" rev-parse HEAD)  libsluice_core.a $AR_C2_SHA"
    echo "ladder   (B1-linked)  sha256 $BIN_LADDER_SHA"
    echo "ladder-c1 (P2C1)      sha256 $BIN_C1_SHA"
    echo "ladder-c2 (P2C2)      sha256 $BIN_C2_SHA"
    echo "warm fixture $READ_FIX sha256 $READ_FIX_SHA"
    echo "frozen parquet: bench $PQ_BENCH_SHA; trace $PQ_TRACE_SHA; trace-p2 $PQ_TRACE_P2_SHA; dataset $PQ_DATASET_SHA"
} > "$IDENTITY_TXT"
log "identity record written: $IDENTITY_TXT"

# --- 2. build gate (Amendment 1: one common kit source, byte-exact) -----------

log "--- build gate: reproduce each pinned binary from kit source $KIT_SOURCE_COMMIT ---"
BUILD_SCRATCH=$(mktemp -d /tmp/p4-kit-2364be4a.XXXXXX)
if ! git -C "$KIT_REPO" archive "$KIT_SOURCE_COMMIT" | tar -x -C "$BUILD_SCRATCH" 2>>"$SMOKE_LOG"; then
    fail "git archive $KIT_SOURCE_COMMIT extraction failed"
else
    SRC=$BUILD_SCRATCH/research/direct-cost-ladder
    rebuild_one() {
        local out=$1 expect=$2 wt=$3 label=$4
        local canary=$BUILD_SCRATCH/canary-$label
        if ! (cd "$SRC" && CCACHE_DISABLE=1 g++ -std=c++20 -O3 -DNDEBUG -fvisibility=hidden \
                -Wall -Werror -I"$wt/include" ladder.cpp \
                "$wt/build/linux/x86_64/release/libsluice_core.a" -o "$canary") >>"$SMOKE_LOG" 2>&1; then
            fail "canary build $label compile failed"
            return 1
        fi
        local got
        got=$(sha_of "$canary")
        if [ "$got" != "$expect" ]; then
            fail "canary build $label not byte-exact: $got (expected $expect)"
            return 1
        fi
        CHECKS=$((CHECKS + 1))
        log "ok: canary build $label byte-exact ($expect)"
        if [ "$(sha_of "$KIT_DIR/$out")" != "$expect" ]; then
            install -m 755 "$canary" "$KIT_DIR/$out"
            CHECKS=$((CHECKS + 1))
            log "installed missing/restored $out from canary"
        fi
    }
    rebuild_one ladder "$BIN_LADDER_SHA" "$WT_B1" b1
    rebuild_one ladder-c1 "$BIN_C1_SHA" "$WT_C1" c1
    rebuild_one ladder-c2 "$BIN_C2_SHA" "$WT_C2" c2
fi
rm -rf "$BUILD_SCRATCH"

# --- 3. fixtures ---------------------------------------------------------------

log "--- fixtures ---"
if [ ! -f "$READ_FIX" ]; then
    log "warm fixture missing on tmpfs (reboot; P0 §1.4) — regenerating"
    mkdir -p /tmp/sluice600
    "$KIT_DIR/mkfixture" "$READ_FIX" 67108864 >>"$SMOKE_LOG" 2>&1
fi
check_eq "warm fixture sha256" "$(sha_of "$READ_FIX")" "$READ_FIX_SHA"
for f in cold64m.bin coldwrite64m.bin; do
    if [ ! -f "$COLD_FIX_DIR/$f" ]; then
        log "cold fixture $f missing — regenerating (deterministic)"
        mkdir -p "$COLD_FIX_DIR"
        "$KIT_DIR/mkfixture" "$COLD_FIX_DIR/$f" 67108864 >>"$SMOKE_LOG" 2>&1
    fi
    check_eq "cold fixture $f present" "$([ -f "$COLD_FIX_DIR/$f" ] && echo yes)" "yes"
done

rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"

# --- 4. full arm inventory + verifiable groups (23 arms x 3 binaries) --------

log "--- arm inventory (23 arms @ kit source 2364be4a, x3 binaries) ---"
cat <<'EOF' | tee -a "$SMOKE_LOG"
a0    A0_raw_pread              read      Layer A floor (W01 context arm)
a1    A1_native_read_at         read      Layer B native mirror (N1)
a2    A2_sluice_read_at         read      Layer B B1 library (W01 cell arm)
a3    A3_nosyscall_stub         read      diagnostic stub (no cell)
a1x   A1x_native_read_exact_at  read      Layer B native composition (P2C1 control)
ax    AX_sluice_read_exact_at   read      Layer B B1 composition (P2C1 primary)
a1c   A1c_native_read_exact     read      Layer B native cursor twin
ac    AC_sluice_read_exact      read      Layer B B1 cursor twin (w01-comp-cursor-4k)
w0    W0_raw_pwrite             write     Layer A floor (context)
w1    W1_native_write_at        write     Layer B native mirror (N1)
w2    W2_sluice_write_at        write     Layer B B1 library (w01-write-4k)
w1x   W1x_native_write_all_at   write     Layer B native composition (control)
wx    WX_sluice_write_all_at    write     Layer B B1 composition (w01-wcomp-4k)
w1c   W1c_native_write_all      write     Layer B native cursor twin
wc    WC_sluice_write_all       write     Layer B B1 cursor twin
m0oc  M0_raw_open_close         metadata  Layer A floor (context)
m0fi  M0_raw_fstat              metadata  Layer A floor (context)
m1oc  M1_native_open_close      metadata  Layer B native mirror (context)
m1fi  M1_native_file_info       metadata  Layer B native mirror (context)
m1sz  M1_native_size            metadata  Layer B native mirror
m2oc  M2_sluice_open_close      metadata  Layer B B1 library (w01-meta-oc)
m2fi  M2_sluice_file_info       metadata  Layer B B1 library (w01-meta-fi)
m2sz  M2_sluice_size            metadata  Layer B B1 library
Layer C (W01): collapses onto Layer B — preregistered structural fact (plan §2).
Layer C kit probe arms (preadv av*, planner rpl*, copy cp*) exist only in the
P3 kit-source build (ladder-4455d98d) and enter no P4 cell; PARQUET Layer C
lives in the dududu strategy arms (plan §4.2/§4.4).
EOF

B_ALL=("$KIT_DIR/ladder" "$KIT_DIR/ladder-c1" "$KIT_DIR/ladder-c2")
NAME_ALL=(ladder ladder-c1 ladder-c2)
READ_GROUP="a0 a1 a2 a1x ax a1c ac"
WRITE_GROUP="w0 w1 w2 w1x wx w1c wc"
META_GROUP="m0oc m0fi m1oc m1fi m1sz m2oc m2fi m2sz"

# read_group <label> <args...>: every read-family arm on every binary; the
# checksum must be identical across arms within each binary, and each
# binary's group checksum must equal the first binary's.
read_group() {
    local label=$1; shift
    local first_ref=""
    for bi in "${!B_ALL[@]}"; do
        local bin=${B_ALL[$bi]} name=${NAME_ALL[$bi]}
        local binary_ref=""
        local arm line
        for arm in $READ_GROUP; do
            line=$(run_ladder "$bin" "$arm" "$@") || continue
            if [ -z "$binary_ref" ]; then
                binary_ref=$line
            else
                check_eq "group $label [$name] arm=$arm checksum" \
                    "$(field "$line" checksum)" "$(field "$binary_ref" checksum)"
            fi
        done
        [ -z "$binary_ref" ] && continue
        if [ -z "$first_ref" ]; then
            first_ref=$binary_ref
        else
            check_eq "group $label cross-binary [$name] checksum" \
                "$(field "$binary_ref" checksum)" "$(field "$first_ref" checksum)"
        fi
    done
}

log "--- read family group (seq 4k) ---"
read_group read-seq-4k 4096 256 64 1 "$READ_FIX" seq

log "--- read family group (rand 4k, w01-rand-4k shape) ---"
read_group read-rand-4k 4096 256 64 1 "$READ_FIX" rand 42

log "--- read family group (tail 4k, w01-comp-eofmix-4k shape) ---"
read_group read-tail-4k 4096 256 64 1 "$READ_FIX" tail

log "--- read family group (1 B, w01-hf-1b shape) ---"
read_group read-1b 1 1024 256 1 "$READ_FIX" seq

log "--- read family group (1 MiB seq, w01-seq-1m shape) ---"
read_group read-1m 1048576 16 8 1 "$READ_FIX" seq

log "--- a3 stub inventory (excluded from equality group) ---"
for bin in "${B_ALL[@]}"; do
    run_ladder "$bin" a3 4096 64 32 1 "$READ_FIX" seq >/dev/null
done

log "--- write family group (per-binary scratch, content_ok=1) ---"
write_first=""
for bi in "${!B_ALL[@]}"; do
    bin=${B_ALL[$bi]}; name=${NAME_ALL[$bi]}
    scratch=$SCRATCH/w-$name.bin
    "$KIT_DIR/mkfixture" "$scratch" 1048576 >>"$SMOKE_LOG" 2>&1
    binary_ref=""
    for arm in $WRITE_GROUP; do
        line=$(run_ladder "$bin" "$arm" 4096 32 16 1 "$scratch" seq) || continue
        check_eq "write [$name] arm=$arm content_ok" "$(field "$line" content_ok)" 1
        if [ -z "$binary_ref" ]; then
            binary_ref=$line
        else
            check_eq "write group [$name] arm=$arm checksum" "$(field "$line" checksum)" "$(field "$binary_ref" checksum)"
            check_eq "write group [$name] arm=$arm content_hash" "$(field "$line" content_hash)" "$(field "$binary_ref" content_hash)"
        fi
    done
    [ -z "$binary_ref" ] && continue
    if [ -z "$write_first" ]; then
        write_first=$binary_ref
    else
        check_eq "write group cross-binary [$name] checksum" "$(field "$binary_ref" checksum)" "$(field "$write_first" checksum)"
        check_eq "write group cross-binary [$name] content_hash" "$(field "$binary_ref" content_hash)" "$(field "$write_first" content_hash)"
    fi
done

log "--- metadata arms (inventory; cross-binary equality per arm) ---"
# Checksums are not comparable across metadata arm kinds (open+close returns
# 1/op, file_info a stat mix, size the byte count); the verifiable group is
# the same arm across the three binaries.
for arm in $META_GROUP; do
    arm_first=""
    for bi in "${!B_ALL[@]}"; do
        bin=${B_ALL[$bi]}; name=${NAME_ALL[$bi]}
        line=$(run_ladder "$bin" "$arm" 4096 8 4 1 "$READ_FIX" seq) || continue
        if [ -z "$arm_first" ]; then
            arm_first=$line
        else
            check_eq "meta arm=$arm cross-binary [$name] checksum" \
                "$(field "$line" checksum)" "$(field "$arm_first" checksum)"
        fi
    done
done

log "--- closed-state spot check (rejection path) ---"
for bin in "${B_ALL[@]}"; do
    run_ladder "$bin" a2 4096 64 32 1 "$READ_FIX" seq 0 closed >/dev/null
done

# --- 5. Layer C per workload shape --------------------------------------------

log "--- Layer C resolution (plan §2, §4.2) ---"
log "W01: Layer C collapses onto Layer B for all W01 cells (single-span-one-buffer shape; preregistered structural fact) — no kit Layer C cell arm."
log "PARQUET preadv competition arm (pq-strategy-gather-native): dududu-side strategy arm on a new local branch cut from r1-parquet-sluice-bench (plan §4.1) — measurement-phase work; the existing replay arm path is verified runnable below."
log "PARQUET copy_file_range (pq-cfr): NOT_APPLICABLE_SHAPE — no file-to-file transfer in a Parquet reader (plan §4.2 row)."

log "--- Layer C diagnostics via ladder-4455d98d (P3 kit-source build; enters no P4 cell) ---"
P3BIN=$KIT_DIR/ladder-4455d98d
"$KIT_DIR/mkfixture" "$SCRATCH/read4m.bin" 4194304 >>"$SMOKE_LOG" 2>&1

av_ref=""
for arm in av0 av1 avs; do
    line=$("$P3BIN" "$arm" 4096 64 32 2 "$SCRATCH/read4m.bin" seq iovcnt=4 2>>"$SMOKE_LOG") || { fail "diag $arm exited $?"; continue; }
    printf '%s\n' "$line" | tee -a "$SMOKE_LOG"
    if [ -z "$av_ref" ]; then
        av_ref=$line
        check_eq "diag av group reference content_ok" "$(field "$av_ref" content_ok)" 1
    else
        check_eq "diag av arm=$arm checksum" "$(field "$line" checksum)" "$(field "$av_ref" checksum)"
        check_eq "diag av arm=$arm content_hash" "$(field "$line" content_hash)" "$(field "$av_ref" content_hash)"
    fi
done

rpl0=$("$P3BIN" rpl0 4096 64 32 2 "$SCRATCH/read4m.bin" trace=g4k 2>>"$SMOKE_LOG") || fail "diag rpl0 exited $?"
if [ -n "$rpl0" ]; then
    printf '%s\n' "$rpl0" | tee -a "$SMOKE_LOG"
    for arm in rplt rpld; do
        extra=""
        [ "$arm" = rplt ] && extra="merge=8192"
        line=$("$P3BIN" "$arm" 4096 64 32 2 "$SCRATCH/read4m.bin" trace=g4k $extra 2>>"$SMOKE_LOG") || { fail "diag $arm exited $?"; continue; }
        printf '%s\n' "$line" | tee -a "$SMOKE_LOG"
        for f in req_digest ref_digest outcome_digest; do
            check_eq "diag $arm $f" "$(field "$line" "$f")" "$(field "$rpl0" "$f")"
        done
    done
fi

"$KIT_DIR/mkfixture" "$SCRATCH/cp2m.bin" 2097152 >>"$SMOKE_LOG" 2>&1
cp_ref=""
for arm in cp1 cp1s cp2 cp3; do
    line=$("$P3BIN" "$arm" 65536 32 8 2 "$SCRATCH/cp2m.bin" dst="$SCRATCH/cp-$arm.dst" 2>>"$SMOKE_LOG") || { fail "diag $arm exited $?"; continue; }
    printf '%s\n' "$line" | tee -a "$SMOKE_LOG"
    check_eq "diag cp arm=$arm content_ok" "$(field "$line" content_ok)" 1
    check_eq "diag cp arm=$arm dst==src" "$(field "$line" content_hash)" "$(field "$line" src_hash)"
    if [ -z "$cp_ref" ]; then
        cp_ref=$line
    else
        check_eq "diag cp arm=$arm content_hash" "$(field "$line" content_hash)" "$(field "$cp_ref" content_hash)"
        check_eq "diag cp arm=$arm checksum" "$(field "$line" checksum)" "$(field "$cp_ref" checksum)"
    fi
done

# --- 6. Parquet minimal smoke (read-only frozen inputs) -----------------------

log "--- Parquet arm runnability (one minimal smoke; frozen trace read-only) ---"
check_eq "parquet_bench sha256" "$(sha_of "$PQ_BENCH")" "$PQ_BENCH_SHA"
check_eq "frozen trace sha256" "$(sha_of "$DUDUDU/build/parquet-results/trace-medium-t3.jsonl")" "$PQ_TRACE_SHA"
check_eq "frozen trace-p2 sha256" "$(sha_of "$DUDUDU/build/parquet-results/trace-medium-t3-p2.jsonl")" "$PQ_TRACE_P2_SHA"
check_eq "frozen dataset sha256" "$(sha_of "$DUDUDU/build/parquet-data/medium-none.parquet")" "$PQ_DATASET_SHA"

PQ_SMOKE=$RAW_DIR/parquet-smoke-replay.txt
if (cd "$DUDUDU" && taskset -c 2 "$PQ_BENCH" \
        --medium=build/parquet-data/medium-none.parquet \
        --trace=build/parquet-results/trace-medium-t3.jsonl \
        --benchmark_filter='Replay/.*' --benchmark_repetitions=1) > "$PQ_SMOKE" 2>&1; then
    CHECKS=$((CHECKS + 1))
    log "ok: parquet replay smoke exited 0 (raw: $PQ_SMOKE)"
else
    fail "parquet replay smoke exited nonzero (raw: $PQ_SMOKE)"
fi
grep -E '^Replay/native/medium' "$PQ_SMOKE" | head -1 | tee -a "$SMOKE_LOG"
grep -E '^Replay/sluice/medium' "$PQ_SMOKE" | head -1 | tee -a "$SMOKE_LOG"
check_eq "replay native line present" "$(grep -cE '^Replay/native/medium' "$PQ_SMOKE")" "1"
check_eq "replay sluice line present" "$(grep -cE '^Replay/sluice/medium' "$PQ_SMOKE")" "1"
check_eq "replay native requests" "$(grep -E '^Replay/native/medium' "$PQ_SMOKE" | sed -n 's/.*replay_requests=\([0-9]*\).*/\1/p')" "57"
check_eq "replay sluice requests" "$(grep -E '^Replay/sluice/medium' "$PQ_SMOKE" | sed -n 's/.*replay_requests=\([0-9]*\).*/\1/p')" "57"
check_eq "replay native bytes" "$(grep -E '^Replay/native/medium' "$PQ_SMOKE" | sed -n 's/.*replay_bytes=\([0-9.]*M\).*/\1/p')" "181.996M"
check_eq "replay sluice bytes" "$(grep -E '^Replay/sluice/medium' "$PQ_SMOKE" | sed -n 's/.*replay_bytes=\([0-9.]*M\).*/\1/p')" "181.996M"
check_eq "dataset digest self-declared by tool" \
    "$(sed -n 's/^# dataset medium path=[^ ]* sha256=\([0-9a-f]*\).*/\1/p' "$PQ_SMOKE")" "$PQ_DATASET_SHA"

check_eq "frozen trace unchanged after smoke" "$(sha_of "$DUDUDU/build/parquet-results/trace-medium-t3.jsonl")" "$PQ_TRACE_SHA"
check_eq "frozen dataset unchanged after smoke" "$(sha_of "$DUDUDU/build/parquet-data/medium-none.parquet")" "$PQ_DATASET_SHA"
check_eq "dududu worktree clean" "$(git -C "$DUDUDU" status --porcelain | wc -l)" "0"

# --- 7. summary ----------------------------------------------------------------

rm -rf "$SCRATCH"
log "=== summary: $CHECKS checks, $FAILURES failures ==="
if [ "$FAILURES" -eq 0 ]; then
    log "SMOKE_PASS"
    exit 0
fi
log "SMOKE_FAIL"
exit 1
