# Exact commands (clean-tree closure)

```bash
# 1. Sync PR #260 branch, require a clean tree
git fetch origin
git checkout research/tax0-a2-control-plane
git reset --hard origin/research/tax0-a2-control-plane   # 7cc294a
git status --short                                        # must be empty

# 2. Build the exact committed implementation (Release + clang + liburing)
xmake f -m release --toolchain=clang --with-liburing=true -y
xmake build tax0_z_ladder_bench
sha256sum build/linux/x86_64/release/tax0_z_ladder_bench
#   f854642e894df2c85efc26f228c343eea7f6453436227f467ab48c4104a352ba
#   (identical to the canonical formal4 session binary)

# 3. Run the closure OUT of the repo so the tree stays `git status`-clean
#    during measurement (driver: research/tax0/scripts/tax0z_closure.py)
python3 /tmp/tax0z_closure.py run \
  --session tax0-clean-tree-closure-2026-09-01 \
  --out-dir /tmp/closure-out

# 4. Compare against the original canonical session (same-work + deltas)
python3 /tmp/tax0z_closure.py compare --session tax0-clean-tree-closure-2026-09-01
python3 /tmp/tax0z_closure.py report --session tax0-clean-tree-closure-2026-09-01

# 5. Copy the session into the repo and commit with the closure report
cp -r /tmp/closure-out/tax0-clean-tree-closure-2026-09-01 research/tax0/results/
# + research/tax0/TAX0-REPRODUCIBILITY-CLOSURE.md
```

## Measurement protocol (same as canonical, see tax0z.py)

- `--warmup 0`, formal rep pairs R7/R14, double-difference normalization.
- perf events: `instructions:u,cycles:u,branch-misses:u,cache-misses:u`.
- Write arms: runner-side post-exit full-file byte verification.
- Same-work gate per cell (ops/bytes/word_sum) and cross-arm equality.
