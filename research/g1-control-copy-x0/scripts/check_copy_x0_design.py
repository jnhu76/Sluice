#!/usr/bin/env python3
"""COPY-X0 M6 structural design gate (prereg §13 M6).

Scans the research harness for forbidden generic-framework identifiers and
asserts the research-only banner. FAILS on any hit. --self-test proves the
gate fires on synthetic inflated text.
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]

FORBIDDEN = [
    "CapabilityRegistry", "TransferManager", "ResourceManager", "DataToken",
    "StrategyPlanner", "AutoTuner", "auto_tuner", "capability_registry",
    "transfer_manager", "resource_manager",
]

SCAN = [
    REPO / "bench" / "g1_control_copy_x0_bench.cpp",
    REPO / "research" / "g1-control-copy-x0" / "scripts" / "run_copy_x0.py",
    REPO / "research" / "g1-control-copy-x0" / "scripts" / "validate_copy_x0.py",
]

REQUIRED_BENCH_MARKERS = [
    "RESEARCH ONLY",
    "COPY-X0-PREREGISTRATION.md",
]


def scan_text(text: str) -> list[str]:
    # The validator itself names the forbidden tokens (to reject them); its
    # own FORBIDDEN list is not an inflation hit — scans below skip list
    # definitions by checking this marker comment convention instead: hits
    # only count outside lines that declare the forbidden vocabulary.
    hits = []
    for i, line in enumerate(text.splitlines(), 1):
        if "FORBIDDEN" in line or "forbidden" in line.lower():
            continue
        for t in FORBIDDEN:
            if t in line:
                hits.append(f"{i}:{t}")
    return hits


def main() -> None:
    if "--self-test" in sys.argv:
        bad = "class CapabilityRegistry { };  // M6 synthetic inflation"
        assert scan_text(bad) == [f"1:CapabilityRegistry"], scan_text(bad)
        ok = "FORBIDDEN = [\"CapabilityRegistry\"]"
        assert scan_text(ok) == [], scan_text(ok)
        print("check_copy_x0_design: self-test PASS")
        return
    failures = []
    for path in SCAN:
        if not path.is_file():
            failures.append(f"missing file: {path}")
            continue
        hits = scan_text(path.read_text())
        if hits:
            failures.append(f"{path}: forbidden identifiers {hits}")
    bench = SCAN[0]
    if bench.is_file():
        text = bench.read_text()
        for m in REQUIRED_BENCH_MARKERS:
            if m not in text:
                failures.append(f"bench missing required marker: {m}")
    if failures:
        print("check_copy_x0_design: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        sys.exit(1)
    print("check_copy_x0_design: PASS (no framework inflation; research-only "
          "banner present)")


if __name__ == "__main__":
    main()
