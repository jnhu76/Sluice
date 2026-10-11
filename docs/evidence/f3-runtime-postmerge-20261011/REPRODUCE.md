# Exact postmerge package audit reproduction

Audit ID: F3_POSTMERGE_20261011_0d365624_DEBUG_N_Y.
Baseline: 0d365624661b2b6569dd4bfa68fe095f8cd59ece.
Original remote capture: /home/jnhu/f3-postmerge-package-20261011.
Independent worktree: /home/jnhu/f3-runtime-retirement-audit.
Debug Linux x86_64; xmake 3.0.9+20260621; pkg-config liburing 2.13.
Both profiles build/install successfully; 50 library objects, 37 async archive
members, 79 installed headers each. No runtime behavioral matrix is claimed.

~~~sh
export PATH="$HOME/.local/bin:$PATH"
base=$(mktemp -d /home/jnhu/f3-postmerge-reproduce-XXXXXX)
repo=/home/jnhu/f3-runtime-retirement-audit
for p in n y; do
    root="$base/profiles/$p"
    mkdir -p "$root"
    git -C "$repo" archive 0d365624661b2b6569dd4bfa68fe095f8cd59ece |
        tar -x -C "$root"
    cd "$root"
    xmake f -P "$root" -o "$root/build" -m debug --liburing="$p" -y
    xmake build -P "$root" -j4 -v sluice_async
    xmake build -P "$root" -j4 -v -g apps
    xmake install -P "$root" -o "$base/installed-$p" sluice_async
    xmake install -P "$root" -o "$base/installed-$p" sluice_core
done
~~~

Capture scripts are included in raw-build-package-symbols.tar.gz.
Extract into the new reproduction directory; update the scripts
original absolute base, profile and output-worktree paths to the chosen fresh
directory before running them. The original captures must remain untouched;
python3 "$base/capture.py" collects xmake resolved sources/objects after
config.load; actual object paths; installed headers; ar members; complete
defined and undefined nm; g++ -MM and -fsyntax-only for
Request/context/host/task_result; installed-prefix host compile/link/execute
with GNU linker map; file and full nm for each Debug app.
The linked probe constructs the ThreadPool context and optional host; it proves
link closure and simple construction/destruction, not supported W-04 behavior.
python3 "$base/classify.py" checks every resolved object exists and prepares
normalized inventories, exact 79-header role classification, full matched
runtime-family linked symbols, raw SHA-256 manifest and compressed raw evidence.

All capture scripts and raw evidence reside in the archive; small JSON/TXT
inventories are directly reviewable. raw-evidence-sha256.json hashes each
uncompressed raw capture by path relative to the original base. Complete
logs/nm/maps include linker-selected archive members, rather than only filtered
zero matches. Source/object paths in resolved JSON are relative to each profile;
object inventories confirm actual Debug locations.

The earlier BEFORE report is /home/jnhu/f3-baseline-audit-20261011-152229/evidence/report.md
at 5833f525b5b271d6a62547c30e4d7cd5499cce41. Historical F1 artifacts are immutable.
This dated audit shows adoption without physical package contraction.
The authorized deletion implementation must produce its own new B-01
versioned package cut and explicit delta; this audit is not that release cut.
