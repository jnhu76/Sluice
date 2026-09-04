#!/usr/bin/env python3
"""Verify architecture documentation structure.

Checks:
- Required architecture documents exist.
- Constitution rule IDs (AC-N) are unique.
- Divergence registry entries have required fields.
- Implementation-map target rows resolve to Xmake target declarations.
- PR template references the architecture gate.
- AGENTS.md references the constitution.

This script does NOT validate architectural correctness. It only checks
structural completeness of the documentation governance files.

Usage:
    python3 scripts/verify-architecture-docs.py
    python3 scripts/verify-architecture-docs.py --self-test

Exit code 0 = all checks pass. Non-zero = at least one failure.
"""

import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

REQUIRED_DOCS = [
    "docs/architecture/overview.md",
    "docs/history/closeout/as-built-async-architecture.md",
    "docs/architecture/zig-io-conformance-map.md",
    "docs/architecture/architecture-constitution.md",
    "docs/architecture/design-compliance-gate.md",
    "docs/architecture/divergence-registry.md",
    "docs/history/closeout/current-architecture-findings.md",
    "docs/history/closeout/remediation-roadmap.md",
    "docs/templates/architecture-design-template.md",
]

failures: list[str] = []

IMPLEMENTATION_MAP_RE = re.compile(
    r"^## Authoritative implementation map\s*$"
    r"(?P<body>.*?)"
    r"(?=^## |\Z)",
    re.MULTILINE | re.DOTALL,
)
IMPLEMENTATION_TARGET_ROW_RE = re.compile(
    r"^\|\s*`(?P<target>[^`]+)`\s*\|", re.MULTILINE
)
XMAKE_TARGET_RE = re.compile(r'target\(\s*["\']([^"\']+)["\']\s*\)')


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
    required_fields = [
        "ID", "Status", "Reason", "Revisit trigger",
        "Governing ADR", "Benefit", "Cost", "Current evidence",
    ]
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
        # Approved entries MUST have a governing authority (not "None")
        status_match = re.search(r"\|\s*Status\s*\|\s*(.+?)\s*\|", section)
        adr_match = re.search(r"\|\s*Governing ADR\s*\|\s*(.+?)\s*\|", section)
        if status_match and adr_match:
            status = status_match.group(1).strip()
            adr = adr_match.group(1).strip()
            if status == "Approved" and (
                not adr or adr.lower().startswith("none")
            ):
                fail(
                    f"DIVERGENCE: {entry_id} has status 'Approved' but no "
                    f"governing ADR or design authority"
                )


def strip_lua_comments(text: str) -> str:
    text = re.sub(r"--\[(=*)\[.*?\]\1\]", "", text, flags=re.DOTALL)
    return "\n".join(line.split("--", 1)[0] for line in text.splitlines())


def check_implementation_map_targets(root: Path = REPO_ROOT) -> list[str]:
    overview = root / "docs/architecture/overview.md"
    if not overview.is_file():
        return []

    text = overview.read_text(encoding="utf-8")
    section = IMPLEMENTATION_MAP_RE.search(text)
    if not section:
        return ["IMPLEMENTATION MAP: authoritative map section not found"]

    targets = IMPLEMENTATION_TARGET_ROW_RE.findall(section.group("body"))
    if not targets:
        return ["IMPLEMENTATION MAP: no backticked Xmake target rows found"]

    errors: list[str] = []
    duplicates = sorted({target for target in targets if targets.count(target) > 1})
    for target in duplicates:
        errors.append(f"IMPLEMENTATION MAP: duplicate target row: {target}")

    xmake_files = [root / "xmake.lua"]
    xmake_dir = root / "xmake"
    if xmake_dir.is_dir():
        xmake_files.extend(sorted(xmake_dir.rglob("*.lua")))
    declared: set[str] = set()
    for path in xmake_files:
        if path.is_file():
            xmake_text = strip_lua_comments(path.read_text(encoding="utf-8"))
            declared.update(XMAKE_TARGET_RE.findall(xmake_text))

    for target in targets:
        if target not in declared:
            errors.append(
                f"IMPLEMENTATION MAP: target `{target}` has no Xmake declaration"
            )
    return errors


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
    failures.extend(check_implementation_map_targets())
    check_pr_template()
    check_agents_md()

    if failures:
        print(f"FAIL: {len(failures)} architecture doc check(s) failed:\n")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("OK: all architecture documentation checks passed.")
    return 0


def self_test() -> int:
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "docs/architecture").mkdir(parents=True)
        (root / "xmake").mkdir()
        (root / "xmake/libraries.lua").write_text(
            'target("sluice_core")\n', encoding="utf-8"
        )
        overview = root / "docs/architecture/overview.md"

        overview.write_text(
            "# Overview\n\n## Authoritative implementation map\n\n"
            "| Boundary | Authority |\n| --- | --- |\n"
            "| `sluice_core` | `xmake/libraries.lua` |\n",
            encoding="utf-8",
        )
        if check_implementation_map_targets(root):
            print("SELF-TEST FAIL: valid target row was rejected")
            return 1

        overview.write_text(
            "# Overview\n\n## Authoritative implementation map\n\n"
            "| Boundary | Authority |\n| --- | --- |\n"
            "| `missing_target` | `xmake/libraries.lua` |\n",
            encoding="utf-8",
        )
        if not check_implementation_map_targets(root):
            print("SELF-TEST FAIL: unknown target row was accepted")
            return 1

        (root / "xmake/libraries.lua").write_text(
            '-- target("sluice_core")\n', encoding="utf-8"
        )
        overview.write_text(
            "# Overview\n\n## Authoritative implementation map\n\n"
            "| Boundary | Authority |\n| --- | --- |\n"
            "| `sluice_core` | `xmake/libraries.lua` |\n",
            encoding="utf-8",
        )
        if not check_implementation_map_targets(root):
            print("SELF-TEST FAIL: commented-out target was accepted")
            return 1

        (root / "xmake/libraries.lua").write_text(
            'target("sluice_core")\n', encoding="utf-8"
        )
        overview.write_text(
            "# Overview\n\n## Authoritative implementation map\n\n"
            "| Boundary | Authority |\n| --- | --- |\n"
            "| `sluice_core` | `xmake/libraries.lua` |\n"
            "| `sluice_core` | `xmake/libraries.lua` |\n",
            encoding="utf-8",
        )
        duplicate_errors = check_implementation_map_targets(root)
        if not any("duplicate" in error for error in duplicate_errors):
            print("SELF-TEST FAIL: duplicate target row was accepted")
            return 1

        overview.write_text("# Overview\n", encoding="utf-8")
        if not check_implementation_map_targets(root):
            print("SELF-TEST FAIL: missing map section was accepted")
            return 1

    print("OK: architecture documentation self-test passed.")
    return 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    if sys.argv[1:]:
        print("usage: verify-architecture-docs.py [--self-test]", file=sys.stderr)
        sys.exit(2)
    sys.exit(main())
