#!/usr/bin/env python3
"""F0 surface-inventory helper (#454).

Regenerates the mechanical facts behind docs/review/f0-role-profile-census.md:
header list, include graph, per-header consumer groups, orphan headers, and
transitive detail/experimental exposure. Output is plain text on stdout; it is
discovery evidence, not a semantic authority.

Usage: python3 scripts/f0_surface_inventory.py [repo-root]
"""

import collections
import os
import re
import sys

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)


def repo_files(root):
    for base in ("include", "src", "apps", "tests"):
        for dp, _, fns in os.walk(os.path.join(root, base)):
            for fn in fns:
                if fn.endswith((".hpp", ".cpp", ".h", ".cc")):
                    yield os.path.relpath(os.path.join(dp, fn), root)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    edges = collections.defaultdict(set)
    rev = collections.defaultdict(set)

    for f in repo_files(root):
        text = open(f, encoding="utf-8", errors="replace").read()
        for m in INCLUDE_RE.finditer(text):
            name = m.group(1)
            if name.startswith("sluice/"):
                target = os.path.join(root, "include", name)
                if os.path.exists(target):
                    target = os.path.relpath(target, root)
                    edges[f].add(target)
                    rev[target].add(f)

    def closure(header, seen=None):
        if seen is None:
            seen = set()
        for e in edges.get(header, ()):
            if e not in seen:
                seen.add(e)
                closure(e, seen)
        return seen

    all_headers = sorted(
        os.path.relpath(os.path.join(dp, fn), root)
        for dp, _, fns in os.walk(os.path.join(root, "include"))
        for fn in fns
    )

    print("== orphan headers (zero sluice/-style includes repo-wide) ==")
    for h in all_headers:
        if h not in rev:
            print(h)

    def group(f):
        if f.startswith("include/"):
            return "hdr"
        for prefix, label in (("src/async/", "src-async"), ("src/", "src-core"),
                              ("apps/", "app"), ("tests/", "tst")):
            if f.startswith(prefix):
                return label
        return "other"

    print("\n== per-header direct consumers by group ==")
    for h in sorted(rev):
        counts = collections.Counter(group(f) for f in rev[h])
        summary = " ".join(f"{k}:{v}" for k, v in sorted(counts.items()))
        print(f"{h}\t{summary}")

    print("\n== non-detail public headers transitively reaching detail/ or experimental/ ==")
    for h in all_headers:
        if "/detail/" in h or "/experimental/" in h or not h.startswith("include/"):
            continue
        reach = closure(h)
        detail = sorted(x for x in reach if "/detail/" in x)
        exp = sorted(x for x in reach if "/experimental/" in x)
        if detail or exp:
            print(f"{h}")
            for x in detail:
                print(f"    detail: {x}")
            for x in exp:
                print(f"    experimental: {x}")


if __name__ == "__main__":
    main()
