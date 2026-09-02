#!/usr/bin/env python3
"""check_rbuf_e0_probe_order.py — structural regression guard for the RBUF-E0
capability probe (#272 adversarial-review remediation). Guards against
reintroducing the probe ordering race (READ_FIXED and WRITE_FIXED submitted
together with no read -> write dependency, so results could depend on
kernel-side op ordering on a foreign kernel/ARM host).

Source-level structural assertions on the run_probe region of
bench/rbuf_e0_bench.cpp:
  1. exactly one READ_FIXED prep and one WRITE_FIXED prep;
  2. strict source order: read prep -> submit -> wait_cqe -> write prep
     -> submit -> wait_cqe (the read CQE is awaited before the write SQE
     is even prepared);
  3. exactly two io_uring_submit calls in the probe (no single submit
     covering both ops);
  4. no IOSQE_IO_LINK in the probe.

With --bench PATH, additionally executes the probe and requires the JSON
capability line to report the serial protocol ran (capable == true,
write_submitted_after_read_cqe == true, exit 0).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
BENCH_SRC = REPO / "bench" / "rbuf_e0_bench.cpp"
REGION_START = "int run_probe(const Config& cfg) {"
REGION_END = "// ---- formal run"


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def probe_region(src: str) -> str:
    start = src.find(REGION_START)
    end = src.find(REGION_END, start)
    if start < 0 or end < 0 or end <= start:
        fail("cannot locate run_probe region in bench source")
    return src[start:end]


def positions(region: str, needle: str) -> list[int]:
    out = []
    at = region.find(needle)
    while at >= 0:
        out.append(at)
        at = region.find(needle, at + 1)
    return out


def check_source() -> None:
    region = probe_region(BENCH_SRC.read_text())
    reads = positions(region, "io_uring_prep_read_fixed(")
    writes = positions(region, "io_uring_prep_write_fixed(")
    # Call sites are namespace-qualified in this file; unqualified matches
    # would also count the rbuf_fatal("...") string literals.
    submits = positions(region, "::io_uring_submit(")
    waits = positions(region, "::io_uring_wait_cqe(")
    if len(reads) != 1:
        fail(f"expected exactly 1 READ_FIXED prep in probe, found "
             f"{len(reads)}")
    if len(writes) != 1:
        fail(f"expected exactly 1 WRITE_FIXED prep in probe, found "
             f"{len(writes)}")
    if len(submits) != 2:
        fail(f"expected exactly 2 io_uring_submit calls in probe (one per "
             f"phase), found {len(submits)} — a single submit covering both "
             f"ops is the ordering race this check guards")
    if len(waits) != 2:
        fail(f"expected exactly 2 io_uring_wait_cqe calls in probe, found "
             f"{len(waits)}")
    order = [reads[0], submits[0], waits[0], writes[0], submits[1], waits[1]]
    if order != sorted(order) or len(set(order)) != 6:
        fail(f"probe ordering broken — required source order read-prep < "
             f"submit < wait < write-prep < submit < wait, got {order}")
    if "IOSQE_IO_LINK" in region:
        fail("probe must not use IOSQE_IO_LINK (serial by submission "
             "structure, not by linkage)")
    print("PASS probe-order-source: run_probe is strictly serial — READ_FIXED "
          "prep -> submit -> wait_cqe -> WRITE_FIXED prep -> submit -> "
          "wait_cqe; 2 submits, no IOSQE_IO_LINK")


def check_runtime(bench: Path) -> None:
    if not bench.is_file():
        fail(f"bench binary not found: {bench}")
    with tempfile.TemporaryDirectory() as td:
        src = str(Path(td) / "probe-src")
        dst = str(Path(td) / "probe-dst")
        p = subprocess.run([str(bench), "--probe", "--src", src, "--dst",
                            dst], capture_output=True, text=True)
    if p.returncode != 0:
        fail(f"probe exited {p.returncode}: {p.stdout} {p.stderr}")
    try:
        cap = json.loads(p.stdout.strip().splitlines()[-1])
    except Exception:
        fail(f"probe JSON unparseable: {p.stdout!r}")
    if not cap.get("capable"):
        fail(f"probe reports not capable: {cap}")
    if not cap.get("write_submitted_after_read_cqe"):
        fail("probe JSON lacks write_submitted_after_read_cqe=true — "
             "rebuild the bench from the remediated source")
    print("PASS probe-order-runtime: executed probe reports capable=true, "
          "write_submitted_after_read_cqe=true")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", type=Path, default=None,
                    help="also execute this probe binary (runtime check)")
    args = ap.parse_args()
    check_source()
    if args.bench:
        check_runtime(args.bench)
    print("check_rbuf_e0_probe_order: ALL CHECKS PASS")


if __name__ == "__main__":
    main()
