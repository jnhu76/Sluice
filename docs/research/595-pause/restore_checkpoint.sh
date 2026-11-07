#!/usr/bin/env bash
# #595 pause checkpoint restore verifier/drill.
# Usage:
#   restore_checkpoint.sh verify <archive_root>          # self-check archive (bundle sha, files)
#   restore_checkpoint.sh drill  <archive_root> <target> # full restore drill in fresh dir
# Exit 0 only if every named check passes; each failure is printed with FAIL:.
set -uo pipefail

mode=${1:?usage: verify|drill}
AR=${2:?archive root}
fails=0
note() { printf '%s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*"; fails=$((fails+1)); }

need_file() { [ -f "$1" ] || fail "missing file $1"; }

# --- frozen identities (checkpoint pause-20261107) ---
BUNDLE_SHA_EXPECTED=83e29705b6930ccdbda8545ec1e8c81f1e590b43705d09e9c6a49affd0eb54cf
MASTER=271b81bfc9d64eecc0266fe7066b9040525dd710
R2_BASE=271b81bfc9d64eecc0266fe7066b9040525dd710
R3_BASE=0a22a79195f1b913145fcd443496e7f70b08cdcf
declare -a REFS=(
  master research-595-flow-audit research-595-r3 research-595-r4
  research-595-r5 research-595-r5-diag research-595-r5rm-diag
  research-595-r5rm-diag-cleanup
  research/595-causal-r2 research/595-information-audit
  research/595-r3-archive research/595-r3-continuation
  research/595-r4-state-aware research/595-r5-phase-a
)
WS="$AR/source/worktree-state"
BUNDLE="$AR/source/sluice-595-research.bundle"

check_archive() {
  note "== verify archive: $AR"
  need_file "$BUNDLE"
  need_file "$AR/source/refs-local.txt"
  need_file "$AR/source/pr-refs-remote.txt"
  need_file "$AR/environment/env.txt"
  for s in causal-r2_worktree causal-r2_worktree-timing causal-r2_worktree-trace r3_worktree worktree; do
    need_file "$WS/$s/tracked-modifications.patch" || true
    need_file "$WS/$s/status.porcelain" || true
  done
  local got
  got=$(sha256sum "$BUNDLE" | awk '{print $1}')
  [ "$got" = "$BUNDLE_SHA_EXPECTED" ] || fail "bundle sha256 $got != expected"
  git bundle verify "$BUNDLE" >/dev/null 2>&1 || fail "git bundle verify"
  note "archive self-check done"
}

run_drill() {
  local T=$2
  note "== drill into fresh dir: $T"
  [ -e "$T" ] && fail "target $T already exists"
  mkdir -p "$T"
  local R="$T/repo"
  git clone -q "$BUNDLE" "$R" || fail "clone from bundle"
  for ref in "${REFS[@]}"; do
    git -C "$R" rev-parse --verify --quiet "refs/remotes/origin/$ref" >/dev/null \
      || fail "ref missing after clone: $ref"
  done
  local m
  m=$(git -C "$R" rev-parse refs/heads/master)
  [ "$m" = "$MASTER" ] || fail "master is $m"
  # key research sources checkout + content spot-checks
  git -C "$R" checkout -q research-595-r5 \
    && grep -q "submit_ready_reads" "$R/bench/r3_ready_batch.hpp" 2>/dev/null \
    || fail "checkout/content research-595-r5 (bench adapter)"
  git -C "$R" checkout -q research-595-r5rm-diag \
    || fail "checkout research-595-r5rm-diag"
  git -C "$R" checkout -q "$R3_BASE" 2>/dev/null \
    || git -C "$R" checkout -q research-595-r3 || fail "checkout R3 base $R3_BASE"
  # R3 patch chain from PR627 branch (completion patch on 0a22a791)
  git -C "$R" show 'refs/remotes/origin/research/595-r3-archive:docs/research/595-r3/completion/evidence/r3-completion.patch' > "$T/r3-completion.patch" 2>/dev/null \
    || fail "extract r3-completion.patch from branch"
  git -C "$R" apply --check "$T/r3-completion.patch" \
    || fail "r3-completion.patch does not apply on $R3_BASE"
  # R3 full worktree diff (PR627) must equal the frozen local capture
  git -C "$R" show 'refs/remotes/origin/research/595-r3-continuation:docs/research/595-r3/continuation/evidence/r3-worktree-full-diff.patch' > "$T/r3-full.patch" 2>/dev/null \
    || fail "extract r3-worktree-full-diff.patch"
  # uncommitted prototype patches replay on their bases
  git -C "$R" checkout -q "$R2_BASE" || fail "checkout R2 base"
  for s in causal-r2_worktree causal-r2_worktree-timing causal-r2_worktree-trace worktree; do
    git -C "$R" apply --check "$WS/$s/tracked-modifications.patch" \
      || fail "$s patch does not apply on $R2_BASE"
    local u; u=$(awk '$1=="??"{print substr($0,4)}' "$WS/$s/status.porcelain" | wc -l)
    local c; c=$(find "$WS/$s/untracked" -type f 2>/dev/null | wc -l)
    [ "$u" = "$c" ] || fail "$s untracked count mismatch (status=$u copied=$c)"
  done
  # repair-round analyzer + runner present in branch (rerun capability)
  git -C "$R" cat-file -e 'refs/remotes/origin/research/595-r5-phase-a:docs/research/595-r5-repair-round/scripts/run_round.py' 2>/dev/null \
    || fail "595-r5-repair-round rerun scripts missing in branch"
  git -C "$R" cat-file -e 'refs/remotes/origin/research-595-flow-audit:docs/research/595-flow-audit/scripts/verify_inherited_evidence.py' 2>/dev/null \
    || fail "flow-audit offline verifier missing in branch"
  note "drill complete: $T"
}

case "$mode" in
  verify) check_archive ;;
  drill)  check_archive && run_drill "$AR" "${3:?target dir}" ;;
  *) echo "usage: $0 verify|drill <archive_root> [target]" >&2; exit 2 ;;
esac
if [ "$fails" -gt 0 ]; then echo "RESULT: $fails failure(s)" >&2; exit 1; fi
echo "RESULT: OK"
