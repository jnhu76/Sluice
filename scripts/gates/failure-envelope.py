#!/usr/bin/env python3
"""failure-envelope.py — machine-checkable phase x fault x required-outcome matrix gate (#198).

Validates docs/verification/failure-envelope.json fail-closed:

  * closed vocabularies for phase / fault / layer / required_outcome / status /
    evidence kind / evidence tier / taxonomy — an unknown token fails the gate;
  * unique FE-NNN ids;
  * every VERIFIED row cites >= 1 evidence pointer and every pointer resolves:
      - ref:    file exists in the repository;
      - anchor: the token appears in the referenced file (mutant id, section);
      - case:   the test-case name appears under tests/;
      - target: the xmake target name appears under xmake/;
  * provenance: when a pointer carries BOTH case and target, the case must
    actually occur in a source file the target builds (case in source, source
    in target). A case that exists somewhere under tests/ and a target that
    exists somewhere under xmake/ but with no owning-source relation is a fake
    tuple and fails the gate;
  * open states (PENDING / PLATFORM-BOUND / COVERAGE-BOUNDARY) REQUIRE a
    status_note — never a silently green row;
  * spanning rows (the `accepted -> cannot disappear` class) must not be bare
    PENDING; PLATFORM-BOUND / COVERAGE-BOUNDARY are honest bounded-open states
    and allowed with a status_note;
  * coverage floor: every phase in the vocabulary has >= 1 VERIFIED row, and
    >= 5 spanning VERIFIED rows exist.

--self-test plants each violation shape against a copy of the real matrix and
requires the corresponding detector to fire (plus a false-positive check that
the unmodified real matrix passes). The provenance shape is covered by BOTH a
dangling-pointer plant (missing token) and a wrong-tuple plant (every token
real, but combined into a tuple the owning source does not support).
"""

import json
import re
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
MATRIX = REPO / "docs/verification/failure-envelope.json"

PHASES = {
    "construction", "reserve", "prepare", "commit", "enqueue", "dispatch",
    "syscall", "transfer", "uring-submit", "cancel", "wait", "reap",
    "reset-reuse", "close", "drain", "destruction",
}
FAULTS = {
    "worker-spawn-failure", "capacity-exhausted", "descriptor-malformed",
    "stage-failure-injection", "waiter-borrow-window", "cas-loss-contention",
    "publication-order", "allocation-failure", "dispatch-failure", "eintr",
    "syscall-error", "short-transfer", "zero-progress-write",
    "submit-transient-error", "submit-zero-progress", "submit-partial",
    "submit-permanent-failure", "stale-identity", "double-terminal",
    "cancel-intent-accounting", "cancel-races-dispatch", "waiter-conflict",
    "wait-cancel-overreach", "lost-wake", "close-races-acceptance",
    "close-races-accepted-work", "interrupt-races-ready",
    "non-quiescent-destruction", "reset-releases-slot",
    "wal-decode-corruption", "kernel-fault", "scheduler-dispatch-fault",
}
LAYERS = {"core", "arena", "fake-async", "threadpool", "uring", "context", "scheduler"}
OUTCOMES = {
    "spawn-failure-propagates", "sync-rejection-idle-completion",
    "sync-rejection-zero-residue", "malformed-descriptor-invalid-argument",
    "full-rollback", "binding-rollback", "payload-visible-on-outstanding",
    "accepted-cannot-disappear", "allocation-free-post-commit",
    "generation-safe-reuse", "retry-or-single-terminal", "verbatim-result-wins",
    "cancel-accounting-exact", "short-valid-loop-owner",
    "zero-progress-invalid-state", "transient-recovered-next-poll",
    "state-unchanged", "suffix-remains-owned", "quarantine-held",
    "exactly-one-terminal", "wake-preserved", "single-waiter-registration",
    "wait-cancel-independent", "reap-gates-publication",
    "close-serializes-acceptance", "close-preserves-accepted",
    "non-quiescent-fail-fast", "quiescent-destroy-succeeds",
    "t1-error-surfaces-via-result", "wal-decode-rejected-deterministic",
}
STATUSES = {"VERIFIED", "PENDING", "PLATFORM-BOUND", "COVERAGE-BOUNDARY"}
KINDS = {"mutation", "test", "death", "weakmem", "fuzz", "formal"}
TIERS = {
    "deterministic-fake", "syscall-boundary", "kernel-scripted",
    "platform-real", "bounded-model", "formal-tla", "fuzz",
}
TAXONOMY = {"T1", "T2", "T3", "T4", "T5", "T6", "T7"}

ROW_FIELDS = {"id", "phase", "fault", "layer", "required_outcome", "taxonomy",
              "spanning", "status", "evidence", "notes", "status_note"}
EV_FIELDS = {"kind", "ref", "tier", "anchor", "case", "target"}


def _corpus(paths_glob_root):
    blob = []
    for p in sorted(paths_glob_root.rglob("*")):
        if p.is_file() and p.suffix in (".cpp", ".hpp", ".lua", ".sh", ".py", ".md", ".json"):
            try:
                blob.append(p.read_text(errors="replace"))
            except OSError:
                pass
    return "\n".join(blob)


def _target_known(target: str, xmake_blob: str) -> bool:
    """A target is known if its name appears in xmake/, or it is a core
    one-file target generated as '<stem>_test' from a quoted stem list
    (xmake/tests/core.lua builds the name by concatenation)."""
    if target in xmake_blob:
        return True
    if target.endswith("_test"):
        stem = target[: -len("_test")]
        return f'"{stem}"' in xmake_blob
    return False


class XmakeTargetSources:
    """Best-effort static resolver: xmake target name -> source file set.

    xmake configuration is imperative Lua; a perfect resolver would need a Lua
    interpreter. This scanner covers the declaration shapes actually used in
    xmake/ and FAILS CLOSED: an evidence target whose source set cannot be
    resolved fails the gate (no silent pass). Covered shapes:

      * target("NAME") blocks with add_files(...) — terminated by the next
        target( line or by an `end` that closes the target (tracking
        if/do/for/while/function scopes so an inner `end` does not end the
        target early);
      * `local <var> = R .. "path"` assignments, scope-aware (a `local` is
        visible only from its enclosing do/if block inward, so repeated
        `local p = ...` in sibling blocks do not shadow each other);
      * sluice_one_file_target(kind, group, "NAME", subdir, ...) -> subdir/NAME.cpp;
      * sluice_one_file_test / sluice_internal_async_test /
        sluice_production_async_test("NAME", {source = ..., ...}) ->
        tests/NAME.cpp or the source override;
      * fallback: tests/<NAME>.cpp when the file exists (covers the core.lua
        stem-loop-generated one-file tests, whose names are concatenated at
        Lua runtime and cannot be parsed statically).

    Globs are expanded only for the fixed production manifests
    (src/*.cpp, src/async/*.cpp).
    """

    def __init__(self, repo: Path):
        self.repo = repo
        self._by_target = {}
        self._loaded = False

    def sources(self, target: str):
        if target not in self._by_target:
            self._load()
        if self._by_target.get(target):
            return self._by_target[target]
        # Fallback: a one-file test target whose name maps directly to
        # tests/<name>.cpp (core.lua stem loops and platform-gated helpers are
        # declared by concatenation and are not statically parseable).
        f = self.repo / "tests" / f"{target}.cpp"
        if f.is_file():
            return (f"tests/{target}.cpp",)
        return ()

    def _load(self):
        if self._loaded:
            return
        self._loaded = True
        for lf in sorted((self.repo / "xmake").rglob("*.lua")):
            self._scan_file(lf)

    @staticmethod
    def _strip_comment(line: str) -> str:
        """Cut a `--` comment, ignoring `--` inside quoted strings."""
        inquote = False
        for i, ch in enumerate(line):
            if ch == '"':
                inquote = not inquote
            elif ch == "-" and i + 1 < len(line) and line[i + 1] == "-" and not inquote:
                return line[:i]
        return line

    @staticmethod
    def _block_tokens(line: str):
        """Return (opens, closes) Lua block tokens approximated for one line.

        Opens: if...then, for...do, while...do, function, repeat, bare do.
        Closes: end, until. The bare-do count deliberately excludes the `do`
        that belongs to for/while so one line cannot open twice by accident.
        """
        opens = closes = 0
        if re.search(r'\bif\b.*\bthen\b', line) or \
           re.search(r'\bfor\b.*\bdo\b', line) or \
           re.search(r'\bwhile\b.*\bdo\b', line):
            opens += 1
        elif re.search(r'\bfunction\b', line) or re.search(r'\brepeat\b', line):
            opens += 1
        elif re.search(r'(?<!\w)do(?!\w)', line):
            opens += 1
        closes += len(re.findall(r'\bend\b', line))
        closes += len(re.findall(r'\buntil\b', line))
        return opens, closes

    def _scan_file(self, lf: Path):
        text = lf.read_text(errors="replace")
        # Scope stack of locals dicts; file scope first.
        scopes = [{}]
        cur_target = None
        targets = {}

        def resolve(expr, lookup):
            expr = expr.strip()
            m = re.match(r'^R\s*\.\.\s*(.+)$', expr)
            if m:
                expr = m.group(1).strip()
            m = re.match(r'^"([^"]+)"$', expr)
            if m:
                return m.group(1).lstrip("./")
            if re.fullmatch(r'\w+', expr):
                return lookup(expr)
            m = re.match(r'^(\w+)\s*\.\.\s*"([^"]+)"$', expr)
            if m:
                base = lookup(m.group(1))
                if base:
                    return base.rstrip("/") + "/" + m.group(2).lstrip("./")
            return None

        def scope_lookup(name):
            for scope in reversed(scopes):
                if name in scope:
                    return scope[name]
            return None

        for raw in text.splitlines():
            line = self._strip_comment(raw).strip()
            if not line:
                continue
            # `local <var> = <expr>` binds in the innermost scope.
            lm = re.match(r'^local\s+(\w+)\s*=\s*(.+)$', line)
            if lm:
                val = resolve(lm.group(2), scope_lookup)
                if val:
                    scopes[-1][lm.group(1)] = val
                continue
            # A target( line both ends any previous target and starts a new one.
            tm = re.match(r'^target\s*\(\s*"([^"]+)"\s*\)\s*$', line)
            if tm:
                cur_target = tm.group(1)
                targets.setdefault(cur_target, set())
                continue
            if cur_target:
                for fm in re.finditer(r'add_files\s*\(([^)]*)\)', line):
                    args = fm.group(1)
                    for tok in re.findall(r'"([^"]+)"', args):
                        for f in self._expand(tok):
                            targets[cur_target].add(f)
                    for var in re.findall(r'\b(\w+)\b', args):
                        val = scope_lookup(var)
                        if val:
                            for f in self._expand(val):
                                targets[cur_target].add(f)
            # Scope close: `end`/`until` with nothing else on the line closes
            # a Lua block; when no Lua block is open it closes the target.
            opens, closes = self._block_tokens(line)
            for _ in range(closes):
                if len(scopes) > 1:
                    scopes.pop()
                else:
                    cur_target = None
            for _ in range(opens):
                scopes.append({})

        for name, files in targets.items():
            if files:
                self._by_target.setdefault(name, set()).update(files)

        # Helper declarations (sluice_one_file_target / one-file test helpers).
        for m in re.finditer(r'sluice_one_file_target\s*\(([^)]*)\)', text):
            args = [a.strip() for a in m.group(1).split(",")]
            if len(args) >= 4:
                name = args[2].strip('"')
                subdir = args[3].strip('"')
                if name and subdir:
                    p = f"{subdir}/{name}.cpp"
                    if (self.repo / p).is_file():
                        self._by_target.setdefault(name, set()).add(p)
        for helper in ("sluice_one_file_test", "sluice_internal_async_test",
                       "sluice_production_async_test"):
            for m in re.finditer(helper + r'\s*\(', text):
                call = self._balanced_call(text, m.end())
                if call is None:
                    continue
                nm = re.match(r'\s*"([^"]+)"', call)
                if not nm:
                    continue
                name = nm.group(1)
                src = f"tests/{name}.cpp"
                sm = re.search(r'source\s*=\s*([^,}]+)', call)
                if sm:
                    val = resolve(sm.group(1).strip(), scope_lookup)
                    if val:
                        src = val
                if (self.repo / src).is_file():
                    self._by_target.setdefault(name, set()).add(src)

    @staticmethod
    def _balanced_call(text: str, start: int):
        depth = 0
        inquote = False
        i = start
        while i < len(text):
            c = text[i]
            if c == '"' and (i == 0 or text[i - 1] != "\\"):
                inquote = not inquote
            elif not inquote:
                if c == "(":
                    depth += 1
                elif c == ")":
                    if depth == 0:
                        return text[start:i]
                    depth -= 1
            i += 1
        return None

    def _expand(self, rel: str):
        rel = rel.lstrip("./")
        if rel.endswith("*.cpp"):
            base = rel[: -len("*.cpp")]
            root = self.repo / base
            if root.is_dir():
                return sorted(str(p.relative_to(self.repo))
                              for p in root.glob("*.cpp") if p.is_file())
            return ()
        return (rel,) if (self.repo / rel).is_file() else ()


class Resolver:
    """Aggressive caching content resolver for pointer checks."""

    def __init__(self, repo: Path):
        self.repo = repo
        self._files = {}
        self._tests = None
        self._xmake = None
        self._targets = XmakeTargetSources(repo)

    def file_text(self, rel: str):
        if rel not in self._files:
            f = self.repo / rel
            self._files[rel] = f.read_text(errors="replace") if f.is_file() else None
        return self._files[rel]

    def tests_blob(self):
        if self._tests is None:
            self._tests = _corpus(self.repo / "tests")
        return self._tests

    def xmake_blob(self):
        if self._xmake is None:
            self._xmake = _corpus(self.repo / "xmake")
        return self._xmake

    def target_sources(self, target: str):
        return self._targets.sources(target)


def validate(doc, resolver: Resolver):
    errors = []

    def err(msg):
        errors.append(msg)

    if doc.get("schema") != "sluice-failure-envelope/1":
        err(f"top-level: schema must be 'sluice-failure-envelope/1', got {doc.get('schema')!r}")
    if doc.get("core_invariant") != "accepted -> cannot disappear":
        err(f"top-level: core_invariant drifted: {doc.get('core_invariant')!r}")
    rows = doc.get("rows")
    if not isinstance(rows, list) or not rows:
        err("top-level: rows must be a non-empty list")
        return errors

    seen_ids = set()
    verified_by_phase = {p: 0 for p in sorted(PHASES)}
    spanning_verified = 0

    for row in rows:
        rid = row.get("id", "<no id>")
        if not isinstance(rid, str) or not re.fullmatch(r"FE-\d{3}", rid):
            err(f"{rid}: id must match FE-NNN")
        if rid in seen_ids:
            err(f"{rid}: duplicate id")
        seen_ids.add(rid)

        unknown_fields = set(row) - ROW_FIELDS
        if unknown_fields:
            err(f"{rid}: unknown row fields {sorted(unknown_fields)}")

        if row.get("phase") not in PHASES:
            err(f"{rid}: unknown phase {row.get('phase')!r}")
        if row.get("fault") not in FAULTS:
            err(f"{rid}: unknown fault {row.get('fault')!r}")
        if row.get("layer") not in LAYERS:
            err(f"{rid}: unknown layer {row.get('layer')!r}")
        if row.get("required_outcome") not in OUTCOMES:
            err(f"{rid}: unknown required_outcome {row.get('required_outcome')!r}")
        if row.get("taxonomy") not in TAXONOMY:
            err(f"{rid}: unknown taxonomy {row.get('taxonomy')!r}")
        if row.get("status") not in STATUSES:
            err(f"{rid}: unknown status {row.get('status')!r}")
        if not isinstance(row.get("spanning"), bool):
            err(f"{rid}: spanning must be a boolean")

        status = row.get("status")
        if status == "VERIFIED":
            if not row.get("evidence"):
                err(f"{rid}: VERIFIED row has no evidence")
        else:
            if not (row.get("status_note") or "").strip():
                err(f"{rid}: status {status} requires a non-empty status_note "
                    f"(never a silently green row)")
        if row.get("spanning") and status == "PENDING":
            # The spanning class (accepted -> cannot disappear) may be honestly
            # PLATFORM-BOUND / COVERAGE-BOUNDARY at an unavailable tier, but a
            # bare PENDING spanning row means runnable evidence was skipped.
            err(f"{rid}: spanning row (accepted -> cannot disappear) must not be "
                f"PENDING (verify it, or bound the open tier explicitly)")

        if status == "VERIFIED" and row.get("phase") in verified_by_phase:
            verified_by_phase[row["phase"]] += 1
        if row.get("spanning") and status == "VERIFIED":
            spanning_verified += 1

        evs = row.get("evidence", [])
        if not isinstance(evs, list):
            err(f"{rid}: evidence must be a list")
            continue
        for i, ev in enumerate(evs):
            where = f"{rid}/evidence[{i}]"
            if not isinstance(ev, dict):
                err(f"{where}: must be an object")
                continue
            unknown_ev = set(ev) - EV_FIELDS
            if unknown_ev:
                err(f"{where}: unknown evidence fields {sorted(unknown_ev)}")
            if ev.get("kind") not in KINDS:
                err(f"{where}: unknown kind {ev.get('kind')!r}")
            if ev.get("tier") not in TIERS:
                err(f"{where}: unknown tier {ev.get('tier')!r}")
            ref = ev.get("ref")
            if not ref:
                err(f"{where}: missing ref")
                continue
            text = resolver.file_text(ref)
            if text is None:
                err(f"{where}: missing evidence ref {ref}")
                continue
            anchor = ev.get("anchor")
            if anchor and anchor not in text:
                err(f"{where}: anchor {anchor!r} not found in {ref}")
            case = ev.get("case")
            if case and case not in resolver.tests_blob():
                err(f"{where}: unknown test case {case!r} (not present under tests/)")
            target = ev.get("target")
            if target and not _target_known(target, resolver.xmake_blob()):
                err(f"{where}: unknown xmake target {target!r} (not present under xmake/)")
            # Provenance: case in source, source in target. A pointer that
            # carries both must name a case the cited target actually builds.
            # Every token existing *somewhere* is not enough — the tuple must
            # be supported by an owning source file.
            if case and target:
                sources = resolver.target_sources(target)
                if not sources:
                    err(f"{where}: cannot resolve any source file for target "
                        f"{target!r} (provenance uncheckable)")
                elif not any(case in (resolver.file_text(s) or "")
                             for s in sources):
                    err(f"{where}: case {case!r} is not found in any source file "
                        f"built by target {target!r} — the evidence tuple does "
                        f"not resolve (case in source, source in target)")

    for phase, n in verified_by_phase.items():
        if n == 0:
            err(f"coverage floor: phase {phase!r} has no VERIFIED row "
                f"(add evidence or an explicit open row and shrink the vocabulary)")
    if spanning_verified < 5:
        err(f"coverage floor: only {spanning_verified} spanning VERIFIED rows (need >= 5)")
    return errors


def self_test() -> int:
    base = json.loads(MATRIX.read_text())
    resolver = Resolver(REPO)

    # False-positive control: the real matrix must pass.
    errors = validate(base, resolver)
    if errors:
        print("self-test FAIL: the real matrix itself reported errors:")
        for e in errors:
            print("  ", e)
        return 1

    def mutant(fn):
        doc = json.loads(json.dumps(base))
        fn(doc)
        return doc

    def by_id(doc, rid):
        return next(r for r in doc["rows"] if r["id"] == rid)

    plants = []

    def plant(name, expect_substr, fn):
        plants.append((name, expect_substr, mutant(fn)))

    plant("unknown phase", "unknown phase",
          lambda d: by_id(d, "FE-016").__setitem__("phase", "poke"))
    plant("unknown outcome", "unknown required_outcome",
          lambda d: by_id(d, "FE-016").__setitem__("required_outcome", "always-fine"))
    plant("verified row without evidence", "no evidence",
          lambda d: by_id(d, "FE-016").__setitem__("evidence", []))
    plant("dangling ref", "missing evidence ref",
          lambda d: by_id(d, "FE-016")["evidence"][0].__setitem__(
              "ref", "docs/verification/does-not-exist.md"))
    plant("unknown test case", "unknown test case",
          lambda d: by_id(d, "FE-016")["evidence"][0].__setitem__(
              "case", "case_that_does_not_exist_anywhere"))
    plant("anchor not in doc", "not found in",
          lambda d: by_id(d, "FE-016")["evidence"][0].__setitem__("anchor", "M999"))
    plant("unknown xmake target", "unknown xmake target",
          lambda d: by_id(d, "FE-016")["evidence"][0].__setitem__(
              "target", "target_that_does_not_exist"))
    plant("wrong tuple: case real but not in target's source",
          "does not resolve",
          lambda d: by_id(d, "FE-016")["evidence"][0].__setitem__(
              "case", "read_exact_assembles_short_reads"))
    plant("wrong tuple: target real but does not build the case's source",
          "does not resolve",
          lambda d: by_id(d, "FE-016")["evidence"][0].__setitem__(
              "target", "reader_test"))
    plant("target without resolvable sources", "cannot resolve any source",
          lambda d: by_id(d, "FE-016")["evidence"][0].__setitem__(
              "target", "sluice_core"))
    plant("duplicate id", "duplicate id",
          lambda d: d["rows"].append(dict(by_id(d, "FE-016"), notes="dup")))
    plant("open row without status_note", "requires a non-empty status_note",
          lambda d: by_id(d, "FE-017").pop("status_note"))
    plant("spanning row not verified", "spanning row",
          lambda d: (by_id(d, "FE-011").__setitem__("status", "PENDING"),
                     by_id(d, "FE-011").__setitem__(
                         "status_note", "planted: spanning PENDING")))
    plant("coverage floor (construction loses its only VERIFIED row)",
          "coverage floor",
          lambda d: (by_id(d, "FE-001").__setitem__("status", "PENDING"),
                     by_id(d, "FE-001").__setitem__("status_note", "planted")))
    plant("schema drift", "schema",
          lambda d: d.__setitem__("schema", "sluice-failure-envelope/2"))

    rc = 0
    for name, expect, doc in plants:
        errors = validate(doc, resolver)
        if not any(expect in e for e in errors):
            print(f"self-test FAIL [{name}]: expected an error containing "
                  f"{expect!r}, got: {errors[:3]}")
            rc = 1
        else:
            print(f"self-test PASS [{name}]")
    if rc == 0:
        print(f"failure-envelope self-test: all {len(plants)} planted violations "
              f"detected; real matrix passes")
    return rc


def main() -> int:
    args = sys.argv[1:]
    if args and args[0] == "--self-test":
        return self_test()
    matrix = Path(args[0]) if args else MATRIX
    try:
        doc = json.loads(matrix.read_text())
    except (OSError, json.JSONDecodeError) as e:
        print(f"failure-envelope: cannot read matrix {matrix}: {e}", file=sys.stderr)
        return 1
    errors = validate(doc, Resolver(REPO))
    n_rows = len(doc.get("rows", []))
    n_open = sum(1 for r in doc.get("rows", []) if r.get("status") != "VERIFIED")
    if errors:
        print(f"failure-envelope: FAIL ({len(errors)} error(s), {n_rows} rows)")
        for e in errors:
            print("  ", e)
        print("reproduce: python3 scripts/gates/failure-envelope.py")
        return 1
    print(f"failure-envelope: OK ({n_rows} rows, {n_rows - n_open} VERIFIED, "
          f"{n_open} honest open rows; vocabulary closed; all pointers resolve)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
