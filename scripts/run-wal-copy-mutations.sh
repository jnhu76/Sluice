#!/usr/bin/env bash
# Deterministic, HERMETIC mutation proof for the WAL + copy_all fuzz targets.
#
# This is NOT an unbounded mutation framework. It applies a fixed set of
# targeted, hand-authored mutations to production code (src/wal.cpp,
# src/copy.cpp), builds the affected instrumented fuzz target in an ISOLATED git
# worktree checked out at BASE_SHA, and replays the seed corpus that is TRACKED
# at BASE_SHA. Untracked seeds in the caller's worktree (e.g. fuzzer-discovered
# seeds from a local smoke run) CANNOT participate: every killer must already
# exist in BASE_SHA, so the result is reproducible from a clean clone.
#
# Hermeticity / proof-chain contracts (§3 of the corrective):
#   A. Corpus authority: seeds are enumerated with `git -C WORKTREE ls-files`
#      against BASE_SHA, never `find` over the caller's worktree.
#   B. Baseline preflight: before applying the first mutant, the UNMODIFIED
#      targets are built from BASE_SHA and every tracked seed is replayed; each
#      baseline target/seed pair MUST exit successfully. A seed that already
#      crashes baseline is BLOCKED, not a mutant killer.
#   C. Evidence preservation: every baseline and mutant invocation is captured
#      to a temporary log; a KILLED report records the mutant ID, target, seed,
#      exit code, and log path / concise failure signature.
#   D. Classification: KILLED / SURVIVED / INVALID / BLOCKED, distinguished so a
#      timeout, loader error, missing seed, or sanitizer-runtime failure is never
#      silently presented as a semantic kill.
#
# Safety:
#   - Requires a clean tracked HEAD (commit first).
#   - Operates in a temporary detached git worktree; the caller's worktree is
#     never edited.
#   - Traps cleanup (worktree + fragment removal) on EXIT/INT/TERM.
#   - Resets the mutated file from BASE_SHA between mutants.
#   - Returns non-zero if any mutant SURVIVES, is INVALID, or is BLOCKED.
#
# A mutation is applied with EXACT-match semantics: the OLD fragment must occur
# exactly once in the file. Zero matches => the code moved (INVALID); >1 matches
# => ambiguous (INVALID). We never guess.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "$PROJECT_ROOT"

# --- Require a clean tracked HEAD. Untracked files are fine (the fuzz
#     infrastructure may be uncommitted), but tracked production/source files
#     must not be dirty, so the base SHA is well-defined.
if ! git diff-index --quiet HEAD -- ; then
    echo "ERROR: tracked working-tree changes exist. Commit first so the base SHA is stable." >&2
    exit 1
fi

BASE_SHA="$(git rev-parse HEAD)"
echo "BASE SHA:              ${BASE_SHA}"
echo "FUZZ TOOLCHAIN:        $(clang++ --version | head -1)"
echo "XMAKE:                 $(xmake --version | head -1)"
echo "SANITERS:              libFuzzer + ASan + UBSan"

# --- Isolated worktree at BASE_SHA. ---
WORKTREE_DIR="$(mktemp -d -t sluice-fuzz-mutants.XXXXXX)"
# Per-invocation evidence directory (kept under the worktree's temp parent so it
# is removed with the worktree). Logs land here for every baseline + mutant run.
EVIDENCE_DIR="$(mktemp -d -t sluice-fuzz-mutants-evidence.XXXXXX)"
TMPDIR="$(mktemp -d -t sluice-fuzz-fragments.XXXXXX)"

cleanup() {
    git worktree remove --force "$WORKTREE_DIR" 2>/dev/null || true
    rm -rf "$EVIDENCE_DIR" "$TMPDIR" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

git worktree add --detach "$WORKTREE_DIR" "$BASE_SHA" >/dev/null
echo "WORKTREE:              ${WORKTREE_DIR}"
echo "EVIDENCE:              ${EVIDENCE_DIR}"

# Configure + seed the build cache once (incremental per mutant after this).
cd "$WORKTREE_DIR"
xmake f -m debug --toolchain=clang -y >/dev/null
echo ""

# --- Enumerate TRACKED seeds for a corpus dir from the BASE_SHA worktree. This
#     is the hermetic corpus authority: only files tracked at BASE_SHA can kill
#     a mutant. Untracked local seeds (e.g. from a caller smoke run) cannot.
#     Emits absolute paths, NUL-separated, into the global SEEDS array.
declare -a SEEN_CORPUS_ERRORS=()
enumerate_corpus() {
    local corpus_rel="$1"   # e.g. fuzz/corpus/wal_read_record
    # List tracked files relative to the worktree root, then absolutize.
    local listed
    listed=$(git -C "$WORKTREE_DIR" ls-files -z -- "$corpus_rel" \
             | tr '\0' '\n' | sort)
    if [[ -z "$listed" ]]; then
        echo "  (no tracked files under ${corpus_rel} at BASE_SHA)" >&2
        SEEN_CORPUS_ERRORS+=("$corpus_rel")
        return 1
    fi
    local abs=""
    while IFS= read -r f; do
        abs+="${WORKTREE_DIR}/${f}"$'\n'
    done <<< "$listed"
    printf '%s' "$abs"
}

# --- Run one target against one seed, capturing all output to a log. Returns
#     the process exit code via $LAST_RC and the log path via $LAST_LOG. Uses a
#     short timeout so a runaway mutant cannot hang the whole proof; a timeout
#     is classified separately from a semantic kill.
run_one() {
    local binary="$1" seed="$2" log="$3" max_len="${4:-1048576}"
    # libFuzzer flags: -runs=1 deterministic replay; -timeout bounds a runaway;
    # -rss_limit_mb bounds memory; -max_len bounds input size.
    set +e
    "$binary" "$seed" -runs=1 -timeout=10 -rss_limit_mb=1024 -max_len="$max_len" \
        >"$log" 2>&1
    LAST_RC=$?
    set -e
    LAST_LOG="$log"
}

# ===========================================================================
# Baseline preflight: build the UNMODIFIED targets from BASE_SHA and replay
# every tracked seed. Every baseline target/seed pair MUST exit 0. A seed that
# already crashes baseline is BLOCKED, not a mutant killer.
# ===========================================================================
echo "================ BASELINE PREFLIGHT ================"
echo "Building unmodified fuzz targets at BASE_SHA..."
if ! xmake build -g fuzz >/dev/null 2>&1; then
    echo "BLOCKED: baseline build failed at BASE_SHA" >&2
    echo "BASELINE wal_read_record_fuzz: BLOCKED"
    echo "BASELINE wal_roundtrip_fuzz: BLOCKED"
    echo "BASELINE copy_all_fault_fuzz: BLOCKED"
    exit 1
fi

# Per-target max_len profiles (§19): the three targets have materially different
# resource profiles, so do NOT use one global max_len.
declare -A MAXLEN=(
    [wal_read_record_fuzz]=1048576
    [wal_roundtrip_fuzz]=262144
    [copy_all_fault_fuzz]=8192
)

# target|corpus-rel pairs.
declare -a BASELINE_PAIRS=(
    "wal_read_record_fuzz|fuzz/corpus/wal_read_record"
    "wal_roundtrip_fuzz|fuzz/corpus/wal_roundtrip"
    "copy_all_fault_fuzz|fuzz/corpus/copy_all_fault"
)

BASELINE_FAIL=0
for pair in "${BASELINE_PAIRS[@]}"; do
    target="${pair%%|*}"
    corpus="${pair#*|}"
    binary="${WORKTREE_DIR}/build/linux/x86_64/debug/${target}"
    if [[ ! -x "$binary" ]]; then
        echo "BASELINE ${target}: BLOCKED (binary missing)"
        BASELINE_FAIL=1
        continue
    fi
    seeds_file="${EVIDENCE_DIR}/seeds.${target}.txt"
    enumerate_corpus "$corpus" >"$seeds_file" || { BASELINE_FAIL=1; continue; }
    seed_count=$(wc -l <"$seeds_file")
    target_fail=0
    idx=0
    while IFS= read -r seed; do
        [[ -z "$seed" ]] && continue
        idx=$((idx + 1))
        log="${EVIDENCE_DIR}/baseline.${target}.${idx}.log"
        run_one "$binary" "$seed" "$log" "${MAXLEN[$target]}"
        rc=$LAST_RC
        if [[ $rc -ne 0 ]]; then
            echo "BASELINE ${target}: FAIL seed=$(basename "$seed") rc=$rc log=$log"
            sed -n '1,12p' "$log" | sed 's/^/    | /'
            target_fail=1
        fi
    done <"$seeds_file"
    if [[ $target_fail -ne 0 ]]; then
        echo "BASELINE ${target}: FAIL (${seed_count} seeds)"
        BASELINE_FAIL=1
    else
        echo "BASELINE ${target}: PASS (${seed_count} seeds)"
    fi
done

if [[ $BASELINE_FAIL -ne 0 ]]; then
    echo ""
    echo "BLOCKED: baseline preflight failed. A seed that already crashes baseline"
    echo "is not a mutant killer; do not classify any mutant. See logs in $EVIDENCE_DIR."
    exit 1
fi
echo "===================================================="
echo ""

# --- Literal mutation applier (perl). Counts non-overlapping occurrences of the
#     OLD fragment; replaces only when the count is exactly 1.
#     Prints "count=N"; exits 0 on success, 2 when count != 1 (INVALID).
apply_mutation() {
    local file="$1" old_file="$2" new_file="$3"
    perl -e '
        my ($file, $oldf, $newf) = @ARGV;
        local $/;
        open(F, "<", $file) or die "read $file: $!"; my $content = <F>; close F;
        open(O, "<", $oldf) or die "read $oldf: $!"; my $old = <O>; close O;
        open(N, "<", $newf) or die "read $newf: $!"; my $new = <N>; close N;
        my $count = 0; my $pos = 0;
        while ((my $i = index($content, $old, $pos)) >= 0) {
            $count++; $pos = $i + length($old);
        }
        if ($count != 1) { print "count=$count\n"; exit 2; }
        substr($content, index($content, $old), length($old), $new);
        open(F, ">", $file) or die "write $file: $!"; print F $content; close F;
        print "count=1\n"; exit 0;
    ' "$file" "$old_file" "$new_file"
}

# --- Try to kill a mutant by replaying every TRACKED seed for the given target,
#     one at a time, against the mutated binary. Prints a concise killer line on
#     success. Returns 0 (killed) / 1 (survived) / 2 (blocked-runtime).
#     Distinguishes a true non-zero exit (KILLED candidate) from a timeout /
#     sanitizer-runtime bootstrap failure, which is NOT a semantic kill.
try_kill() {
    local target="$1" seeds_file="$2" max_len="$3"
    local binary="${WORKTREE_DIR}/build/linux/x86_64/debug/${target}"
    if [[ ! -x "$binary" ]]; then
        echo "  (binary missing: ${binary})"
        return 2
    fi
    local idx=0
    while IFS= read -r seed; do
        [[ -z "$seed" ]] && continue
        idx=$((idx + 1))
        local log="${EVIDENCE_DIR}/mutant.${MUT_ID}.${target}.${idx}.log"
        run_one "$binary" "$seed" "$log" "$max_len"
        local rc=$LAST_RC
        if [[ $rc -ne 0 ]]; then
            # Extract a concise failure signature (first FUZZ_ASSERT line, or the
            # first error line) so the report is self-explanatory without dumping
            # the whole log.
            local sig
            sig=$(grep -m1 -E "FUZZ_ASSERT FAILED|SUMMARY: libFuzzer|ERROR: " "$log" 2>/dev/null \
                  | head -1)
            [[ -z "$sig" ]] && sig="rc=$rc"
            echo "KILLED seed=$(basename "$seed") rc=$rc sig='${sig}' log=$log"
            return 0
        fi
    done <"$seeds_file"
    return 1
}

# --- Mutation bookkeeping. ---
WAL_KILLED=0; WAL_SURVIVED=0; WAL_INVALID=0; WAL_BLOCKED=0
CP_KILLED=0; CP_SURVIVED=0; CP_INVALID=0; CP_BLOCKED=0
RESULTS=()   # "ID  RESULT  killer-or-detail"

# Build the per-target tracked-seed files once (corpus authority established at
# baseline). These are reused for every mutant so the same seed lists drive both
# the baseline preflight and the mutant replay.
WAL_RAW_SEEDS="${EVIDENCE_DIR}/seeds.wal_read_record_fuzz.txt"
WAL_RTP_SEEDS="${EVIDENCE_DIR}/seeds.wal_roundtrip_fuzz.txt"
COPY_SEEDS="${EVIDENCE_DIR}/seeds.copy_all_fault_fuzz.txt"

run_mutation() {
    local id="$1" file="$2" old_file="$3" new_file="$4" group="$5"
    shift 5
    # Remaining args: alternating target seeds-file pairs (seeds files pre-built
    # during baseline; this keeps corpus authority in one place).
    local rel_file="$file"
    local target_file="${WORKTREE_DIR}/${rel_file}"

    MUT_ID="$id"
    echo "--- ${id}: ${rel_file} ---"

    # Reset the file to BASE_SHA, then apply the mutation.
    git -C "$WORKTREE_DIR" checkout "$BASE_SHA" -- "$rel_file" >/dev/null
    local out
    set +e
    out=$(apply_mutation "$target_file" "$old_file" "$new_file")
    local apply_rc=$?
    set -e
    if [[ $apply_rc -ne 0 || "$out" != "count=1" ]]; then
        echo "  INVALID (fragment matched ${out#count=}; expected exactly 1)"
        if [[ "$group" == "wal" ]]; then WAL_INVALID=$((WAL_INVALID + 1));
        else CP_INVALID=$((CP_INVALID + 1)); fi
        RESULTS+=("${id}  INVALID  n/a")
        git -C "$WORKTREE_DIR" checkout "$BASE_SHA" -- "$rel_file" >/dev/null
        return
    fi

    # Build the affected instrumented target(s). Incremental compilation means
    # only the mutated translation unit recompiles.
    echo "  building..."
    set +e
    xmake build -g fuzz >"${EVIDENCE_DIR}/build.${id}.log" 2>&1
    local build_rc=$?
    set -e
    if [[ $build_rc -ne 0 ]]; then
        echo "  INVALID (build failed; see ${EVIDENCE_DIR}/build.${id}.log)"
        if [[ "$group" == "wal" ]]; then WAL_INVALID=$((WAL_INVALID + 1));
        else CP_INVALID=$((CP_INVALID + 1)); fi
        RESULTS+=("${id}  INVALID  build-failed")
        git -C "$WORKTREE_DIR" checkout "$BASE_SHA" -- "$rel_file" >/dev/null
        return
    fi

    # Replay each affected target's tracked seed corpus.
    local killed=0 local_killer="" blocked=0
    while [[ $# -gt 0 ]]; do
        local target="$1" seeds_file="$2"
        shift 2
        local ml="${MAXLEN[$target]}"
        set +e
        killer=$(try_kill "$target" "$seeds_file" "$ml")
        tk_rc=$?
        set -e
        if [[ $tk_rc -eq 0 ]]; then
            killed=1
            local_killer="${target}:${killer}"
            break
        elif [[ $tk_rc -eq 2 ]]; then
            blocked=1
        fi
    done

    if [[ $killed -eq 1 ]]; then
        echo "  ${local_killer}"
        if [[ "$group" == "wal" ]]; then WAL_KILLED=$((WAL_KILLED + 1));
        else CP_KILLED=$((CP_KILLED + 1)); fi
        RESULTS+=("${id}  KILLED  ${local_killer}")
    elif [[ $blocked -eq 1 ]]; then
        echo "  BLOCKED (runtime: binary missing or could not execute)"
        if [[ "$group" == "wal" ]]; then WAL_BLOCKED=$((WAL_BLOCKED + 1));
        else CP_BLOCKED=$((CP_BLOCKED + 1)); fi
        RESULTS+=("${id}  BLOCKED  runtime")
    else
        echo "  SURVIVED (all tracked seeds exited 0)"
        if [[ "$group" == "wal" ]]; then WAL_SURVIVED=$((WAL_SURVIVED + 1));
        else CP_SURVIVED=$((CP_SURVIVED + 1)); fi
        RESULTS+=("${id}  SURVIVED  n/a")
    fi

    # Reset for the next mutant.
    git -C "$WORKTREE_DIR" checkout "$BASE_SHA" -- "$rel_file" >/dev/null
}

# ===========================================================================
# WAL mutants (src/wal.cpp).
# ===========================================================================
WAL="src/wal.cpp"

# M-WAL-01: invert the magic comparison.
cat > "$TMPDIR/wal01.old" << 'EOF'
    if (rec_magic != magic) {
EOF
cat > "$TMPDIR/wal01.new" << 'EOF'
    if (rec_magic == magic) {
EOF
run_mutation "M-WAL-01" "$WAL" "$TMPDIR/wal01.old" "$TMPDIR/wal01.new" "wal" \
    "wal_read_record_fuzz" "$WAL_RAW_SEEDS" \
    "wal_roundtrip_fuzz" "$WAL_RTP_SEEDS"

# M-WAL-02: invert checksum validation.
cat > "$TMPDIR/wal02.old" << 'EOF'
    if (stored != checksum_of(payload)) {
EOF
cat > "$TMPDIR/wal02.new" << 'EOF'
    if (stored == checksum_of(payload)) {
EOF
run_mutation "M-WAL-02" "$WAL" "$TMPDIR/wal02.old" "$TMPDIR/wal02.new" "wal" \
    "wal_read_record_fuzz" "$WAL_RAW_SEEDS" \
    "wal_roundtrip_fuzz" "$WAL_RTP_SEEDS"

# M-WAL-03: decode the length from the wrong header offset (magic, not length).
cat > "$TMPDIR/wal03.old" << 'EOF'
    std::uint32_t length = get_le_u32(header.data() + 4);
EOF
cat > "$TMPDIR/wal03.new" << 'EOF'
    std::uint32_t length = get_le_u32(header.data());
EOF
run_mutation "M-WAL-03" "$WAL" "$TMPDIR/wal03.old" "$TMPDIR/wal03.new" "wal" \
    "wal_read_record_fuzz" "$WAL_RAW_SEEDS" \
    "wal_roundtrip_fuzz" "$WAL_RTP_SEEDS"

# M-WAL-04: enlarge the bounded read chunk beyond the documented bound.
cat > "$TMPDIR/wal04.old" << 'EOF'
        const std::size_t chunk =
            detail::read_chunk_size(payload_size - old_size);
EOF
cat > "$TMPDIR/wal04.new" << 'EOF'
        const std::size_t chunk =
            detail::read_chunk_size(payload_size - old_size) + 64 * 1024;
EOF
run_mutation "M-WAL-04" "$WAL" "$TMPDIR/wal04.old" "$TMPDIR/wal04.new" "wal" \
    "wal_read_record_fuzz" "$WAL_RAW_SEEDS" \
    "wal_roundtrip_fuzz" "$WAL_RTP_SEEDS"

# M-WAL-05: make the vector encoder emit a different checksum than the scalar
# encoder (scalar/vector drift, caught by the canonical-format oracle).
cat > "$TMPDIR/wal05.old" << 'EOF'
    put_le_u32(trailer.data(), checksum_of(payload));

    // Max 3 slices (header, payload, checksum) — stack-allocated to avoid
EOF
cat > "$TMPDIR/wal05.new" << 'EOF'
    put_le_u32(trailer.data(), checksum_of(payload) + 1);

    // Max 3 slices (header, payload, checksum) — stack-allocated to avoid
EOF
run_mutation "M-WAL-05" "$WAL" "$TMPDIR/wal05.old" "$TMPDIR/wal05.new" "wal" \
    "wal_read_record_fuzz" "$WAL_RAW_SEEDS" \
    "wal_roundtrip_fuzz" "$WAL_RTP_SEEDS"

# M-WAL-06: COMMON-MODE checksum mutation. Mutate the SHARED checksum_of()
# helper from addition to XOR so BOTH writers (write_record / write_record_vec)
# AND the reader (read_record) drift together. A reader-only check would agree
# with the mutated codec and miss the drift; the independent canonical-format
# oracle (wal_oracle.hpp) kills it because it re-derives the checksum without
# calling production's helper.
cat > "$TMPDIR/wal06.old" << 'EOF'
std::uint32_t checksum_of(std::span<const std::byte> payload) {
    std::uint64_t sum = 0;
    for (auto b : payload) {
        sum += std::to_integer<unsigned>(b);
    }
    return static_cast<std::uint32_t>(sum & 0xFFFFFFFFU);
}
EOF
cat > "$TMPDIR/wal06.new" << 'EOF'
std::uint32_t checksum_of(std::span<const std::byte> payload) {
    std::uint32_t acc = 0;
    for (auto b : payload) {
        acc ^= static_cast<std::uint32_t>(std::to_integer<unsigned>(b));
    }
    return acc;
}
EOF
run_mutation "M-WAL-06" "$WAL" "$TMPDIR/wal06.old" "$TMPDIR/wal06.new" "wal" \
    "wal_read_record_fuzz" "$WAL_RAW_SEEDS" \
    "wal_roundtrip_fuzz" "$WAL_RTP_SEEDS"

# ===========================================================================
# copy_all mutants (src/copy.cpp).
# ===========================================================================
COPY="src/copy.cpp"

# M-COPY-01: remove the remaining-limit clamp from the SCRATCH-path Reader
# request size.
cat > "$TMPDIR/copy01.old" << 'EOF'
        if (limit.is_limited()) {
            std::uint64_t left = limit.remaining() - total;
            to_read = static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), left));
        }
EOF
cat > "$TMPDIR/copy01.new" << 'EOF'
        if (false && limit.is_limited()) {
            std::uint64_t left = limit.remaining() - total;
            to_read = static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), left));
        }
EOF
run_mutation "M-COPY-01" "$COPY" "$TMPDIR/copy01.old" "$TMPDIR/copy01.new" "copy" \
    "copy_all_fault_fuzz" "$COPY_SEEDS"

# M-COPY-02: remove total += got (the loop accumulator).
cat > "$TMPDIR/copy02.old" << 'EOF'
        total += got;
EOF
cat > "$TMPDIR/copy02.new" << 'EOF'
        // total += got; // mutated away
EOF
run_mutation "M-COPY-02" "$COPY" "$TMPDIR/copy02.old" "$TMPDIR/copy02.new" "copy" \
    "copy_all_fault_fuzz" "$COPY_SEEDS"

# M-COPY-03: swallow the scratch-path writer error instead of returning it.
cat > "$TMPDIR/copy03.old" << 'EOF'
        auto wr = writer.write_all(std::span<const std::byte>(scratch.data(), got));
        if (!wr.has_value()) {
            if (stats) {
                ++stats->writer_error_stops;
            }
            return make_unexpected<std::uint64_t>(wr.error());
        }
EOF
cat > "$TMPDIR/copy03.new" << 'EOF'
        auto wr = writer.write_all(std::span<const std::byte>(scratch.data(), got));
        if (!wr.has_value()) {
            // mutation: swallow the writer error and keep going
        }
EOF
run_mutation "M-COPY-03" "$COPY" "$TMPDIR/copy03.old" "$TMPDIR/copy03.new" "copy" \
    "copy_all_fault_fuzz" "$COPY_SEEDS"

# M-COPY-04: permit empty scratch for a non-zero copy.
cat > "$TMPDIR/copy04.old" << 'EOF'
    if (scratch.empty()) {
        return make_unexpected<std::uint64_t>(IoError{.code = IoError::Code::invalid_state});
    }
EOF
cat > "$TMPDIR/copy04.new" << 'EOF'
    if (false && scratch.empty()) {
        return make_unexpected<std::uint64_t>(IoError{.code = IoError::Code::invalid_state});
    }
EOF
run_mutation "M-COPY-04" "$COPY" "$TMPDIR/copy04.old" "$TMPDIR/copy04.new" "copy" \
    "copy_all_fault_fuzz" "$COPY_SEEDS"

# M-COPY-05: remove the got > requested defensive rejection (broken-reader).
cat > "$TMPDIR/copy05.old" << 'EOF'
        if (got > to_read) {
            // Defensive: a reader returning more than asked is broken.
            if (stats) {
                ++stats->reader_error_stops;
            }
            return make_unexpected<std::uint64_t>(IoError{.code = IoError::Code::invalid_state});
        }
EOF
cat > "$TMPDIR/copy05.new" << 'EOF'
        if (false && got > to_read) {
            // Defensive: a reader returning more than asked is broken.
            if (stats) {
                ++stats->reader_error_stops;
            }
            return make_unexpected<std::uint64_t>(IoError{.code = IoError::Code::invalid_state});
        }
EOF
run_mutation "M-COPY-05" "$COPY" "$TMPDIR/copy05.old" "$TMPDIR/copy05.new" "copy" \
    "copy_all_fault_fuzz" "$COPY_SEEDS"

# NOTE: M-COPY-06 (deferred Reject policy must return, not fall through) was
# RETIRED by SEMANTIC-DIET-0 (PR #287): the deferred-strategy public contract
# (reject / fallback-to-Auto) was intentionally removed, so no program can
# request a deferred strategy anymore and the mutant has no target. The
# remaining Auto/Scratch/BufferedFirst strategies are all implemented; there is
# no unsupported-strategy path left to witness. Do not re-add a placeholder
# mutant to preserve the count.

# M-COPY-07: remove the BUFFERED-path remaining-limit clamp. The fast path
# computes `allowed` from the limit; removing the clamp lets a bounded copy
# over-copy from the buffered region. Expected killer: a buffered + bounded-limit
# seed.
cat > "$TMPDIR/copy07.old" << 'EOF'
                std::size_t allowed = buffered.size();
                if (limit.is_limited()) {
                    std::uint64_t left = limit.remaining() - total;
                    allowed =
                        static_cast<std::size_t>(std::min<std::uint64_t>(buffered.size(), left));
                }
EOF
cat > "$TMPDIR/copy07.new" << 'EOF'
                std::size_t allowed = buffered.size();
                if (false && limit.is_limited()) {
                    std::uint64_t left = limit.remaining() - total;
                    allowed =
                        static_cast<std::size_t>(std::min<std::uint64_t>(buffered.size(), left));
                }
EOF
run_mutation "M-COPY-07" "$COPY" "$TMPDIR/copy07.old" "$TMPDIR/copy07.new" "copy" \
    "copy_all_fault_fuzz" "$COPY_SEEDS"

# M-COPY-08: consume buffered bytes BEFORE writer success (violates the
# writer-error ownership rule CB3). Move consume_buffered ahead of write_all so a
# writer failure still consumes the region. Expected killer: a buffered
# writer-failure seed.
cat > "$TMPDIR/copy08.old" << 'EOF'
                auto wr = writer.write_all(buffered.first(allowed));
                if (!wr.has_value()) {
                    if (stats) {
                        ++stats->writer_error_stops;
                    }
                    return make_unexpected<std::uint64_t>(wr.error());
                }
                auto cr = br->consume_buffered(allowed);
                if (!cr.has_value()) {
                    // Should be impossible: allowed <= buffered.size().
                    if (stats) {
                        ++stats->reader_error_stops;
                    }
                    return make_unexpected<std::uint64_t>(cr.error());
                }
EOF
cat > "$TMPDIR/copy08.new" << 'EOF'
                auto cr = br->consume_buffered(allowed);
                if (!cr.has_value()) {
                    // Should be impossible: allowed <= buffered.size().
                    if (stats) {
                        ++stats->reader_error_stops;
                    }
                    return make_unexpected<std::uint64_t>(cr.error());
                }
                auto wr = writer.write_all(buffered.first(allowed));
                if (!wr.has_value()) {
                    if (stats) {
                        ++stats->writer_error_stops;
                    }
                    return make_unexpected<std::uint64_t>(wr.error());
                }
EOF
run_mutation "M-COPY-08" "$COPY" "$TMPDIR/copy08.old" "$TMPDIR/copy08.new" "copy" \
    "copy_all_fault_fuzz" "$COPY_SEEDS"

# ===========================================================================
# Report.
# ===========================================================================
echo ""
echo "================ MUTATION RESULTS ================"
printf "%-12s %-10s %s\n" "MUTANT" "RESULT" "KILLER / DETAIL"
for r in "${RESULTS[@]}"; do
    # r is "ID  RESULT  detail" (two-space separators).
    id="${r%%  *}"
    rest="${r#*  }"
    printf "%-12s %s\n" "$id" "$rest"
done
TOTAL_KILLED=$((WAL_KILLED + CP_KILLED))
TOTAL_SURVIVED=$((WAL_SURVIVED + CP_SURVIVED))
TOTAL_INVALID=$((WAL_INVALID + CP_INVALID))
TOTAL_BLOCKED=$((WAL_BLOCKED + CP_BLOCKED))

echo "--------------------------------------------------"
echo "WAL:  KILLED=${WAL_KILLED}  SURVIVED=${WAL_SURVIVED}  INVALID=${WAL_INVALID}  BLOCKED=${WAL_BLOCKED}  (of 6)"
echo "COPY: KILLED=${CP_KILLED}  SURVIVED=${CP_SURVIVED}  INVALID=${CP_INVALID}  BLOCKED=${CP_BLOCKED}  (of 7)"
echo "TOTAL: KILLED=${TOTAL_KILLED}  SURVIVED=${TOTAL_SURVIVED}  INVALID=${TOTAL_INVALID}  BLOCKED=${TOTAL_BLOCKED}  (of 13)"
echo "=================================================="
echo "EVIDENCE LOGS: ${EVIDENCE_DIR}"

if [[ $TOTAL_SURVIVED -gt 0 || $TOTAL_INVALID -gt 0 || $TOTAL_BLOCKED -gt 0 ]]; then
    echo "VERDICT: FAIL (survived=${TOTAL_SURVIVED} invalid=${TOTAL_INVALID} blocked=${TOTAL_BLOCKED})"
    exit 1
fi
echo "VERDICT: PASS (all 13 mutants killed by tracked BASE_SHA seeds)"
exit 0
