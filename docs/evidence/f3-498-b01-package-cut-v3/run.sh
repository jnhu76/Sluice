#!/bin/bash
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
base=/home/jnhu/f3-498-package-cut-v3
repo=/home/jnhu/Source/Sluice-f3-498-pr-b
for stage in before after; do
 for p in n y; do
  root=$base/$stage/profiles/$p
  mkdir -p "$root"
  ref=f527456b5eaa10a74e872ee1d23b9dd6413f1f1d
  if [ "$stage" = after ]; then ref=ecf7286f1eea8b22739f95b50edfcd94c2dace7b; fi
  git -C "$repo" archive "$ref" | tar -x -C "$root"
  cd "$root"
  xmake f -P "$root" -o "$root/build" -m debug --liburing="$p" -y >"$base/$stage/evidence/config-$p.log" 2>&1
  xmake build -P "$root" -j4 -v sluice_async >"$base/$stage/evidence/build-$p.log" 2>&1
  xmake install -P "$root" -o "$base/$stage/installed-$p" sluice_async >"$base/$stage/evidence/install-async-$p.log" 2>&1
  xmake install -P "$root" -o "$base/$stage/installed-$p" sluice_core >"$base/$stage/evidence/install-core-$p.log" 2>&1
 done
 python3 "$base/$stage/capture.py" >"$base/$stage/capture.log" 2>&1
 done
