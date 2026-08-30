# TAX-0B EXP-0 Diagnostics (non-canonical)

Supplementary diagnostic captures for the EXP-0 capacity-invariance
matrices (#250 TAX-0 ladder / PR #253). NOT canonical evidence artifacts:
single runs, captured in separate invocations after the canonical
matrices, excluded from `perf-evidence-validate.py`'s artifact glob by
living in this subdirectory with non-JSON extensions. The canonical
evidence is the five `tax0b-exp0-*.json` artifacts in the parent
directory.

Host/tooling: same host, build (freeze `77063e5`, binary sha256
`896995e1…`), and taskset placement (`0,2,4,6`) as the canonical
artifacts; `perf 7.1.9` under sudo with kernel-inclusive counting
(paranoia layer bypassed by root — this is the layer TAX-0A FC-7 called
`HOST_WITH_SUDO`).

## Kernel-inclusive perf stat (one process per cell, 256 MiB / 65,536 ops)

Files `threadpool-C8.txt` / `threadpool-C512.txt` / `uring-C8.txt` /
`uring-C512.txt` (tmpfs data file; exact invocation in the report §5):

| cell | task-clock | context-switches | cpu-migrations | cycles (u+k) | instructions (u+k) |
|---|---|---|---|---|---|
| threadpool C=8   | 352.46 msec | 6,719  | 0 | 1,008,658,520 | 535,571,834 |
| threadpool C=512 | 357.19 msec | 7,910  | 0 |   959,355,102 | 533,248,002 |
| uring C=8        | 301.47 msec | 15,652 | 2 |   834,722,149 | 662,094,373 |
| uring C=512      | 368.55 msec | 16,132 | 2 |   880,788,978 | 860,789,735 |

Readings used by the report (direction only, no statistics on n=1):

- Kernel-inclusive instruction slope for the uring arm:
  (860,789,735 − 662,094,373) / 65,536 / (512 − 8) ≈ **6.02 instr/op/C**,
  matching the user-mode official slope (+5.994) — the capacity tax is
  entirely user-space; kernel instruction count is capacity-flat.
- ThreadPool totals are capacity-flat kernel+user (533-536M instr).
- context-switches and cpu-migrations do not scale with C (the tax is
  not scheduling churn); taskset held migrations at ≤2.

## Not collected here

- **Flame graphs**: the Release binaries are stripped and frame-pointer
  free (`readelf` shows no `.symtab`), so dwarf unwinding symbolizes to
  raw addresses. TAX-0A §17 preregisters flame attribution for EXP-2
  with a one-off diagnostic symbol build ("diagnostic builds are not
  committed as evidence") — deliberately out of EXP-0 scope.
- **bpftrace**: not installed on this host; the perf counter layer above
  covers the scheduling/syscall-free corroboration EXP-0 needed.
