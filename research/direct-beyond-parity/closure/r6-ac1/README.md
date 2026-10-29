# R6/AC-1 experiment directory — positional vectored I/O (#605)

Summary: `../r6-ac1.md` (start there). Documents: `SEMANTICS.md`,
`ERROR_MATRIX.md`, `E5_ORACLE.md`, `API_DRAFT.md`,
`OWNER_DECISION_PACKET.md`.

## Layout

- `probes/r6ac1_probe.c` — the independent semantic probe (13 cells, raw
  syscalls, no Sluice headers, machine-readable `obs key=value` output).
- `probes/run_all.sh` — rewrites every file in `raw/` (tmpfs cells, btrfs
  spot-checks, stability repeats).
- `raw/` — archived outputs of the session that produced the documents,
  plus `env-pin.txt` (host identity, probe source/binary sha256, scratch
  filesystem identities, base commit).

## Reproduce

```sh
cd probes
cc -O2 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -o r6ac1_probe r6ac1_probe.c -lpthread
zsh run_all.sh
```

Single cells: `./r6ac1_probe <cell> [scratch_dir]` — cells: `env`,
`array-order`, `short-read`, `iov-fault-read`, `iov-fault-write`,
`iovmax`, `sum-overflow`, `null-empty`, `fsize-partial-write`, `eintr`,
`cursor`, `atomicity`, `nonseekable`, `all`.

Scratch: `/tmp/r6-ac1-scratch` (tmpfs, default) and
`/var/tmp/r6-ac1-scratch-btrfs` (btrfs, passed as the second argument by
`run_all.sh`). Fixtures are (re)created per cell; nothing outside the two
scratch dirs is touched.

## Session identity

See `raw/env-pin.txt`: host E5, kernel 7.2.11-200.fc44.x86_64, glibc 2.43,
`IOV_MAX` 1024, probe source sha256 `9e364ff7…cf56a9`, binary sha256
`ccb0334c…62a94` (reproduced bit-identical from source), base commit
`bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` (master).
