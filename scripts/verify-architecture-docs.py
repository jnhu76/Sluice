#!/usr/bin/env python3
"""Verify architecture documentation structure.

Checks:
- Required architecture documents exist.
- Constitution rule IDs (AC-N) are unique.
- Divergence registry entries have required fields.
- PR template references the architecture gate.
- AGENTS.md references the constitution.

This script does NOT validate architectural correctness. It only checks
structural completeness of the documentation governance files.

Usage:
    python3 scripts/verify-architecture-docs.py

Exit code 0 = all checks pass. Non-zero = at least one failure.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

REQUIRED_DOCS = [
    "docs/architecture/as-built-async-architecture.md",
    "docs/architecture/zig-io-conformance-map.md",
    "docs/architecture/architecture-constitution.md",
    "docs/architecture/design-compliance-gate.md",
    "docs/architecture/divergence-registry.md",
    "docs/architecture/current-architecture-findings.md",
    "docs/architecture/remediation-roadmap.md",
    "docs/templates/architecture-design-template.md",
]

failures: list[str] = []


def fail(msg: str) -> None:
    failures.append(msg)


def check_required_docs() -> None:
    for rel in REQUIRED_DOCS:
        path = REPO_ROOT / rel
        if not path.is_file():
            fail(f"MISSING: required document not found: {rel}")


def check_constitution_ids() -> None:
    path = REPO_ROOT / "docs/architecture/architecture-constitution.md"
    if not path.is_file():
        return  # already reported by check_required_docs
    text = path.read_text(encoding="utf-8")
    ids = re.findall(r"^## (AC-\d+)\.", text, re.MULTILINE)
    if not ids:
        fail("CONSTITUTION: no AC-N rule headings found")
        return
    seen: dict[str, int] = {}
    for rule_id in ids:
        seen[rule_id] = seen.get(rule_id, 0) + 1
    for rule_id, count in seen.items():
        if count > 1:
            fail(f"CONSTITUTION: duplicate rule ID {rule_id} ({count} occurrences)")


def check_divergence_fields() -> None:
    path = REPO_ROOT / "docs/architecture/divergence-registry.md"
    if not path.is_file():
        return
    text = path.read_text(encoding="utf-8")
    # Find all DIV-NN entries
    entries = re.findall(r"^## (DIV-\d+):", text, re.MULTILINE)
    if not entries:
        fail("DIVERGENCE: no DIV-NN entry headings found")
        return
    required_fields = ["ID", "Status", "Reason", "Revisit trigger"]
    for entry_id in entries:
        # Extract the section for this entry (until next ## or end)
        pattern = rf"^## {re.escape(entry_id)}:.*?(?=^## |\Z)"
        match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
        if not match:
            fail(f"DIVERGENCE: cannot extract section for {entry_id}")
            continue
        section = match.group(0)
        for field in required_fields:
            if field not in section:
                fail(f"DIVERGENCE: {entry_id} missing required field: {field}")


def check_pr_template() -> None:
    path = REPO_ROOT / ".github/pull_request_template.md"
    if not path.is_file():
        fail("PR TEMPLATE: .github/pull_request_template.md not found")
        return
    text = path.read_text(encoding="utf-8")
    if "architecture" not in text.lower():
        fail("PR TEMPLATE: does not reference architecture gate")
    if "AC-" not in text and "constitution" not in text.lower():
        fail("PR TEMPLATE: does not reference constitution rules")


def check_agents_md() -> None:
    path = REPO_ROOT / "AGENTS.md"
    if not path.is_file():
        fail("AGENTS: AGENTS.md not found")
        return
    text = path.read_text(encoding="utf-8")
    if "architecture-constitution.md" not in text:
        fail("AGENTS: does not reference architecture-constitution.md")
    if "design-compliance-gate.md" not in text:
        fail("AGENTS: does not reference design-compliance-gate.md")
    if "divergence-registry.md" not in text:
        fail("AGENTS: does not reference divergence-registry.md")


def main() -> int:
    check_required_docs()
    check_constitution_ids()
    check_divergence_fields()
    check_pr_template()
    check_agents_md()

    if failures:
        print(f"FAIL: {len(failures)} architecture doc check(s) failed:\n")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("OK: all architecture documentation checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
