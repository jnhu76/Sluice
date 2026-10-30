# E5 Raw Evidence Preservation — Execution Receipt

Execution record for
[`../E5_RAW_EVIDENCE_PRESERVATION_PLAN.md`](../E5_RAW_EVIDENCE_PRESERVATION_PLAN.md)
(§3 fixture rebuild, §4 audit, §5 independent copy). This receipt records what
was run and with what outcome; it contains no credentials. The E5 host is
accessed only as `jnhu@192.168.31.75` (ssh / rsync over ssh); no other E5
address or identity is used anywhere in this record.

**Verdict: `VERIFIED`** — existence confirmed, frozen consistency confirmed
with fully attributed drift, full manifest self-check clean, fixture group
REBUILD_VERIFIED, independent backup verified at its destination, restore
spot-check 10/10. Failures: 0.

Provenance of statements below:

- **[E5-round]** — observed on the E5 host / from the backup pull during the
  2026-10-30 execution round; output summaries transcribed from that round's
  captured results (scale: full tree).
- **[rec-check]** — re-executed on this WSL2 recording host on 2026-10-30 by
  the session that wrote this receipt, against the backup destination; exact
  commands and outputs given inline.

Timeline: backup copy completed 2026-10-29T23:41:41Z; E5-side existence
probes, manifest generation (header `generated_on 2026-10-30T07:13:33+08:00`)
and fixture rebuild on 2026-10-30; receipt recorded 2026-10-30.

---

## 1. Existence and current state of the E5 evidence tree

Source root: `/home/jnhu/sluice-593-research/evidence` on E5.

[E5-round] Commands and outputs (run on / against E5, read-only):

```sh
find /home/jnhu/sluice-593-research/evidence -type f | wc -l   # → 6913
du -sb /home/jnhu/sluice-593-research/evidence                 # → 1423204159  (bytes, apparent size)
```

Per-top-level-directory file counts (`find <dir> -type f | wc -l` per dir):

| Top dir under `evidence/` | Files |
|---|---|
| `ab/` | 4,291 |
| `discrimination/` | 2,171 |
| `attribution/` | 178 |
| `baseline/` | 173 |
| `worktree-snapshots-20261026/` | 43 |
| `K4-b-mutation/` | 28 |
| `K2-a-mutation/` | 25 |
| **Total** | **6,913** |

Two-level structure: 43 directories at `find -maxdepth 2 -type d`. `ab/`
contains `K2-a/`, `K4-b/` and the long-named combo directory
`combo-K4-b (topology+depth-gated deferred flush)+K1(a+b) merged (publication snapshot + no-observer delivery gate)`
(contains `census/` and `w03_uring_4k_d16/`); `attribution/` contains
`census/`, `diag/`, `overhead/`; `baseline/` contains `attribution/`,
`disk-fixture/`, `patches/`, `probes/`, `r-diag/`, `r-disk/`,
`r-uring-core64/`, `r0-r3/`, `smoke/`; `discrimination/` contains `k1`–`k4`;
`worktree-snapshots-20261026/` contains 9 worktree snapshot directories
(`baseline`, `K2-a`, `K4-b`, `combo-K4-b-K1ab`, `k1-elision`, `k2-elision`,
`k3-fusion`, `k4-defer`, `observe`).

Extension census (top 15, `find -type f`, lowercased extension counts):
`log` 2314, `jsonl` 2075, `check` 1920, `txt` 432, `obs` 34, `csv` 24, `sh` 21,
`tsv` 13, `md` 13, `diff` 12, `py` 11, `bin` 10, `patch` 9, `json` 7, `data` 6;
plus 2 files without an extension (`find ! -name '*.*'` count).

**Writers / open handles [E5-round]:**

```sh
timeout 60 lsof +D /home/jnhu/sluice-593-research/evidence
# → empty output, exit code 1 = no open files (lsof 4.98.0 confirmed present)
ps aux | grep -E 'sluice|bench|xmake|python' | grep -v grep
# → only system daemons /usr/bin/firewalld, tuned, tuned-ppd
#   (root python3 system services, unrelated to the evidence tree);
#   no sluice/bench/xmake process
```

The writer identity of the newest 43 files
(`worktree-snapshots-20261026/`, mtime 2026-10-26 23:39:45) could not be
determined — the files carry no creator metadata; see §2.

**Filesystem and mount [E5-round]:**

```sh
df -T /home/jnhu/sluice-593-research
# /dev/sda3  btrfs  466138112 1K-blocks total, 85200120 used, 379146344 avail (19%), mounted /home
findmnt -T /home/jnhu/sluice-593-research/evidence
# TARGET /home  SOURCE /dev/sda3[/home]  FSTYPE btrfs
# OPTIONS rw,relatime,seclabel,compress=zstd:1,ssd,discard=async,space_cache=v2,subvolid=257,subvol=/home
```

SELinux enabled (`seclabel` in the findmnt options; `id` context
`unconfined_u`); btrfs mounted with `compress=zstd:1`.

**Anomalies observed (all bounded; none alters the verdict):**

1. One top-level directory name contains spaces, parentheses and `+`
   (the `ab/combo-K4-b (topology+depth-gated deferred flush)+K1(a+b) merged …`
   name above) — a quoting/escaping hazard for scripted handling.
2. 2 files without a file extension (`find ! -name '*.*'` count).
3. Snapshot-directory file counts are uneven: `K2-a` 6, `combo-K4-b-K1ab` 7,
   `k1-elision` 5, `k4-defer` 5 (each including captured untracked source
   files), the other 5 worktree dirs 4 each — itemized in §2; accounted for,
   not an unexplained increment.
4. The remote non-interactive shell PATH lacks `date`/`tr`
   (`zsh: command not found`) — worked around with `find -printf` time
   formatting; does not affect any conclusion.
5. The `#594` evidence-index item-by-item comparison could not be executed:
   `research/593-key-path` does not exist in the local repository (absent from
   the working tree, the `HEAD` tree, and `git log --all`).
6. In the first `lsof` run, the `|| echo NO_OPEN_FILES` fallback marker did not
   print because the pipeline's exit code was taken from `head`; `lsof` was
   re-run separately and confirmed present (4.98.0) with exit 1 = no open
   files.
7. SELinux enabled and btrfs `zstd:1` compression mounted (see above) —
   environmental facts, no action required.

---

## 2. Frozen consistency — drift vs the recorded baseline, fully attributed

Recorded baseline (plan §1, from `docs/research/key-path-593/evidence-index.md`
§1): 6,870 files / 1,422,946,592 bytes. Observed 2026-10-30: 6,913 files /
1,423,204,159 bytes. Drift: **+43 files / +257,567 bytes.**

[E5-round] Attribution — the drift corresponds exactly, file-for-file and
byte-for-byte, to one directory:

```sh
find evidence/worktree-snapshots-20261026 -type f | wc -l   # → 43
du -sb evidence/worktree-snapshots-20261026                 # → 257567
# 6913 - 6870 = 43 ; 1423204159 - 1422946592 = 257567 — both deltas match exactly
```

All 43 files have mtime 2026-10-26 23:39:45 (one single write batch).
Composition: 9 worktree snapshot directories × 4 regular files each
(`HEAD.txt`, `status.txt`, `tracked.diff`, `untracked-list.txt`) = 36, plus 7
captured untracked source files:

- `K2-a/tests/k2_e18_poison_eligibility_test.cpp`
- `K2-a/tests/k2_e18_residue_wake_test.cpp`
- `combo-K4-b-K1ab/tests/k2_e18_poison_eligibility_test.cpp`
- `combo-K4-b-K1ab/tests/k4_residue_wake_test.cpp`
- `combo-K4-b-K1ab/tests/k4_retry_residue_wake_test.cpp`
- `k1-elision/src/async/observe_counters.hpp`
- `k4-defer/src/async/observe_counters.hpp`

Content provenance: `baseline/HEAD.txt` and `K2-a/HEAD.txt` both record
`bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`; `baseline/status.txt` shows
` M bench/bench_w02_pipeline.cpp` etc. — i.e. the 43 files are snapshots of 9
Sluice git worktrees at commit `bdfd0d8d` with uncommitted arm changes.
[rec-check] That commit exists on `master` of this repository with exactly the
recorded identity:

```sh
$ git -C /home/hoo/Projects/Sluice log -1 --date=iso bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57
bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57
jm.hu <5766215+jnhu76@users.noreply.github.com>
2026-10-25 20:55:08 +0800
research(uring): archive #580 evidence — NO_TRIGGER_NO_CHANGE (#592)
```

It follows that the historical 6,870 / 1,422,946,592 baseline was taken between
2026-10-26 04:00:55 (`combo oracle-gate.txt`, the last write before the
snapshot batch) and 2026-10-26 23:39:45.

**Not resolved:** which session/script initiated the snapshot operation — the
files themselves carry no metadata that could establish this. **Not executed:**
the `#594` index comparison (anomaly 5 in §1). Neither affects the frozen-
consistency verdict: the drift is exactly the snapshot directory and nothing
else; no deletions or modifications elsewhere.

**Conclusion: frozen consistency CONFIRMED** (drift +43 / +257,567 fully
attributed; zero unexplained delta).

---

## 3. Full per-file manifest and self-check

[E5-round] Generated on E5 (2026-10-30), outside `evidence/`:

- `E5_EVIDENCE_MANIFEST.json` — full-field manifest, header:
  `generated_on 2026-10-30T07:13:33+08:00`, `host E5`,
  `kernel 7.2.11-200.fc44.x86_64`,
  `source_root /home/jnhu/sluice-593-research/evidence`,
  `file_count 6913`, `total_bytes 1423204159`; per-file entries carry `path`
  (relative to `evidence/`), `size`, `sha256`, `category`.
- `E5_EVIDENCE_SHA256SUMS` — 6,913 lines, `sha256sum -c` compatible; entries
  are prefixed `evidence/…` (note the prefix difference vs the JSON when
  scripting against both).

Self-check results (execution round): manifest line count = `find` file count
(6,913 = 6,913); both-direction verification clean (every manifest entry
resolves and matches; every on-disk file is manifested); 0 self-check
failures. Post-probe re-count of the evidence tree unchanged at 6,913 files /
1,423,204,159 bytes — the probes and manifest generation wrote nothing into
`evidence/`.

Self-hashes (sha256 of the two manifest files):

```
d8040d86af37544ed9d0d6e627a91fa304c77a7608687bbc2e56036fc2d619d1  E5_EVIDENCE_MANIFEST.json
6272d4a324622e53c0c58005215ec1a5625e94b3257ac3efe114e296e91a30f0  E5_EVIDENCE_SHA256SUMS
```

[rec-check] Re-hashed at the backup destination by this recording session
(2026-10-30) — both identical (commands and output in §5). Both files are
archived in this directory by the same commit that adds this receipt.

---

## 4. Fixture group — `baseline/disk-fixture/` REBUILD_VERIFIED

Plan §3 required a byte-identical regeneration test before the 1.35 GB fixture
group could be downgraded from "irreplaceable". That test was executed and
passed 10/10.

**Generator (true source — this repository's git objects, not a loose script):**
commit `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` (#593 frozen base).

- `bench/bench_common.hpp`: `kPatternPeriod = 65536`; `mix64(x)` (splitmix64
  constants); `pattern_table()[i] = (uint8_t)(mix64(i) >> 24)`;
  `fill_pattern(dst, len, base)` copies `table[base % 65536]` cyclically.
- `bench/bench_w01_direct.cpp` `write_file` / `Fixture::create` — sizes:
  `small` 4096, `write_4k` created 64 MiB (terminal state 409,600,000 B after
  the run's refill+timed writes), `read_4k_seq` / `read_4k_rand` 67,108,864,
  `read_64k` 134,217,728, `read_1m` 536,870,912, `composition` 4,194,304,
  `sync_small` 4096; written in 1 MiB chunks, content ≡ pattern over
  `[0, size)`.
- `bench/bench_w02_pipeline.cpp` and `bench/bench_w03_external_loop.cpp`
  `write_fixture` (`kFixtureBytes` = 64 MiB → `pipeline.bin` / `external.bin`).

**No seed exists or is needed:** the fixture bytes are a position-keyed fixed
table, not random data; the `mt19937_64(0x5eed403)` in the code only selects
read offsets and never enters file bytes. The terminal 409,600,000-byte state
of `write_4k.bin` is reproducible by argument and by measurement: the refill
phase writes `fill_pattern(chunk, written)` over `[0, 409600000)` and the timed
phase writes pool slot `(i % 1024) * 4096` at offset `i * 4096` — both depend
only on `i mod 16` (pattern period 65536 = 16 × 4096; 1024 ≡ 0 mod 16), so the
terminal state equals the pure pattern. (Arithmetic cross-check by the
recording session: the ten sizes above, with `read_4k_seq`, `read_4k_rand`,
`pipeline`, `external` each 67,108,864, sum to exactly 1,353,326,592 B — the
recorded group total.)

[E5-round] Rebuild (2026-10-30, in E5 `/tmp/fixture-rebuild-check` — never
inside `evidence/`): the three functions above copied verbatim into
`fixturegen.cpp`, compiled `g++ -O2 -Wall -Wextra -o fixturegen fixturegen.cpp`,
run per file `./fixturegen <name> <size>`; the sorted sha256 lists of rebuild
vs `evidence/baseline/disk-fixture` originals diff to empty — **ALL_MATCH
10/10**. After the rebuild the evidence tree re-counted 6,913 files /
1,423,204,159 bytes with `disk-fixture` mtimes still Oct 25 — zero writes to
the evidence tree.

Expected (and rebuild-produced) sha256 per file:

| sha256 | Files |
|---|---|
| `a3d459b35d0d79b1932eb3a313dbdf12428a84051145e0a694b43349a450c327` | `small.bin`, `sync_small.bin` |
| `a91d5e853b11215cd1d744f0daec3bc7587ae4a72e26264741cf8cc9f622fd02` | `composition.bin` |
| `1cdf83d6f6d97d740b0f6e0a10077f2ccde01172631885147705335f532744e6` | `read_4k_seq`, `read_4k_rand`, `pipeline`, `external` (four files, same hash — consistent with the position-keyed pattern) |
| `5601711cdbad6f05b78d5751e4e5030d30b041d5ac46fd358f11d0316898231d` | `read_64k.bin` |
| `a20ab4e6728ed770abf6f0d6c79b2f1c7723786a38b7bdb4b92502f12ae4d5ce` | `read_1m.bin` |
| `b658b9b1cb44f6625316a731892157d7d2a3e6f2cdddf8be27a3e5613ae93aa8` | `write_4k.bin` (409,600,000 B terminal state) |

Note: `/tmp/fixture-rebuild-check/` on E5 (`fixturegen.cpp`, bidirectional
sha256 lists, `REBUILD_LOG.txt`) is on tmpfs and will not survive a reboot —
for long-term archiving, this section (recipe + hashes) is the record.

**Generator search outcome [E5-round]:** no standalone `*.py`/`*.sh` generator
exists on E5 (only runners such as `run_stage.sh`, `attribution.sh`,
`analyze_baseline.py`); `run_stage.sh` passes the directory in as
`SLUICE_BENCH_TMP_DIR` (observed for the r-disk stage). The plan §1 command
against `research/593-key-path` failed in this repository
(`fatal: Not a valid object name` — branch absent, see §1 anomaly 5), and the
existing `research/600-direct-excess-cost` and `research/direct-beyond-parity`
contain no fixture/generator/seed files either. The generator anchor is
therefore commit `bdfd0d8d`'s `bench/` tree — which [rec-check] this recording
session confirmed exists in this repository (§2).

Also resolved (plan §3 UNKNOWN): the pre-existing `baseline/SHA256SUMS.txt`
covers evidence files + binaries but contains **no** `disk-fixture` entries
(`grep -c disk-fixture` = 0) — archive decisions must not infer from it that
the fixtures are droppable.

**Disposition of all groups:**

- `evidence/baseline/disk-fixture/` — **REBUILD_VERIFIED** (recipe above).
- KEEP_RAW (measured observations, no generator, not rebuild-verified;
  archived byte-for-byte in the backup): `evidence/baseline/` excluding
  `disk-fixture/` (r0/r1/r2/r3/r-diag/r-uring-core64/r-disk JSONL, observe.log,
  `attribution/`, `smoke/`, `probes/`, `patches/`, `baseline-summary.*`,
  `env-survey.txt`, `SHA256SUMS.txt`); `evidence/discrimination/`
  (38,051,256 B, k1–k4); `evidence/ab/` (16,721,621 B, K2-a/K4-b/combo);
  `evidence/attribution/` (11,637,942 B, diag/census timev/perf/strace
  outputs); the evidence-root manifests and environment files
  (`environment.md` etc. — `E5_EVIDENCE_MANIFEST.json` /
  `E5_EVIDENCE_SHA256SUMS` are the §3 artifacts, archived here).

---

## 5. Independent backup — verified at destination

**Backup location: `/home/hoo/sluice-593-evidence-backup/evidence/`** on this
WSL2 recording host (a different physical machine from E5; `df -h /home/hoo`
showed 812 G available at copy time). Completed 2026-10-29T23:41:41Z.

[E5-round] Method: pull with `rsync -a --info=stats1` from
`jnhu@192.168.31.75:/home/jnhu/sluice-593-research/evidence/`, executed in 2
rounds (no timeout-resume needed):

- Round 1 exit code 23: 6 `root:root 0600` perf `.data` files were
  Permission-denied sender-side:
  `attribution/diag/perfrec-w02-uring-4k-d16.data`,
  `attribution/diag/perfsched-w03-uring-4k-d16.data`,
  `discrimination/k1/sched-elide.data`, `discrimination/k1/sched-full.data`,
  `discrimination/k2/perfrec-elide.data`,
  `discrimination/k2/perfrec-full.data` (23,012,821 bytes total).
- Those 6 files were recovered via an ssh remote `sudo cat` binary stream and
  `touch`-ed to the remote mtimes; round 2 then exited 0.
- **The E5 side was not modified in any way** (no permission changes, no
  deletions) during the entire backup.

The two manifest files were copied to
`/home/hoo/sluice-593-evidence-backup/` and are hash-identical on both sides
(§3 hashes). Count and byte parity: both sides 6,913 files / 1,423,204,159
bytes (`du -sb` and the sum of file sizes agree).

[E5-round] Destination-side full verification: after rewriting the path prefix
in the sums file, `sha256sum -c` over all 6,913 entries → all `OK`, 0 non-OK,
empty stderr.

[rec-check] Re-verified at the destination by this recording session
(2026-10-30) — commands run and outputs observed:

```sh
$ cd /home/hoo/sluice-593-evidence-backup
$ find evidence -type f | wc -l
6913
$ du -sb evidence | cut -f1
1423204159
$ sha256sum E5_EVIDENCE_MANIFEST.json E5_EVIDENCE_SHA256SUMS
d8040d86af37544ed9d0d6e627a91fa304c77a7608687bbc2e56036fc2d619d1  E5_EVIDENCE_MANIFEST.json
6272d4a324622e53c0c58005215ec1a5625e94b3257ac3efe114e296e91a30f0  E5_EVIDENCE_SHA256SUMS
$ sha256sum -c E5_EVIDENCE_SHA256SUMS | grep -vc ': OK$'
0            # with stderr empty (0 bytes) — 6913/6913 OK
```

The manifest hashes equal the E5-side self-hashes recorded in §3. Nothing on
E5 or on this host was deleted by the backup or by this recording session.

---

## 6. Restore spot-check (backup → E5)

[E5-round] 10 files, chosen to cover all 7 top-level directories, the largest
file, the newest files, one sudo-recovered perf `.data`, and the key result
files:

1. `K2-a-mutation/README.md` (manifest line 1)
2. `ab/combo-K4-b (topology+depth-gated deferred flush)+K1(a+b) merged (publication snapshot + no-observer delivery gate)/census/census-perf.tsv` (manifest line 3457)
3. `baseline/disk-fixture/read_1m.bin` (largest file, 512 MiB)
4. `worktree-snapshots-20261026/observe/untracked-list.txt` (empty file,
   retained per policy)
5. the newest non-empty `tracked.diff` under `worktree-snapshots-20261026/`
6. `attribution/diag/perfsched-w03-uring-4k-d16-timehist-summary.txt`
7. `discrimination/k2/perfrec-full.data` (also re-exercises the sudo-recovered
   transfer path)
8. `K4-b-mutation/README.md`
9. `attribution/census-table.txt`
10. `discrimination/k2/pair-results.txt`

Method: `rsync -a --files-from` from the backup to
`E5:/tmp/restore-check/` (exit code 0), then sha256 on both sides: **10/10
identical**, and all 10 match the manifest entries.

---

## 7. Restore procedure (no credentials recorded)

To restore the full evidence tree from the backup to a target path on E5:

```sh
rsync -a -e 'ssh -o StrictHostKeyChecking=accept-new' \
  /home/hoo/sluice-593-evidence-backup/evidence/ \
  jnhu@192.168.31.75:<target-path>/
```

Then verify at the destination against the archived sums (entries are
`evidence/…`-prefixed; run from the directory containing `evidence/`):

```sh
sha256sum -c E5_EVIDENCE_SHA256SUMS | grep -vc ': OK$'   # expect 0
find evidence -type f | wc -l                            # expect 6913
```

Credentials for `jnhu@192.168.31.75` are supplied interactively (or via the
operator's own ssh agent/passphrase tooling) and are never written to any file
in this archive.

---

## 8. Artifacts archived alongside this receipt

| File | Bytes | sha256 |
|---|---|---|
| `E5_EVIDENCE_SHA256SUMS` | 1,022,755 | `6272d4a324622e53c0c58005215ec1a5625e94b3257ac3efe114e296e91a30f0` |
| `E5_EVIDENCE_MANIFEST.json` | 1,508,711 | `d8040d86af37544ed9d0d6e627a91fa304c77a7608687bbc2e56036fc2d619d1` |

The JSON is 1,508,711 bytes (< 3 MB) and is archived uncompressed — no gzip
was needed. Both hashes were re-computed from the archived copies by this
recording session (§5 [rec-check] block).
