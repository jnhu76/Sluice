# g1-control-c0-native-1 — notes

Authored after the session (conventions: research/rbuf-e0).

## Session timeline

1. `probe` — capability preflight: capable=true, features 0x3FFFF
   (IORING_FEAT_RSRC_TAGS present), memlock 8 MiB (not a file-registration
   factor), perf instructions:u OK. FILE-ID-E0 + replacement-window
   witnesses recorded (raw/fileid.json, raw/replacement-window.json).
2. `generate` — fixtures for tmpfs + btrfs at 512 MiB (4K cells) and
   1 GiB (64K/2M cells); C++ generator cross-validated against the Python
   generator (src sha256 8a3c4bf0… for 1 GiB — byte-identical to the
   CHUNK-E0/RBUF-E0 canonical fixture); expected dst pattern hashes frozen
   into manifest.json.
3. `q0` — 30/30 PASS (READ 4K d8 tmpfs F0), 0 gate errors -> qualified
   (raw/q0.json; #262 NOT closed).
4. `formal` — 560 runs (2 ops x 5 cells x 2 fs x 4 arms x 7 seeded-
   interleaved rounds), 0 gate errors.
5. `summarize` — first execution CONTAMINATED: q0 runs share the READ 4K
   d8 tmpfs F0 cell signature and were not yet excluded; that run printed
   a transient BENEFIT ESTABLISHED verdict. The fail-closed validator
   flagged the violation (37 valid runs in one cell, expected 7) before
   any verdict was used; driver+validator fixed to exclude q0-* ids
   (validator ratio re-check rounding tolerance 1e-6 -> 5e-5); clean
   re-run produced the final verdicts (NOT ESTABLISHED, READ and WRITE).
   Frozen rules/matrix/hypotheses untouched. Full disclosure in the
   report.

## Verdicts

- C0-PERF: FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED (READ, WRITE)
- C0-IDENTITY: ORDINARY-FD WRONG-TARGET REPRODUCED /
  FIXED L0 BINDING PRESERVED TARGET (+ replacement honored;
  BOUNDARY-A window confirmed; BOUNDARY-D retention confirmed)
- C0-MINIMALITY: L0 SUFFICIENT; generation NOT EARNED;
  per-request live-use NOT EARNED; replacement NO CURRENT REQUIREMENT
- Classification: C-PARTIAL (product capability only; G1-Control NOT
  proven); C1 DO NOT PROMOTE

## Environment deltas observed during the session

- None (governor/limits/fs unchanged; no ulimit/sysctl touched).
