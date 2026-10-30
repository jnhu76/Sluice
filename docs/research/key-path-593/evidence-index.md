# Evidence index

This index maps every claim in this research record to the artifact that
supports it, with the persistent path and SHA-256 of the artifact. Raw
measurement data is **not** committed to the repository; the disclosure in §7 is
part of the record, not a footnote.

## 1. Persistent evidence workspace

| Item | Value |
|---|---|
| Root | `/home/jnhu/sluice-593-research` (E5 host, persistent filesystem) |
| Evidence | `/home/jnhu/sluice-593-research/evidence` |
| Research drafts | `/home/jnhu/sluice-593-research/mapping` and `brief.md` |
| Size | 6,870 files, 1,422,946,592 bytes (1.33 GiB) |
| Workload raw records | `pairs-*/{full,var,aa}-<n>.jsonl` (+ `.check`, `.log`), 55 pair directories, 15 pairs/case |
| Pair directory naming | `pairs-<case>-<mode>`; `mode` ∈ `elide-vs-full`, `aa-full`, `k4b-vs-combo-elide-vs-full` |
| Identity files per arm | `identity.txt`, `oracle-gate.txt`, `case-summary.tsv`, `pair-results.{txt,json}` |
| Census | `census/census-summary.txt`, `census.json`, `census-{perf,strace,time}.tsv`, per-case `.obs` dumps |
| Privileged diagnostics | `attribution/diag/perf{ku,k,rec,sched}-*.{log,jsonl,data,report.txt,comm.txt,timehist*.txt}` |

## 2. Evidence layout and volume

**The table below counts top-level files per directory only.** The 55 pair
directories (30 JSONL + checks/logs each) are recursive and are counted in §2.1,
not here. Recursive totals were verified with `find … -type f | wc -l` and
`find … -printf '%s\n' | awk`.

| Path (relative to evidence root) | Top-level files | Bytes | Content |
|---|---:|---:|---|
| `environment.md`, 3 top-level patches | 4 | 97,433 | freeze, K2-a/K4-b/observe patches |
| `ab/K2-a/` | 16 | 638,214 | final K2-a A/B summaries, census, oracle gate, mutation receipts |
| `ab/K4-b/` | 16 | 628,440 | final K4-b A/B summaries, census, oracle gate, patch verification |
| `ab/combo-K4-b…K1(a+b)…/` | 18 | 361,678 | combo summary, increment summary, census, merge provenance, mutant status |
| `K2-a-mutation/` | 25 | 19,298 | base/mutant/restored logs and binary hashes |
| `K4-b-mutation/` | 28 | 23,158 | same, six mutants |
| `attribution/` | 5 | 21,566 | counters, reducer, census table |
| `attribution/census/` | 33 | 1,520,055 | 10-case counter census + trace |
| `attribution/diag/` | 35 | 9,465,332 | sudo perf/tracepoint/record/sched + strace |
| `attribution/overhead/` | 105 | 630,989 | instrumentation overhead arms |
| `baseline/` (top) | 8 | 99,527 | summary, manifest, env survey, scripts |
| `baseline/attribution/` | 96 | 169,170 | Phase B time/perf/strace arms |
| `baseline/probes/` | 6 | 23,362 | PMU availability + io_uring capability probes |
| `baseline/r0…r3`, `r-diag`, `r-disk`, `r-uring-core64`, `smoke` | 20 | 260,001 | G0 matrix stages, A/A, real-fs, C>D diagnostic |
| `baseline/disk-fixture/` | 10 | 1,353,326,592 | 64 MiB fixtures on btrfs (data, not results) |
| `baseline/patches/` | 1 | 1,018 | C>D diagnostic bench patch |
| `discrimination/k1/` | 51 | 27,922,599 | K1 arm: patch, pairs, futex, sched `.data`, strace |
| `discrimination/k2/` | 59 | 995,302 | K2/E18 arm: patch, pairs, micro probe, perf record |
| `discrimination/k3/` | 21 | 31,906 | K3 fusion arm: patch, pairs, mutation logs |
| `discrimination/k4/` | 60 | 167,098 | early K4 defer arm: patch, pairs, strace, census |

### 2.1 Recursive totals for the measured arms

| Path | Files (recursive) | Bytes |
|---|---:|---:|
| `ab/K2-a/` | 1,665 | 6,563,891 |
| `ab/K4-b/` | 1,713 | 6,598,917 |
| `ab/combo-…/` | 913 | 3,558,813 |
| `discrimination/k1/` | 411 | 29,568,902 |
| `discrimination/k2/` | 599 | 3,426,931 |
| `discrimination/k3/` | 381 | 1,627,089 |
| `discrimination/k4/` | 780 | 3,428,334 |
| `attribution/` | 178 | 11,637,942 |
| `baseline/` | 173 | 1,356,395,884 |
| `K2-a-mutation/`, `K4-b-mutation/` | 25 + 28 | 19,298 + 23,158 |
| **Whole tree** | **6,870** | **1,422,946,592 (1.33 GiB)** |

Large non-result artifacts are called out: `baseline/disk-fixture` (1.35 GB of
input fixtures), `perf*.data` (≈7.5 MB each), and one 11 MB strace text.

## 3. SHA-256 manifest — orientation and summaries

Paths are relative to `/home/jnhu/sluice-593-research`.

| Artifact | SHA-256 |
|---|---|
| `brief.md` | `c750a314807934b696d9e109f7545da7f62115c6ff58647dba59b587303bc198` |
| `mapping/synthesis.md` | `fb85cbd676aabe927b06190e407cb21e6e6c06540f5244279fb1b402323d7efc` |
| `mapping/key-path-cost-ledger.md` | `d6a0234a03a22a07dca62053fa4283635619d9a659df8f4a19dd73c099e4c709` |
| `mapping/k1.md` | `6d3c7e2cd70aa4fd02bb878c3ad958bb257463add8da60bef9a3383b177df953` |
| `mapping/k2.md` | `d265874f2560d35a7e7d9e66a00bab8abe4e2ed9e423b4c3d04c5df77a07d345` |
| `mapping/k3.md` | `09d6bb8e5d5b0071f78a09ff53e66a14bc3fc58a249d99e08614001a57e1643b` |
| `mapping/k4.md` | `c925afc533777b08e1b9cd325cf077ab1a7c51e826fba380110f3a126e96b2fd` |
| `evidence/environment.md` | `d0b7218fe2feb8e456ae85df5fe661998c58a0c4c8231275c0098cb574cdc393` |

## 4. SHA-256 manifest — patches

| Artifact | SHA-256 |
|---|---|
| `evidence/K2-a-patch.diff` | `f31b46cb369761e0e4651868124483bbe64d68619e23ded8347c042317ab0226` |
| `evidence/K4-b-patch.diff` | `4e9018d82ccf68be80a318a3ce4a562e728261b97e33fa4774e34c2a7511eef9` |
| `evidence/observe-patch.diff` | `6f6d4688952c4402250616efcf8ccfea77e2819ae61f1dbb4bed5dc33b1e5473` |
| `evidence/baseline/patches/bench-w02-uring-core64.patch` | `03cb4954e15fae79ac802740b5ff1d8d3bfc3d88bf5c98bd7260638b221152b2` |
| `discrimination/k1/k1-elision.patch` | `3cdc4d88813f873c028aa841ca4b1b134b8170037e00ecd7a8d17b74ca6baa78` |
| `discrimination/k1/k1-elision-observe.patch` | `fd76e6ea041428ed0feed671eb06414a4d8581c5f48c0fd0def2c89798e08022` |
| `discrimination/k2/k2-e18-arm.patch` | `a488b2615cf16229a4d619026a2942ba77aa57b3c796a4327da2cdc88af2cfe0` |
| `discrimination/k2/k2-e18-test-repins.patch` | `98efb96285337439460e1af94d352c12f6fddbb51686b3d3be864371fc85895e` |
| `discrimination/k3/k3-fusion-arm.patch` | `b9e4ba44f3bfcf456a12ddef1fb7786aecbf515cf7d8493902ea96e0ade84930` |
| `discrimination/k4/k4-defer-arm.patch` | `f986eb861e32431b5ae91ae585c0861872e48faaa3ae40053bdc3026ae513337` |
| `discrimination/k4/k4-defer-test-repins.patch` | `915d443371fa19527c469731c0d947e38ff3d76959585c499094a3546429a8be` |
| `discrimination/k4/k4-defer-observe.patch` | `e5bd52895657d81de838d29ab17e85cfb349206e9899a9a0bb045fffe99e32c0` |

Committed copies of these patches and the combo patch are under
`bench/research/key-path-593/patches/`; their hashes are in
`bench/research/key-path-593/patches/SHA256SUMS`. The combo patch
(`combo-k4b-k1ab.diff`, 1,103 lines,
`37f1f33d579ebff5ddd71953729b4d8e69fcf87b536af906d645145d6c8d499b`) was
generated from the surviving merge worktree in this session and has no
pre-existing evidence-file counterpart; the merge provenance and the rejected
hunk are archived with it.

## 5. SHA-256 manifest — results and identities

| Artifact | SHA-256 |
|---|---|
| `ab/K2-a/README.md` | `61270a1417536258633707c53bf822959972046bbab71f71b1992a4fcf57a694` |
| `ab/K2-a/case-summary.tsv` | `8cc4a1ed6924295e183e129cb1eca1ecd597271de7cf78e8acc8ad29df1538de` |
| `ab/K2-a/pair-results.txt` | `7d7da9a19204cb81adf1a25dfe798d66c37dd492e25b42273e80d46f2130efc6` |
| `ab/K2-a/pair-results.json` | `17525037e2cf9e8f44ac60daeaa5e322221463c462f887759b2449ed7fc69dc6` |
| `ab/K2-a/census/census-summary.txt` | `7a40e195cf9f21907001e82a1b8d2ebb127fa07a430b0f6346adc8f4c43d605f` |
| `ab/K2-a/identity.txt` | `dfd5c1e448c9d84e2e21633d8f3dd5fa44b5541154e8fa30e9184b96d567e30f` |
| `ab/K2-a/oracle-gate.txt` | `9246cbf8c78277a2c7a3c46ce6cf8c14f30c48b1753fa10c29b23994b7b679d6` |
| `ab/K2-a/mutation-receipts-verified.txt` | `e9fce37de74a19eb1dfe01323b030e2bf59f8f4544639f0e71d935107fb0e518` |
| `ab/K4-b/README.md` | `6c35984f58cb88be9f86f33a0c973414438cd8793cd84fea9c2dbed5701d868c` |
| `ab/K4-b/case-summary.tsv` | `2f2592ad08f18fce7bcb1c5f6f18d3dbe14fcf12007f3c82c3b0c45274bac0a3` |
| `ab/K4-b/pair-results.txt` | `92448a0f7cecade86ad8b4388e776b6a4dcfeb0f101d6d0e6ff4add83d8df867` |
| `ab/K4-b/pair-results.json` | `464983914de7e434d47121fad639d03c6d49c66562b5464423f4c8e3196818eb` |
| `ab/K4-b/census/census-summary.txt` | `422ed6ff00898938636926459be18ec486d7308387d169dacb73d03e18c20fab` |
| `ab/K4-b/identity.txt` | `d1b995603df2f1ee51ebe2ee0acb68891fd595d6504bfdcdadba6336525beb18` |
| `ab/K4-b/oracle-gate.txt` | `4ab7b0671f0966ec5eaf1a18e5addb970c59a2c649077c83f9c8a8a46c097709` |
| `ab/K4-b/patch-verification.txt` | `41e8642a132f184bf99cd92812e7730f25b303293f9ff9874a7844908219e81c` |
| `ab/K4-b/mutation-receipts-verified.txt` | `18c43f6ec37826926337227b877a851e931b92e871f8e186b96440876f109a78` |
| `ab/combo-…/README.md` | `d0fa126c26e77d198440907114331c1bf61a64d6eb5c5d226833d3c6ab3093a7` |
| `ab/combo-…/case-summary.tsv` | `a5f6a76ae5b62906818dafe7e1fbe62446b7ebef0962232adbea5e82c2325329` |
| `ab/combo-…/pair-results.txt` | `0a249b503d9d58e537036420143e430271274fa7d3160ee6d7428a81f9412277` |
| `ab/combo-…/pair-results.json` | `69198c8bf11ec483d70523b0d5e87745ee78755c86bf653a3c74a0be72b43318` |
| `ab/combo-…/pair-results-with-increment.txt` | `e1db6c9b6bd04b6b434b1bf0a9096cfc19ba40f6f75ddee321f45fe599457cc6` |
| `ab/combo-…/census/census-summary.txt` | `c243054d3c25e730b8954f90c17a7d7170853ddb26ef05a5ab30379dde3ed9f9` |
| `ab/combo-…/identity.txt` | `39b583ab636f6af1c17cbf6f747ec2a4859176c55dc57196216d040a44431cfe` |
| `ab/combo-…/oracle-gate.txt` | `04c1900d019bbd017972d01b5c45558ef1eeb5fba9d77fbe3ec944ab329abebf` |
| `ab/combo-…/merge-provenance.txt` | `0d143633c4bee3b8b13256b92284bb645a842070a75ae09dd72b103d911e8d09` |
| `ab/combo-…/mutants/status.txt` | `1d16eaacf686bd6d4db8d123d5a476ff9aebcbf89b85676e19ca57c107535eb4` |

`ab/combo-…` abbreviates
`ab/combo-K4-b (topology+depth-gated deferred flush)+K1(a+b) merged (publication snapshot + no-observer delivery gate)`.

## 6. SHA-256 manifest — discrimination, attribution, baseline

| Artifact | SHA-256 |
|---|---|
| `discrimination/k1/README.md` | `4871d1ba52aa5a5808cb546ccceabe95aac4f65a667cb7834574394b3dc908a5` |
| `discrimination/k1/pair-results.txt` | `fda8ab8e16246e9ed204d3020a1b4df180d4e81ac273683735c1fcf747f73b83` |
| `discrimination/k1/elision-binary-sha256.txt` | `c5162b7fe2629e42a40a74505d3a654562157b303c4563cfa7b20d9abfb33a6b` |
| `discrimination/k1/placement-freeze.txt` | `8a84bea05bcb0a9359f22b5023ffd4d83aa55bfc29ef4faf89450099c773c462` |
| `discrimination/k2/README.md` | `d33bda6177783af4ee9d7b8267b266d67cfb7ad0d3bfa7eea68a802713e694c0` |
| `discrimination/k2/pair-results.txt` | `23ff7202f913a5df9ab3ac1d95761fdb5320bd690d7e834a5f39ed522be1c97e` |
| `discrimination/k2/e18-binary-sha256.txt` | `2426db7f82e9896bbe86e53947c5c1165cf98d42242560dd280fcffc2716f3cc` |
| `discrimination/k2/micro-core-cycles.txt` | `2beaeb17833afbb401981be21dd9657614a32db3df90c20409d44622082faadf` |
| `discrimination/k3/README.md` | `210f8306dae3844994ba2b408a5a7cac03e48d5ce43736ff6ff844d088207145` |
| `discrimination/k3/pair-results.txt` | `0a812821c8b2c99b9ef5a31f3b2ec8879792b1ebb268e01abd64441a8298bdf1` |
| `discrimination/k3/fusion-binary-sha256.txt` | `bf3f579018064abbc65eadf4f043e71357b0ef653ba73dbc2cf8a4efd4b339a3` |
| `discrimination/k4/README.md` | `b73895a91ea25b6950348c3a74000e4c9cc333789ffdd4ac48b0730334246205` |
| `discrimination/k4/pair-results.txt` | `56b6c247de61d57e931cc70493d583181244f0071544d1c653b4234dd287d958` |
| `discrimination/k4/k4-binary-sha256.txt` | `7ab33ca0307b5398051f7ee56148e51e07a9b549f66f9e0439b863f18c83aff3` |
| `K2-a-mutation/README.md` | `0726968027705c954f688080862e60d1a2800a10258e857c00e77f9e09db86b6` |
| `K2-a-mutation/binary-sha256.txt` | `4691dd7041ca82793ac8fe6ce13045f9c66c121cd8549632efb680c749bf4156` |
| `K2-a-mutation/test-binary-sha256.txt` | `a524bb1e125a71c3a08e3a45f4ef1abd3b4a8d7655fb37d8c4a6aa44bde25aea` |
| `K4-b-mutation/README.md` | `4031e50f3b00d22baf522a4b1f4d7fe9033d537213d502966c0a442617d1dfab` |
| `K4-b-mutation/binary-sha256.txt` | `8d4808ae4da8936241cd2815cfdc56bfb425de0f93757ceb384437a6adbcb63a` |
| `K4-b-mutation/test-binary-sha256.txt` | `7e166a7aa38a642ec4fa75e7ef5ecff77cfb4e106b5eb856b99e9e98a9315370` |
| `attribution/README.md` | `88ce7ac42dbdb90b332fb94c71b8f2c9af48005f0dae3b533e7b7b9c8fe0b116` |
| `attribution/census-table.txt` | `e8418aaa5103e7eb37e2aea17abc1d94bbbfa65631267b39259f05794f48d410` |
| `attribution/analyze_census.py` | `b54cc6fa99257f4b64710ff90315f2bd0427f10d24ee309a30e33c820f3ac3eb` |
| `baseline/README.md` | `cd2205d6ae351290de541e5737b5e7998824429fdb82f12d29ae388c1a746d9c` |
| `baseline/baseline-summary.md` | `779951922baa50643c02e774578ba5a9748da0597fbebeeb22cdd0ec267774b0` |
| `baseline/baseline-summary.json` | `1b1f0e9f791797fd01b4a9fb39bc50a08f8b316038d14a01c2f3405ec3cea1e9` |
| `baseline/SHA256SUMS.txt` | `3156ec13bc2ae33a14b9fb2b880b64b14a461acf14106acf066d2e7d79fa9c36` |
| `baseline/env-survey.txt` | `d4330329dc6079bfdde2462285ab409b0bb379f4957a740473567d0b3fa704a3` |

## 7. Storage and preservation disclosure

- **What is committed in this PR:** the fifteen documents in
  `docs/research/key-path-593/`, the rebuildable patches and harness in
  `bench/research/key-path-593/`, and their checksum manifests. No raw JSONL,
  no perf `.data`, no binary.
- **What is not committed:** the 1.33 GiB evidence workspace. It remains at
  `/home/jnhu/sluice-593-research/evidence` on the E5 host.
- **Preservation status: `PARTIAL`.**
  - It is a **single persistent copy** on one host. There is no replication,
    no backup guarantee, no CI artifact storage and no object store.
  - **The SHA-256 manifest in §3–§6 covers summaries, patches, identities and
    census summaries — not the raw per-rep JSONL or the perf `.data`.** For the
    raw data, only the path, file count (6,870) and total size
    (1,422,946,592 bytes) are verified. Raw-data integrity is **not**
    hash-anchored; a hash of a summary does not prove the raw records behind
    it. Anyone with access to the host can re-reduce; anyone without it can
    audit summaries and their hashes only.
  - The worktrees used to build the arms (`baseline`, `observe`, `k1-elision`,
    `k2-elision`, `K2-a`, `k3-fusion`, `k4-defer`, `K4-b`, `combo-K4-b-K1ab`)
    also live only on this host. Their HEADs are all `bdfd0d8d`, with
    uncommitted arm changes (the arms are reproduced by the archived patches).
  - The K4 discrimination session has **no archived reducer**: its
    `pair-results.txt` cannot be regenerated from the committed scripts. K1 and
    K3 reducers were copied into the harness; the K4 format remains ad hoc.
  - Two large categories are inputs, not results: `baseline/disk-fixture`
    (1.35 GB of 64 MiB fixtures) and the perf `.data` files (≈7.5 MB each).
  - The combo patch is a **regenerated** artifact from a surviving worktree;
    the original combo session did not archive a single combined patch, only
    the two member patches, the merge provenance and the rejected hunk. The
    regenerated patch is byte-stable with the tree that was measured (the
    worktree was not modified after the run), but it is a reconstruction of a
    patch, not of the run.
- **What would make this `SUFFICIENT`:** copy the evidence workspace to a
  second host or an artifact store, publish the raw JSONL compressed as a
  release asset, and hash the raw files (or a per-directory manifest). None was
  available in this run.

## 8. Worktree inventory

| Worktree | HEAD | Dirty files |
|---|---|---|
| `worktrees/baseline` | `bdfd0d8d` | 1 (C>D diagnostic bench patch) |
| `worktrees/observe` | `bdfd0d8d` | 7 |
| `worktrees/k1-elision` | `bdfd0d8d` | 10 |
| `worktrees/k2-elision` | `bdfd0d8d` | 9 |
| `worktrees/K2-a` | `bdfd0d8d` | 9 |
| `worktrees/k3-fusion` | `bdfd0d8d` | 11 |
| `worktrees/k4-defer` | `bdfd0d8d` | 11 |
| `worktrees/K4-b` | `bdfd0d8d` | 9 |
| `worktrees/combo-K4-b-K1ab` | `bdfd0d8d` | 10 (7 modified + 3 untracked tests) |
| `worktrees/pr` | branch `research/593-key-path` | clean after this record's commit; the arm worktrees stay detached at `bdfd0d8d` |

The PR worktree is the only one on a branch; the arm worktrees are detached at
the frozen base with uncommitted research changes, which the archived patches
reproduce.

## 9. Artifact → claim map

| Claim | Evidence |
|---|---|
| Baseline binaries and environment | `evidence/environment.md`, `evidence/baseline/SHA256SUMS.txt`, `baseline/env-survey.txt`, `baseline/probes/` |
| Mechanism frequency table, fixed chains | `attribution/census/census-summary.txt`, `attribution/census-table.txt`, `attribution/analyze_census.py` |
| Instrumentation overhead | `attribution/overhead/` |
| CPU anatomy, syscall counts, thread split | `attribution/diag/perfku-*`, `perfrec-*`, `perfsched-*`, `syscnt-*`, `strace-*` |
| K1 arm and result | `discrimination/k1/` (patch, pairs, futex, sched, placement) |
| K2 mechanism and upper bound | `discrimination/k2/` (E18 arm, micro probe, perf record), `ab/K2-a/` |
| K3 arm and result | `discrimination/k3/` |
| K4 gate evolution | `discrimination/k4/` (early defer arm), `ab/K4-b/` (final gate) |
| Combo | `ab/combo-…/` (summary, increment, census, merge provenance, mutant status) |
| Mutation receipts | `K2-a-mutation/`, `K4-b-mutation/`, `discrimination/k3/mut-*`, `ab/combo-…/mutants/status.txt` |
| Oracle/byte correctness per run | `ab/*/oracle-gate.txt`, per-run `.check` files |
| Binary identities | `*/binary-sha256.txt`, `*/identity.txt`, `elision-binary-sha256.txt`, `fusion-binary-sha256.txt`, `e18-binary-sha256.txt`, `k4-binary-sha256.txt` |
| Patch rebuildability | `ab/K4-b/patch-verification.txt`, `K2-a-mutation/README.md`, `ab/combo-…/merge-provenance.txt` |
