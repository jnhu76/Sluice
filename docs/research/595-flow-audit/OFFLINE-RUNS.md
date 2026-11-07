# Offline re-run record (repair round, 2026-11-07)

All commands run offline over the frozen r5-repair evidence;
no server program, no timing. Python 3.14, `python3 -I`.

## Commands and exit codes

| command | exit | result |
|---|---|---|
| `python3 -I scripts/verify_inherited_evidence.py --json ...` | 0 | 60/60 checks; arm means match round-analysis.json at raw float precision (max rel diff 0.0, 75 values); flat-mean equivalence demonstrated per arm; F5 exact sets/totals/integer tails + frozen recovery rerun reproduces derived summary |
| `python3 -I scripts/cpu_scope_recalc.py --json ...` | 0 | 0 problems; primary per frozen definition cross-checked vs harness metrics; three windows separated; io-wq rusage inclusion UNRESOLVED |
| `python3 -I scripts/ledger_csv.py build` (then `check`) | 0 | 32 rows, 0 problems; v1 CSV had 42 problems incl. 11 misaligned rows |
| `python3 -I scripts/test_offline_checks.py` | 0 | 30/30 tests (admission/F5/CPU/CSV negatives + hand-computed positives + mermaid structure) |

## Input identities

| input | sha256 |
|---|---|
| results.jsonl MAIN-W03-D16 | `426f5508a1666528a89c78f09f321ba0fb5f4889b4a598ace4cd7d9841294d06` |
| results.jsonl PROT-W03-D4 | `c6d7420f1ffb222080447717ecfe18f2b963648f5b53dbf58e59de86115822b5` |
| results.jsonl PROT-W03-D2SERIAL | `8e2e7884f940ea54d5a1f09abb41c4ef7c1b4da94d6d22ff02a5d7d1d15d6f64` |
| results.jsonl PROT-W02-D1 | `f17b1a1b64e2352694fc3ce6d13a432f27d9171d2110b9dabd1eb89042bba7b7` |
| results.jsonl PROT-W02-D2 | `8bfeb1cbd3b86dddb9ea2efc8e6b24c1fd9a89ce6f4a5482d261a1a9f4b50e9a` |
| diag-ops.csv | `3be1509dc82d8139684d3c11288396a401e287e70399ba1dc7bb2bfeb76d79a3` |
| f5-position.json (frozen derived) | `fdf0f113f8ed15189c18959b4a59c891b6abc5ff0bda8fcadcb559f4bda1fa63` |
| recover_f5_position.py (frozen) | `77848fc752e46a56e4eb89e3513724219b54ec08d3726cb7f0002da024403483` |
| measure3.py (BINS source) | `8b4f65c958c4f6af5cbd1fb12087cba61bf0d649a88af0bedbfea63fc179dcd6` |

Machine summaries: verifier `cells` map above; MAIN p99 run-level
mean diffs C-A0 = 9.524 us, R-A0 = 10.707 us; per-block relative
effect means +17.97%/+20.14% (frozen contrasts reproduced); pooled
per-request p99 UNAVAILABLE (per-rep percentiles only).

## Second-round reruns (review round 2, R7-R10, 2026-11-07)

Inputs identical to the table above (frozen evidence untouched).

Pre-fix reproduction of the two reviewer counterexamples (inline
`python3 -I` heredocs against 461cb267, both exits 0):

- `round-analysis.json` replaced by `[]` -> `compare_frozen` printed its
  unconditional success line ("... over 5 cells x 3 arms x 5 metrics
  (75 values)") with `Report.ok = True` while 0 values were compared.
- `f5_summary_checks(make_f5())` -> `Report.ok = True` with position-bucket
  sums 16800 (d16) / 200000 (d4) = 216800 total vs validation totals
  480000 and clean counters.

| command | exit | result |
|---|---|---|
| `python3 -I scripts/verify_inherited_evidence.py --json ...` | 0 | 162 checks PASS / 0 FAIL (60 prior + 102 new closure/coverage checks); frozen compare now reports 75/75 comparisons actually performed; F5 closure d16 residual 17 (bound 2584), d4 residual 7 (bound 190) |
| `python3 -I scripts/cpu_scope_recalc.py --json ...` | 0 | 0 problems; `notes.primary_scope` now states io-wq inclusion UNRESOLVED (R9), no retracted exclusion claim left in machine output |
| `python3 -I scripts/test_offline_checks.py` | 0 | 37/37 tests (30 prior + second-round negatives: empty round-analysis.json, missing (arm,metric) coverage, non-closing bucket denominators, sub-MIN_N bucket, groupless tag) |
