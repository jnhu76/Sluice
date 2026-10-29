# P5 S1 static dossier — four-archive exact-path recount + gates (session 2026-10-28)

Session: P5 交互测量员. Host E5, kernel 7.2.11-200.fc44.x86_64, binutils
GNU objdump 2.46.1-1.fc44. All commands executed this session on the four
release archives' extracted `blocking_file.cpp.o`:

```
sha256 (archives): B1 d1e351a5…3007f0  C1 07d06803…2049e  C2 67251028…20a807  AB 07d06803…2049e
sha256 (objects):  B1 3d84a523…d301b  C1 d5c58bb3…87a8b  C2 7c7827ae…c86e8  AB d5c58bb3…87a8b
```

**AB's `blocking_file.cpp.o` is byte-identical to C1's** (merge R1 took
P2C1's `src/blocking_file.cpp` wholesale; measured, not assumed). The AB
archive rebuilds deterministically (`07d06803…` across three matrix runs and
this session's re-run).

Counting convention: P1 audit exact-path convention — legal nonempty call,
first step full count, every conditional branch/syscall call/ret on the
path; alignment nops and byte-continuation lines excluded. Segment counts
below are machine-parsed (`objdump -dr --demangle`, mnemonic-column regex);
path sums hand-composed from the parsed segments, cross-checked against the
recorded anchor decompositions.

## Caliber note (preregistered ±1, P2C2 §2.1)

Session caliber reproduces every read-side and arc anchor exactly. On
`write_all_at` 1-step paths the P1-audit caliber counts +1 vs this session
caliber (audit B1 101 / C2 81 vs session 100 / 80) — the same endpoint-nop
class the plan's S1 item 3 already discloses for `write_at` (40 audit / 39
session). Arcs have no caliber delta (44=44, 35=35 on both sides). All
verdicts below hold in either caliber; the one failing gate fails by +2,
beyond any ±1 caliber ambiguity.

## Item 5 — recount table (1-step / per-extra-step arc)

| Path | B1 (audit anchor / session) | C1 | C2 | AB (=C1 object) | AB gate bound | Verdict |
|---|---|---|---|---|---|---|
| read_exact_at | 100 / 100 | 68 | — | **68** | ≤ 75 | PASS (−7) |
| read_exact_at arc | 44 / 44 | 25 | — | **25** | ≤ 26 | PASS (−1) |
| read_exact | 82 / 82 | 55 | — | **55** | ≤ 60 | PASS (−5) |
| read_exact arc | 34 / 34 | 21 | — | **21** | ≤ 21 | PASS (at bound — exactly C1's own §2.1 verdict) |
| write_all_at | 101 / 100 | — | 81 / 80 | **79** (audit caliber 80) | ≤ 93 | PASS (−14 session) |
| write_all_at arc | 44 / 44 | — | 35 | **39** | ≤ 37 | **FAIL (+2)** |
| write_all | 82 / 82 | — | 62 / 62 | **61** (audit caliber 62) | ≤ 77 | PASS (−16) |
| write_all arc | 34 / 34 | — | 25 | **29** | ≤ 30 | PASS (−1; 30 worst-case audit caliber still PASS) |

All 8 anchors reproduce (B1 100/44, 82/34, 101/100, 82/34; C1 68/25, 55/21;
C2 81/80, 62/25 — each within the disclosed ±1 where noted).

Segment decomposition (AB paths; addresses in the AB object):

- AB read_exact_at 1-step 68 = 序 30 (f30–fb1) + 步 13 (fc0–fed, pread@fe5)
  + merge 5 (1130–1140) + complete 物化 4 (1170–117e) + 尾 16 (107d–10e0).
  arc 25 = 短合并+回边 12 (1130–1157) + 步 13.
- AB read_exact 1-step 55 = 序 22 (15a0–15fc) + 步 9 (1600–161d, read@1615)
  + merge 5 (1780–1791) + 物化 4 (17c0–17ce) + 尾 15 (16ad–16ef).
  arc 21 = 短合并+回边 12 (1780–17a7) + 步 9.
- AB write_all_at 1-step 79 = 序 30 (1220–12a1) + 步 25 (12b0–1313,
  pwrite@1303; per-step closed/access/sign/range re-tests retained — R1)
  + merge 5 (14a0–14ac) + complete 物化 4 (14f0–14fe) + 尾 15 (13ae–13f4).
  arc 39 = 短合并+回边 14 (14a0–14c8) + 步 25. The +4 vs C2's arc 35:
  AB merge carries separate ==total (cmp/je) and loop-condition (cmp/jae)
  checks plus the confirmed-update mov inside the back-edge (A's loop shape:
  advance → complete-check → zero-progress-check → loop-condition), where
  C2's shape fuses them (35 = merge+back-edge 11 + 步 24).
- AB write_all 1-step 61 = 序 22 (1870–18c9) + 步 15 (18d0–1906, write@18f6)
  + merge 5 (1a90–1a9d) + 物化 4 (1ae0–1aee) + 尾 15 (199e–19e1).
  arc 29 = 短合并+回边 14 (1a90–1ab8) + 步 15.

## Item 1 — read-side gates (PASS)

- M1 (no re-test inside the compose step loops): 0 re-test-class
  instructions (`cmpb|movabs|jb|js` on File fields) in fc0–fed and
  1600–161d; exactly 1 pread/read call per step (relocations
  R_X86_64_PLT32 pread@fe6 / read@1616).
- M2 (state register residency): `movaps …0x20(%rsp)` = 0,
  `…0x10(%rsp)` = 0 in the whole AB object (B1: 2 and 2).
- M3 (single-point exit materialization): complete = 1170 block
  (read_exact_at) / 17c0 (read_exact); eof = 11e8 / 1838; error exits
  share the 1380/167f-class blocks; B1's dead zero-stores absent.

## Item 2 — write-side gates (FAIL on one bound)

- W-M1/W-M2 machine checks pass (no staging store + no movdqa reload on the
  AB back-edges 14a0–14c8 / 1a90–1ab8; movaps gate 0/0).
- Bounds: write_all_at 79 ≤ 93 PASS; **write_all_at arc 39 > 37 FAIL**;
  write_all 61 ≤ 77 PASS; write_all arc 29 ≤ 30 PASS.
- Per plan §4 S1: failing any bound → **INTERFERENCE_STATIC** recorded; the
  write side's stacked claims are not admitted (no wall number repairs a
  failed static bound). B's own single-factor evidence (C2 81/35, 62/25) is
  untouched — the stack's write path is A's compose, not B's.

## Item 3 — public-op machine identity (PASS)

Address-stripped instruction streams of the four public ops, AB vs B1
(sha256 of stripped streams, first 16 hex):

```
read_at   B1 762b2ae435c5c9bc (111 insns) = AB 762b2ae435c5c9bc (111)  SHA-EQUAL
read      B1 27b981d95db3c56f (100)       = AB 27b981d95db3c56f (100)  SHA-EQUAL
write_at  B1 a2e22afb94708a5f (111)       = AB a2e22afb94708a5f (111)  SHA-EQUAL
write     B1 8dba982c2e98e2a1 (100)       = AB 8dba982c2e98e2a1 (100)  SHA-EQUAL
```

Identical streams ⇒ exact-path counts ±0 (read_at 39 / read 29 /
write_at 40-audit-39-session / write 29) by structure.

## Item 4 — impossible-count guards (PASS)

Present on all four compose back-edges in the AB object:
read_exact_at 1130/1133, read_exact 1780/1783, write_all_at 14a0/14a3,
write_all 1a90/1a93 (cmp remaining,transferred + jb).

## Item 7 — mechanism-duplication audit

Changed-line counts in `src/blocking_file.cpp` (`diff | grep -c '^[<>]'`):

- B1→A(C1): 128; B1→B(C2): 85; A(C1)↔B(C2): 121; **B1→AB: 128 = B1→C1
  exactly** — the stack's production TU *is* A's end state; zero lines of
  B's textual end state survive. B's transform is subsumed semantically on
  the exit-staging axis (A's compose already materializes once per exit
  flavor with register-carried state and public write steps with per-step
  re-tests) but NOT instruction-for-instruction (C2's compose object code
  is absent; the AB write arc is +4 vs C2's).

Kit identity consequence: `ladder-ab` (kit source 2364be4a, built this
session against the stacked archive) sha256
`8305d22ca4ff74db0017a24fd7867b85f4a48ec767fbfc5d49563402ce870381` —
**byte-identical to `ladder-c1`**. The A+B arm is machine-identical to the
A arm at the library level; all matrix cells still run it as its own arm
with fresh invocations, as preregistered (§6 row 1 idempotence outcome,
recorded either way).
