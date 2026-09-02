# RE-H0 — native semantic-floor residual and performance-envelope closure

Campaign issue: **#277** (execution authority #227). Host-0 RE performance
lane: what does the explicit Sluice boundary cost on the one available
native host, after already-known implementation taxes are removed?

```text
RE-0H-H0  environment qualification          RE-H0-AUDIT.md §6
RE-1-H0   ThreadPool ladder L0/L1/L2         RE-H0-REPORT.md (RE-1)
RE-1U-H0  semantic-floor ladder Z1..Z3       RE-H0-REPORT.md (RE-1U)
RE-2-H0   initial value envelope             RE-H0-REPORT.md (RE-2)
G1-PERFORMANCE (Host-0) adjudication         RE-H0-REPORT.md
```

## Layout

```text
RE-H0-AUDIT.md             as-built instrument + production audit (pre-freeze)
RE-H0-PREREGISTRATION.md   frozen protocol (thresholds, cells, stop law)
RE-H0-REPORT.md            results + Host-0 G1 adjudication
scripts/re_h0.py           session runner (no retries; fail-closed)
scripts/re_h0_analysis.py  mechanically recomputable analysis (P13 authority)
scripts/check_re_h0_analysis.py  26 analysis diagnostics (TDD, red first)
scripts/plot_re_h0.py      SVG plots from summary/analysis JSON
results/<session>/         immutable sessions (O(10) files):
                           environment/manifest/gates/runs.jsonl/
                           summary/analysis
plots/                     committed SVGs
```

## Instruments (UNCHANGED research binaries)

- `tax0_z_ladder_bench` (TAX-0B frozen harness): Z1 raw liburing,
  Z1b semantic-equivalent floor, Z1bw +one continuation, Z2
  AsyncIoContext+UringAsyncBackend, Z3 ApplicationRuntime.
- `e1_abstraction_tax_bench` (E1 frozen harness): L0 raw blocking,
  L1 competent fixed pool, L2 Sluice ThreadPoolBackend path.

## Decomposition authority (never violate)

```text
C_sem     = Z1b/Z1      capability cost
T_backend = Z2/Z1b      backend abstraction tax
C_cont    = Z1bw/Z1b    continuation cost
T_runtime = Z3/Z1bw     runtime/mediation tax (matched rep-envelope)
T_pool    = L1/L0       fixed-pool execution cost
T_sluice  = L2/L1       Sluice pool incremental
```

`Z1 -> anything` is NOT a tax measure.

## Hard boundaries

- HOST-0 ONLY (native Fedora 44, SATA SSD btrfs primary, tmpfs control).
- No cross-host / modern-NVMe / ARM claim; #270 stays OPEN / NOT EXECUTED.
- No production optimization authorized. Attribution is census-first;
  any candidate selection stops before production (human adversarial
  review gate, #255 discipline).
- #262 stop law: pre-measurement gate ran 160/160 clean; any formal-cell
  surprise ⇒ CELL INVALID / CAMPAIGN PAUSE, no retries.

## Reproduce

```sh
xmake f -m release --toolchain=clang --with-liburing=true -y
xmake build -r tax0_z_ladder_bench e1_abstraction_tax_bench
python3 research/re-h0/scripts/check_re_h0_analysis.py
python3 research/re-h0/scripts/re_h0.py qual262
python3 research/re-h0/scripts/re_h0.py re1u
python3 research/re-h0/scripts/re_h0.py re1
python3 research/re-h0/scripts/re_h0.py re2
python3 research/re-h0/scripts/re_h0.py analyze --session <id>
python3 research/re-h0/scripts/plot_re_h0.py --session <id>...
```
