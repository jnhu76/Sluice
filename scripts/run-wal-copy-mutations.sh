#!/usr/bin/env bash
# Deterministic mutation proof for the WAL + copy_all fuzz targets.
#
# This is NOT an unbounded mutation framework. It applies a fixed set of
# targeted, hand-authored mutations to production code (src/wal.cpp,
# src/copy.cpp), builds the affected instrumented fuzz target in an isolated
# git worktree, and replays the committed seed corpus deterministically. A
# mutant is KILLED if any seed makes the target crash (oracle assertion /
# sanitizer trip). It SURVIVES if no seed crashes it, and INVALID if the
# intended source fragment does not match exactly once.
#
# Safety:
#   - Requires a clean tracked HEAD (commit first).
#   - Operates in a temporary detached git worktree; the caller's worktree is
#     never edited.
#   - Traps cleanup (worktree removal) on EXIT/INT/TERM.
#   - Resets the mutated file from the base commit between mutants.
#   - Returns non-zero if any mutant SURVIVES or is INVALID.
#
# A mutation is applied with EXACT-match semantics: the OLD fragment must
# occur exactly once in the file. Zero matches => the code moved (INVALID);
# >1 matches => ambiguous (INVALID). We never guess.
set -uo pipefail

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

# --- Isolated worktree. ---
WORKTREE_DIR="$(mktemp -d -t sluice-fuzz-mutants.XXXXXX)"
cleanup() {
    git worktree remove --force "$WORKTREE_DIR" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
git worktree add --detach "$WORKTREE_DIR" "$BASE_SHA" >/dev/null
echo "WORKTREE:              ${WORKTREE_DIR}"

# Configure + seed the build cache once (incremental per mutant after this).
cd "$WORKTREE_DIR"
xmake f -m debug --toolchain=clang -y >/dev/null
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

# --- Try to kill a mutant by replaying every seed in a corpus, one at a time.
#     Returns 0 (killed) as soon as a seed crashes the target, printing the
#     killing seed; returns 1 (survived) if none do.
try_kill() {
    local target="$1" corpus_dir="$2"
    local binary="${WORKTREE_DIR}/build/linux/x86_64/debug/${target}"
    if [[ ! -x "$binary" ]]; then
        echo "  (binary missing: ${binary})"
        return 1
    fi
    local seeds=()
    while IFS= read -r -d '' s; do seeds+=("$s"); done < <(find "$corpus_dir" -type f -print0 | sort -z)
    for seed in "${seeds[@]}"; do
        set +e
        "$binary" "$seed" -runs=1 -timeout=2 -rss_limit_mb=1024 >/dev/null 2>&1
        local rc=$?
        set -e
        if [[ $rc -ne 0 ]]; then
            echo "KILLED $(basename "$seed")"
            return 0
        fi
    done
    return 1
}

# --- Mutation bookkeeping. ---
WAL_KILLED=0; WAL_SURVIVED=0; WAL_INVALID=0
CP_KILLED=0; CP_SURVIVED=0; CP_INVALID=0
RESULTS=()   # "ID  RESULT  killer"

run_mutation() {
    local id="$1" file="$2" old_file="$3" new_file="$4" group="$5"
    shift 5
    # Remaining args: alternating target corpus-dir pairs.
    local rel_file="$file"
    local target_file="${WORKTREE_DIR}/${rel_file}"

    echo "--- ${id}: ${rel_file} ---"

    # Reset the file to base, then apply the mutation.
    git -C "$WORKTREE_DIR" checkout "$BASE_SHA" -- "$rel_file" >/dev/null
    local out
    out=$(apply_mutation "$target_file" "$old_file" "$new_file") || true
    if [[ "$out" != "count=1" ]]; then
        echo "  INVALID (fragment matched ${out#count=}; expected exactly 1)"
        if [[ "$group" == "wal" ]]; then WAL_INVALID=$((WAL_INVALID + 1));
        else CP_INVALID=$((CP_INVALID + 1)); fi
        RESULTS+=("${id}  INVALID  n/a")
        # restore clean state
        git -C "$WORKTREE_DIR" checkout "$BASE_SHA" -- "$rel_file" >/dev/null
        return
    fi

    # Build the affected instrumented target(s). Building the whole fuzz group
    # keeps the rule simple; incremental compilation means only the mutated
    # translation unit recompiles.
    echo "  building..."
    if ! xmake build -g fuzz >/dev/null 2>&1; then
        echo "  INVALID (build failed)"
        if [[ "$group" == "wal" ]]; then WAL_INVALID=$((WAL_INVALID + 1));
        else CP_INVALID=$((CP_INVALID + 1)); fi
        RESULTS+=("${id}  INVALID  build-failed")
        git -C "$WORKTREE_DIR" checkout "$BASE_SHA" -- "$rel_file" >/dev/null
        return
    fi

    # Replay each affected corpus.
    local killed=0 local_killer=""
    while [[ $# -gt 0 ]]; do
        local target="$1" corpus_dir="$2"
        shift 2
        local cdir="${PROJECT_ROOT}/${corpus_dir}"
        if [[ ! -d "$cdir" ]]; then
            echo "  (corpus missing: ${cdir})"
            continue
        fi
        local killer
        killer=$(try_kill "$target" "$cdir")
        if [[ -n "$killer" ]]; then
            killed=1
            local_killer="${target}:${killer}"
            break
        fi
    done

    if [[ $killed -eq 1 ]]; then
        echo "  ${local_killer}"
        if [[ "$group" == "wal" ]]; then WAL_KILLED=$((WAL_KILLED + 1));
        else CP_KILLED=$((CP_KILLED + 1)); fi
        RESULTS+=("${id}  KILLED  ${local_killer}")
    else
        echo "  SURVIVED"
        if [[ "$group" == "wal" ]]; then WAL_SURVIVED=$((WAL_SURVIVED + 1));
        else CP_SURVIVED=$((CP_SURVIVED + 1)); fi
        RESULTS+=("${id}  SURVIVED  n/a")
    fi

    # Reset for the next mutant.
    git -C "$WORKTREE_DIR" checkout "$BASE_SHA" -- "$rel_file" >/dev/null
}

# --- Temp dir for OLD/NEW fragment files. ---
TMPDIR="$(mktemp -d -t sluice-fuzz-fragments.XXXXXX)"
trap 'cleanup; rm -rf "$TMPDIR"' EXIT INT TERM

# ===========================================================================
# WAL mutants (src/wal.cpp).
# ===========================================================================
WAL="src/wal.cpp"
WAL_TARGETS="wal_read_record_fuzz fuzz/corpus/wal_read_record wal_roundtrip_fuzz fuzz/corpus/wal_roundtrip"

# M-WAL-01: invert the magic comparison.
cat > "$TMPDIR/wal01.old" << 'EOF'
    if (rec_magic != magic) {
EOF
cat > "$TMPDIR/wal01.new" << 'EOF'
    if (rec_magic == magic) {
EOF
run_mutation "M-WAL-01" "$WAL" "$TMPDIR/wal01.old" "$TMPDIR/wal01.new" "wal" $WAL_TARGETS

# M-WAL-02: invert checksum validation.
cat > "$TMPDIR/wal02.old" << 'EOF'
    if (stored != checksum_of(payload)) {
EOF
cat > "$TMPDIR/wal02.new" << 'EOF'
    if (stored == checksum_of(payload)) {
EOF
run_mutation "M-WAL-02" "$WAL" "$TMPDIR/wal02.old" "$TMPDIR/wal02.new" "wal" $WAL_TARGETS

# M-WAL-03: decode the length from the wrong header offset (magic, not length).
cat > "$TMPDIR/wal03.old" << 'EOF'
    std::uint32_t length = get_le_u32(header.data() + 4);
EOF
cat > "$TMPDIR/wal03.new" << 'EOF'
    std::uint32_t length = get_le_u32(header.data());
EOF
run_mutation "M-WAL-03" "$WAL" "$TMPDIR/wal03.old" "$TMPDIR/wal03.new" "wal" $WAL_TARGETS

# M-WAL-04: enlarge the bounded read chunk beyond the documented bound (mutate
# the call site so the oracle's read_chunk_size() bound stays the true one).
cat > "$TMPDIR/wal04.old" << 'EOF'
        const std::size_t chunk = detail::read_chunk_size(payload_size - old_size);
EOF
cat > "$TMPDIR/wal04.new" << 'EOF'
        const std::size_t chunk = detail::read_chunk_size(payload_size - old_size) + 64 * 1024;
EOF
run_mutation "M-WAL-04" "$WAL" "$TMPDIR/wal04.old" "$TMPDIR/wal04.new" "wal" $WAL_TARGETS

# M-WAL-05: make the vector encoder emit a different checksum.
cat > "$TMPDIR/wal05.old" << 'EOF'
    put_le_u32(trailer.data(), checksum_of(payload));

    // Max 3 slices (header, payload, checksum) — stack-allocated to avoid
EOF
cat > "$TMPDIR/wal05.new" << 'EOF'
    put_le_u32(trailer.data(), checksum_of(payload) + 1);

    // Max 3 slices (header, payload, checksum) — stack-allocated to avoid
EOF
run_mutation "M-WAL-05" "$WAL" "$TMPDIR/wal05.old" "$TMPDIR/wal05.new" "wal" $WAL_TARGETS

# ===========================================================================
# copy_all mutants (src/copy.cpp).
# ===========================================================================
COPY="src/copy.cpp"
COPY_TARGETS="copy_all_fault_fuzz fuzz/corpus/copy_all_fault"

# M-COPY-01: remove the remaining-limit clamp from the Reader request size.
cat > "$TMPDIR/copy01.old" << 'EOF'
            to_read = static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), left));
EOF
cat > "$TMPDIR/copy01.new" << 'EOF'
            to_read = static_cast<std::size_t>(scratch.size());
EOF
run_mutation "M-COPY-01" "$COPY" "$TMPDIR/copy01.old" "$TMPDIR/copy01.new" "copy" $COPY_TARGETS

# M-COPY-02: remove total += got (the loop accumulator).
cat > "$TMPDIR/copy02.old" << 'EOF'
        total += got;
EOF
cat > "$TMPDIR/copy02.new" << 'EOF'
        // total += got; // mutated away
EOF
run_mutation "M-COPY-02" "$COPY" "$TMPDIR/copy02.old" "$TMPDIR/copy02.new" "copy" $COPY_TARGETS

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
run_mutation "M-COPY-03" "$COPY" "$TMPDIR/copy03.old" "$TMPDIR/copy03.new" "copy" $COPY_TARGETS

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
run_mutation "M-COPY-04" "$COPY" "$TMPDIR/copy04.old" "$TMPDIR/copy04.new" "copy" $COPY_TARGETS

# M-COPY-05: remove the got > requested defensive rejection.
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
run_mutation "M-COPY-05" "$COPY" "$TMPDIR/copy05.old" "$TMPDIR/copy05.new" "copy" $COPY_TARGETS

# M-COPY-06: make the deferred Reject policy fall through instead of returning.
cat > "$TMPDIR/copy06.old" << 'EOF'
            // Default policy: return invalid_state, touch nothing.
            dec.unsupported_requested = true;
            dec.reason = "deferred_not_implemented";
            if (stats) {
                ++stats->strategy_deferred_rejected_calls;
                ++stats->reader_error_stops;
            }
            return make_unexpected<std::uint64_t>(IoError{.code = IoError::Code::invalid_state});
        }
EOF
cat > "$TMPDIR/copy06.new" << 'EOF'
            // Default policy: return invalid_state, touch nothing.
            dec.unsupported_requested = true;
            dec.reason = "deferred_not_implemented";
            if (stats) {
                ++stats->strategy_deferred_rejected_calls;
                ++stats->reader_error_stops;
            }
            // mutation: fall through instead of returning
        }
EOF
run_mutation "M-COPY-06" "$COPY" "$TMPDIR/copy06.old" "$TMPDIR/copy06.new" "copy" $COPY_TARGETS

# ===========================================================================
# Report.
# ===========================================================================
echo ""
echo "================ MUTATION RESULTS ================"
printf "%-12s %-10s %s\n" "MUTANT" "RESULT" "KILLER (target:seed)"
for r in "${RESULTS[@]}"; do
    printf "%-12s %s\n" "${r%%  *}" "${r#*  }"
done
TOTAL_KILLED=$((WAL_KILLED + CP_KILLED))
TOTAL_SURVIVED=$((WAL_SURVIVED + CP_SURVIVED))
TOTAL_INVALID=$((WAL_INVALID + CP_INVALID))

echo "--------------------------------------------------"
echo "WAL:  KILLED=${WAL_KILLED}  SURVIVED=${WAL_SURVIVED}  INVALID=${WAL_INVALID}  (of 5)"
echo "COPY: KILLED=${CP_KILLED}  SURVIVED=${CP_SURVIVED}  INVALID=${CP_INVALID}  (of 6)"
echo "TOTAL: KILLED=${TOTAL_KILLED}  SURVIVED=${TOTAL_SURVIVED}  INVALID=${TOTAL_INVALID}  (of 11)"
echo "=================================================="

if [[ $TOTAL_SURVIVED -gt 0 || $TOTAL_INVALID -gt 0 ]]; then
    echo "VERDICT: FAIL (survived=${TOTAL_SURVIVED} invalid=${TOTAL_INVALID})"
    exit 1
fi
echo "VERDICT: PASS (all 11 mutants killed)"
exit 0
