# FULL_ARCHIVE.md — Layer A record (full E5 evidence)

This archive (Layer B, in Git) holds decision-complete data. The **full
execution archive** (Layer A) lives on the E5 host and retains every produced
byte.

## Identity

| Item | Value |
|---|---|
| Root | `/home/jnhu/sluice-595-e5/` (evidence tree `evidence/`, harness `harness/`, research worktree `worktree/`) |
| Manifest | `MANIFEST.sha256` (verbatim copy in this directory), 1,859 data entries = 1,853 files under `evidence/` + 3 harness files + 3 release binaries |
| Manifest SHA256 | `dfdb447119c2af5ade4a401a21283bfb7b1eb733f27bae61f3be9cd684fe27e3` |
| Verification | `sha256sum -c MANIFEST.sha256` → 1,859/1,859 pass (2026-11-04, archival audit) |
| Backup tarball | `/home/jnhu/sluice-595-e5/backup/sluice-595-e5-full-evidence-20261104.tar.gz` |
| Tarball SHA256 | `c384c910116a3b2022c9871b229ad0aacfaebf6cb124228fe7d5f8c856dbdeb4` |
| Tarball contents | 1,860 files: the 1,853 `evidence/` files + `MANIFEST.sha256` + 3 harness files + 3 release binaries |
| Backup verification | extracted to a clean dir and checked against the embedded manifest — all entries pass |

## Status flags

- `FULL_EVIDENCE_BACKUP`: present, extract-verified (see hashes above).
- `FULL_EVIDENCE_SINGLE_COPY`: **NO** — a verified backup copy exists.
- **Locality caveat**: original and backup are on the same host and filesystem
  (`/home`). This is redundancy against file corruption, **not** off-host
  durability. The evidence must not be described as durably persisted until an
  off-host copy exists.

## What only exists in Layer A

Per-invocation bench JSONL and stdout logs for all 23 measurement phases
(`raw-minimal/EXCLUDED.md`), the platform freeze `platform.txt`, the boundary
map `boundary-map.md`, the round report `report-595-e5.md`, the pre-archive
comment draft `issue-595-comment.md`, and the three release binaries
(reproducible from `patch.diff` + `build-notes.md`; hashes recorded in both).
