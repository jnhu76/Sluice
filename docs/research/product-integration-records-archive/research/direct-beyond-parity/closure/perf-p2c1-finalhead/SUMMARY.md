# P2C1 Final-HEAD Performance Regression Check — E5 (perf-p2c1-finalhead)

Session: 2026-10-29 15:18–15:29 +08:00 · host `E5` · kernel `7.2.11-200.fc44.x86_64`
Ask: charter Phase 3.3 — one controlled Final-HEAD check, base = master
`43cb8029ca60b572c91a9aa9dccb17d4300e2f88` (C1 merged), candidate = PR #612
re-anchored head `645082821a11ffcc1f835ab46088f54a5add4a64`.
Methodology: kit `/home/jnhu/Source/Sluice-direct-cost-ladder` @ `4455d98d`,
tool copies hash-identical to the R4 session `research/direct-beyond-parity/
closure/perf-p2c1/` receipts (ladder.cpp `fa26ac36…`, paired2.sh `0f0e0316…`,
paired.sh `be811ac8…`, mkfixture.cpp `63bd8858…`, analyze2-p5.py `1dd959d5…`).

## 1. Identity [RUN]

- Fresh worktrees: `/home/jnhu/Source/Sluice-perf-final-base` @ 43cb8029,
  `/home/jnhu/Source/Sluice-perf-final-head` @ 64508282 (both detached,
  created this session). Build recipe identical to R4 (`xmake f -p linux
  -a x86_64 -m release --liburing=n --kind=static --hardened=n
  --sanitizers= --ccache=y -y; xmake -y`, CCACHE_DISABLE=1).
- Base archive sha256 `e65a252fb0040f3e913fd41a31a41f99977c7bf9e7661480f126a3f0d207ebcf`
  == R4 base archive (re-hashed live at `/home/jnhu/Source/Sluice-perf-p2c1-base/...`).
- Head archive sha256 `a536bc6ac37c5c680f2cbe5d1714df7e09dbaf1ef1945cb509969f9bf110055f`
  == R4 head archive (re-hashed live at `/home/jnhu/Source/Sluice-perf-p2c1-head/...`).
- Ladder binaries byte-identical to R4's: `ladder-base 89084b1d…`,
  `ladder-cand 3af44bf9…`; g++ (GCC) 16.2.1 20260819.
- `src/ include/ apps/ xmake*` diff `77e5399a`→`43cb8029` is EMPTY — the new
  base is code-identical to the R4 baseline (research/docs-only delta).
- Consequence: this session measures EXACTLY the machine code R4 measured;
  deltas vs R4 are attributable to session conditions, not code.
- Fixture: `/tmp/sluice600/data64m.bin` sha256 `bc020fd1…2993a` (pinned
  identity, unchanged start→end). Write cells ran on a byte-copy
  `/tmp/sluice600/data64m-write.bin` so write arms (LCG pattern, ladder.cpp:1646)
  could never touch the read fixture.
- Smoke [RUN]: ax seq checksum 69632 equal on both binaries; ax tail 40960
  equal; wx content_ok=1; read fixture untouched.

## 2. Environment [RUN] (`ENV-P2C1-FINALHEAD.txt`)

Xeon E5-2666 v3 (10c/20t), governor schedutil, `perf_event_paranoid=2`,
loadavg 0.12 start / 0.10 end (transient 0.72 immediately after the A/B
burst), 0 matching measurement processes at both surveys, `taskset -c 2`,
strictly serial invocations.

## 3. Pre-registration [RUN] (`PREREGISTERED_PLAN.txt`, written before sampling)

A/A first (16 pairs per cell, ≥8 required); A/B 16 pairs per shape, hard cap
24; extension ONLY for a candidate-UNFAVORABLE CI-excluding 16-pair result
(regression firming), never to chase a win; comp-read-4k never extended merely
to resolve its win. Statistic ladderA − ladderB = cand − base (negative favors
candidate); analyzer `analyze2-p5.py --order`, t(0.975,15)=2.131.

## 4. A/A noise [RUN] — first, per discipline (16 pairs per cell, cand×cand)

| cell | sd_AA | median | CI95 | verdict |
|---|---|---|---|---|
| ax seq | 27.022 | +2.045 | [−10.91,+17.88] | null |
| ax tail | 13.595 | −1.998 | [−11.91,+2.57] | null |
| ac | 24.489 | −12.833 | [−25.38,+0.71] | null (t=−2.02, barely spans 0) — drift disclosed |
| a2 | 18.775 | −2.997 | [−11.80,+8.20] | null |
| w2 | 45.341 | +5.811 | [−5.55,+42.76] | null (+181 outlier) |
| wx | 14.692 | +0.066 | [−3.11,+12.55] | null |
| wc | 29.591 | +0.638 | [−9.85,+21.68] | null |

Fresh MDE′(16) = 2.131·sd_AA/4: ax-seq 14.40, ax-tail 7.24, ac 13.04, a2 10.00,
w2 24.15, wx 7.83, wc 15.77 ns/op. Machinery unbiased overall, but the ac cell
showed a −12.8 ns median A/A drift — an A-favorable machinery bias bound for
that shape this session. [UNCERTAIN] attribution: schedutil frequency behavior
vs host state; disclosed, not absorbed.

## 5. Paired A/B (ABBA, 16 pairs each — no cell extended; trigger never met)

| cell | median ns/op | mean | CI95 | pos | AB/BA medians | verdict |
|---|---|---|---|---|---|---|
| comp-read-4k (ax seq) | **−13.163** | −11.749 | [−32.62,+9.13] spans 0 | 1/16 | −10.42/−16.85 | **INCONCLUSIVE** (preserved; favorable direction, no deterministic regression) |
| eofmix (ax tail) | **−13.012** | −12.985 | [−21.96,−4.01] excl 0 | 4/16 | −12.97/−13.01 | direction **PRESERVED**; win evidence standard met this session (|median| 13.01 ≥ MDE′ 7.24) |
| cursor (ac) | **−15.601** | −15.288 | [−30.80,+0.22] barely spans 0 | 5/16 | −14.23/−17.74 | direction **PRESERVED** (both subgroups negative); magnitude confounded by the cell's −12.8 A/A drift — not independently re-established this session; R4 WIN_CONFIRMED stands; **no regression** |
| neg read_at (a2) | **−11.282** | −7.102 | [−17.02,+2.82] spans 0 | 6/16 | −11.28/−7.55 | no deterministic deviation; favorable median on machine-identical code = session drift disclosure; equivalence below ~11 ns not wall-resolvable this session |
| neg write_at (w2) | **−6.522** | +4.512 | [−21.33,+30.35] spans 0 | 4/16 | −8.78/−3.90 | no deterministic deviation (+183 outlier) |
| write_all_at (wx) | **−12.519** | −9.912 | [−22.35,+2.53] spans 0 | 6/16 | −4.53/−15.90 | **no apparent regression**; disclosure only, no claim |
| write_all (wc) | **−7.869** | −7.102 | [−25.46,+11.26] spans 0 | 5/16 | −10.86/−5.63 | **no apparent regression**; disclosure only, no claim |

Checksum gates [RUN]: single value per cell across all arm lines — read seq
`25165824`, tail `12595200` (= R4's exact values); write cells content_ok=1
with single checksum. 32 sample lines per receipt.

Session-wide disclosure: BOTH negative-control cells show A-favorable medians
(−11.3 / −6.5) — a session-wide A-favorable machinery drift of roughly −6…−13
ns median. All favorable A/B medians must be read against that band. For the
REGRESSION question this bias is conservative (it works against finding
candidate-slower results), and no cell is more unfavorable than its control
band: comp-read-4k −13.2 / a2 −11.3, eofmix −13.0 / a2 −11.3, cursor −15.6 /
a2 −11.3, wx −12.5 / w2 −6.5, wc −7.9 / w2 −6.5.

## 6. Machine identity [RUN] (`machid/`)

`objdump -d --disassemble=<mangled>` on `blocking_file.cpp.o` from each fresh
archive, instruction mnemonics + operands (offsets stripped), base vs head:

- `read_at` 126 insns **IDENTICAL**; `write_at` 126 **IDENTICAL**; `read` 115
  **IDENTICAL**; `file_info` 114 **IDENTICAL** — every non-composition public
  primitive checked is machine-identical (negative-control guarantee).
- `read_exact_at` 240 → 174 insns **DIFF** (−27.5%, exactly R4's figure);
  `read_exact` 208 → 167 **DIFF**; `write_all_at` 240 → 211 **DIFF**;
  `write_all` 208 → 194 **DIFF**.
- The write-composition DIFFs are explained: the shared `compose()` template
  was refactored (CompositionState machine → plain confirmed_bytes loop with
  direct outcome builders; diff 43cb8029..64508282 src/blocking_file.cpp), and
  all four composition forms instantiate it. The write steps themselves still
  route through the machine-identical `write_at`/`write` primitives. Wall
  parity for both write cells is confirmed above (§5).

## 7. Commands

```
cd /home/jnhu/Source/Sluice-perf-finalhead
git -C /home/jnhu/Source/Sluice worktree add --detach /home/jnhu/Source/Sluice-perf-final-base 43cb8029ca60b572c91a9aa9dccb17d4300e2f88
git -C /home/jnhu/Source/Sluice worktree add --detach /home/jnhu/Source/Sluice-perf-final-head 645082821a11ffcc1f835ab46088f54a5add4a64
# in each worktree: xmake f -p linux -a x86_64 -m release --liburing=n --kind=static --hardened=n --sanitizers= --ccache=y -y && xmake -y
./build-both.sh
./run_aa.sh <name> ./ladder-cand ./ladder-cand <arm> 4096 1024 1024 5 16 <fixture> [tail]
./run_ab.sh <name> ./ladder-cand ./ladder-base <arm> 4096 1024 1024 5 16 <fixture> [tail]
python3 analyze2-p5.py --order < <receipt>
```
Read fixture `/tmp/sluice600/data64m.bin`; write fixture
`/tmp/sluice600/data64m-write.bin`. Every receipt embeds its `# cmd:` line.

## 8. Verdict

**NO_NEW_REGRESSION.** No measured shape shows a candidate-unfavorable
CI-excluding result; comp-read-4k INCONCLUSIVE semantics preserved (CI spans
0; favorable direction; never extended); eofmix and cursor benefit directions
preserved (eofmix also CI-excluding at 16 pairs); negative controls show no
deterministic deviation (machine-identical code; favorable wall medians
disclosed as session drift); write_all_at/write_all show no unexplained
regression (CI spans 0; disassembly delta explained by the shared compose()
refactor; write primitives machine-identical). Budget: 16 ABBA pairs per
shape × 7 shapes (cap 24 unused), A/A 16 pairs × 7 cells. No GitHub writes,
no merges, no test or threshold relaxation from this session. R4's
WIN_CONFIRMED conclusions for eofmix/cursor and INCONCLUSIVE for comp-read-4k
are the standing authorities; this session corroborates direction on all
three and adds the negative-regression evidence at the Final HEADs.
