#!/usr/bin/env python3
# verify-backend-conformance.py
#
# Phase C1 aggregate backend-conformance gate.
#
# Reads scripts/backend_conformance_manifest.py and produces an honest per-
# backend, per-section report. It:
#   * preflights each target via `xmake show -t` (target existence) then
#     `xmake build` (build) then `xmake run` (run) — NO Lua-source grep;
#   * drives the shared suite once PER registered backend in a separate
#     subprocess filtered to that backend's exact driver case, and counts a
#     run as PASS only when it provably executed exactly that one case and
#     emitted exactly one [conformance-meta] line whose backend (canonicalized)
#     matches the registry, profile matches the manifest, and mode is allowed
#     for the profile — never display names, skip text, "PASS" strings, or
#     build-directory contents;
#   * classifies each backend's verdict (ELIGIBLE / INCOMPLETE / NOT
#     CONFORMING) from the manifest's closed profiles and the run results:
#     a mandatory RUN_FAIL is NOT CONFORMING; missing/insufficient mandatory
#     evidence is INCOMPLETE; only provably-satisfied mandatory layers make a
#     backend ELIGIBLE;
#   * exits non-zero on any mandatory MISSING_TARGET / BUILD_FAIL / RUN_FAIL,
#     any INCOMPLETE verdict for a non-kernel backend, or when a registered
#     backend's mandatory case-set is not covered.
#
# Usage:
#   python3 scripts/verify-backend-conformance.py
#   python3 scripts/verify-backend-conformance.py --no-build   # skip xmake
#                                                            # build (assume
#                                                            # already built)
#
# Exit codes:
#   0 — every mandatory gate PASS / NOT_APPLICABLE; all profiles honest.
#   1 — at least one mandatory gate did not pass (target missing, build/run
#       failure, an INCOMPLETE verdict for a non-kernel backend, or a
#       backend's mandatory case-set uncovered).
#   2 — harness error (xmake unavailable, manifest import failure).
"""Phase C1 aggregate backend-conformance gate."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Optional

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))

import backend_conformance_manifest as M  # noqa: E402


# Result states for a single piece of evidence.
PASS = "PASS"
MISSING_TARGET = "MISSING_TARGET"
BUILD_FAIL = "BUILD_FAIL"
RUN_FAIL = "RUN_FAIL"
NOT_RUN = "NOT_RUN"
NOT_APPLICABLE = "NOT_APPLICABLE"
INCOMPLETE = "INCOMPLETE"

# States that count as "satisfying" a mandatory evidence slot for the verdict.
SATISFACTORY = {PASS, NOT_APPLICABLE}

# Verdicts for a backend.
ELIGIBLE = "ELIGIBLE"               # every mandatory layer provably satisfied.
INCOMPLETE = "INCOMPLETE"           # insufficient evidence (not a violation).
NOT_CONFORMING = "NOT CONFORMING"   # a mandatory evidence proved a violation.


@dataclass
class RunResult:
    evidence_id: str
    target: str
    state: str
    detail: str = ""
    stdout: str = ""


# ---------------------------------------------------------------------------
# xmake execution helpers
# ---------------------------------------------------------------------------

def run_cmd(cmd: list[str], timeout: int = 600) -> tuple[int, str, str]:
    """Run a command in REPO_ROOT, returning (returncode, stdout, stderr)."""
    try:
        p = subprocess.run(
            cmd, cwd=REPO_ROOT, capture_output=True, text=True, timeout=timeout
        )
        return p.returncode, p.stdout, p.stderr
    except FileNotFoundError:
        return 127, "", f"command not found: {cmd[0]}"
    except subprocess.TimeoutExpired:
        return 124, "", f"timeout after {timeout}s: {' '.join(cmd)}"


def xmake_target_exists(target: str) -> bool:
    """Preflight: is `target` a valid xmake target? Uses `xmake show -t`."""
    rc, out, err = run_cmd(["xmake", "show", "-t", target], timeout=120)
    if rc != 0:
        return False
    # `xmake show -t <valid>` prints "The information of target(<name>):".
    # An invalid name prints an error and returns non-zero (handled above).
    return "information of target" in out or "targetfile" in out


def xmake_build_target(target: str) -> tuple[bool, str]:
    rc, out, err = run_cmd(["xmake", "build", target], timeout=900)
    combined = out + "\n" + err
    return rc == 0, combined


def xmake_run_target(target: str, env_filter: Optional[str] = None
                     ) -> tuple[int, str]:
    """Run a built test target; return (returncode, combined_output).

    `env_filter`, when set, is passed through as the SLUICE_TEST_FILTER env
    var so the in-binary test harness runs ONLY the exact case-name allowlist
    it names (see tests/harness.hpp; a filter that matches zero cases makes
    the binary exit non-zero). This is how the aggregate gate drives each
    registered backend's shared-suite case in a SEPARATE subprocess: one
    backend's failure cannot affect another backend's process exit code or
    [conformance-meta] emission.
    """
    env = dict(os.environ) if env_filter else None
    if env_filter:
        env["SLUICE_TEST_FILTER"] = env_filter
    try:
        p = subprocess.run(["xmake", "run", target], cwd=REPO_ROOT,
                           capture_output=True, text=True, timeout=600,
                           env=env)
        return p.returncode, p.stdout + "\n" + p.stderr
    except FileNotFoundError:
        return 127, f"command not found: xmake"
    except subprocess.TimeoutExpired:
        return 124, f"timeout after 600s: xmake run {target}"


def _state_for_rc(rc: int) -> str:
    """Map a subprocess return code to a gate state (pure, testable)."""
    return PASS if rc == 0 else RUN_FAIL


def run_shell_script(script_rel: str) -> tuple[int, str]:
    """Run a repo-relative shell script (the __script__: authority entries)."""
    assert script_rel.startswith("__script__:")
    name = script_rel[len("__script__:"):]
    path = os.path.join(REPO_ROOT, "scripts", name)
    if not os.path.isfile(path):
        return 0, ""  # will be reported as MISSING_TARGET by caller
    rc, out, err = run_cmd(["bash", path], timeout=300)
    return rc, out + "\n" + err


# ---------------------------------------------------------------------------
# [conformance-meta] parsing
# ---------------------------------------------------------------------------

META_RE = re.compile(
    r"^\[conformance-meta\]\s+backend=(\S+)\s+profile=(\S+)\s+mode=(\S+)\s*$"
)

EVIDENCE_META_RE = re.compile(
    r"^\[evidence-meta\]\s+evidence=(\S+)\s+mode=(\S+)\s*$"
)

# Match the display name even when it contains "(stub)" — backend= value is
# captured greedily up to whitespace, so "Uring(stub)" is captured whole.


def parse_meta_lines(driver_output: str) -> dict[str, dict[str, str]]:
    """Parse [conformance-meta] lines into {backend: {profile, mode}}.

    backend is the value after backend= EXACTLY as printed (e.g. "Uring(stub)").
    Deduplicated by backend name — intended for the REPORT. The fail-closed
    per-run validation uses parse_meta_line_list() instead, which preserves
    duplicates so that a double-emitted meta line cannot be mistaken for a
    single valid one.
    """
    meta: dict[str, dict[str, str]] = {}
    for line in driver_output.splitlines():
        m = META_RE.match(line.strip())
        if m:
            backend, profile, mode = m.group(1), m.group(2), m.group(3)
            meta[backend] = {"profile": profile, "mode": mode}
    return meta


def parse_meta_line_list(driver_output: str) -> list[tuple[str, str, str]]:
    """Every [conformance-meta] line in order, preserving duplicates.

    Returns [(backend, profile, mode)] with backend EXACTLY as printed.
    """
    lines: list[tuple[str, str, str]] = []
    for line in driver_output.splitlines():
        m = META_RE.match(line.strip())
        if m:
            lines.append((m.group(1), m.group(2), m.group(3)))
    return lines


# The harness prints exactly one "[run] <case-name>" line per executed case
# (tests/harness.hpp). The gate counts these to prove WHICH case ran.
RUN_RE = re.compile(r"^\[run\]\s+(\S+)\s*$")


def parse_run_lines(driver_output: str) -> list[str]:
    """The case names the harness actually executed, in order.

    An empty list means the SLUICE_TEST_FILTER selected nothing — that must
    never be treated as PASS.
    """
    return [m.group(1) for line in driver_output.splitlines()
            if (m := RUN_RE.match(line.strip()))]


def parse_evidence_meta_lines(driver_output: str) -> list[tuple[str, str]]:
    """Every real-only evidence declaration in order, preserving duplicates."""
    return [(m.group(1), m.group(2)) for line in driver_output.splitlines()
            if (m := EVIDENCE_META_RE.match(line.strip()))]


def canonical_backend_key(meta_backend: str, registered: list[str]) -> Optional[str]:
    """Map a meta backend= value to a registered backend name.

    The driver prints "Uring(stub)" in stub builds but the manifest registers
    "Uring". We match by prefix so the registry key wins, but we NEVER infer
    mode from this — mode comes from the meta line itself.
    """
    if meta_backend in registered:
        return meta_backend
    for name in registered:
        if meta_backend == name or meta_backend.startswith(name + "("):
            return name
    return None


# ---------------------------------------------------------------------------
# Gate execution
# ---------------------------------------------------------------------------

@dataclass
class Gate:
    args: argparse.Namespace
    results: dict[str, RunResult] = field(default_factory=dict)  # evidence_id -> RunResult
    # Per-backend shared-suite result. The shared driver binary runs ALL three
    # registered backends in ONE process, but the in-binary test harness
    # (tests/harness.hpp) BREAKS on the first failing case. Driving all three
    # backends in a single process therefore makes one backend's failure
    # contaminate the others (they never run, so their meta is never emitted
    # and their result is inferred from the single shared exit code — a
    # dishonest cross-backend attribution). To make each backend's verdict
    # depend ONLY on that backend's own run, the gate drives the shared suite
    # once per registered backend in a SEPARATE subprocess, filtered to that
    # backend's case via SLUICE_TEST_FILTER. Each subprocess owns its exit
    # code, its [conformance-meta] line, and its [conformance] FAIL lines.
    shared_by_backend: dict[str, RunResult] = field(default_factory=dict)
    # Phase C2a: per-backend shared CAPACITY-suite result, driven in its own
    # subprocess. Uring real mode runs the exact shared cases; Uring stub mode
    # executes only build/API classification and is recorded INCOMPLETE.
    capacity_by_backend: dict[str, RunResult] = field(default_factory=dict)
    # Phase C2e: per-backend shared CLOSE/DRAIN-suite result, driven in its own
    # subprocess (conformance_close_drain_fake / conformance_close_drain_threadpool).
    # Backends without a close_admission seam (Uring before D4) have no
    # close/drain driver case; their gap is the manifest's
    # uring_c2e_close_drain_not_implemented record.
    close_drain_by_backend: dict[str, RunResult] = field(default_factory=dict)
    meta: dict[str, dict[str, str]] = field(default_factory=dict)

    def run(self) -> int:
        # Drive every IMPLEMENTED evidence record once. Non-shared evidence is
        # already per-backend by construction (the manifest tags each record
        # with the backend(s) it covers), so it needs no subprocess isolation.
        for ev in M.EVIDENCE:
            if ev.status == M.STATUS_NOT_IMPLEMENTED:
                self.results[ev.evidence_id] = RunResult(
                    ev.evidence_id, ev.target, INCOMPLETE,
                    detail="manifest status not_implemented (Phase C2/D)")
                continue
            if ev.status == M.STATUS_NOT_APPLICABLE:
                self.results[ev.evidence_id] = RunResult(
                    ev.evidence_id, ev.target, NOT_APPLICABLE,
                    detail=ev.reason or "not applicable")
                continue
            # The shared base, capacity, and close/drain suites are driven per
            # backend below.
            if ev.evidence_id in ("shared_suite", "shared_capacity_suite",
                                  "c2e_shared_close_drain_suite"):
                continue
            self.results[ev.evidence_id] = self._drive(ev)

        # Drive the shared suite once PER registered backend, each in its own
        # subprocess filtered to that backend's case. This is the corrective
        # for the cross-backend contamination defect: each backend's shared
        # result now comes from ITS OWN subprocess exit code + meta + FAIL
        # lines, never from another backend's.
        shared_ev = M.evidence_by_id("shared_suite")
        if shared_ev is not None:
            self._run_shared_suite(shared_ev)

        # Phase C2a: drive the shared CAPACITY suite once per registered backend
        # that has a capacity driver case. Real Uring participates after D1;
        # its stub branch is classified INCOMPLETE below.
        cap_ev = M.evidence_by_id("shared_capacity_suite")
        if cap_ev is not None:
            self._run_capacity_suite(cap_ev)

        # Phase C2e: drive the shared CLOSE/DRAIN suite once per registered
        # backend that HAS a close_admission driver case (Fake, ThreadPool).
        # Backends without the seam (Uring before D4) have no driver case;
        # their gap is the manifest's uring_c2e_close_drain_not_implemented
        # record (never skip-as-pass).
        cd_ev = M.evidence_by_id("c2e_shared_close_drain_suite")
        if cd_ev is not None:
            self._run_close_drain_suite(cd_ev)

        # Parse [conformance-meta] from every per-backend shared run for the
        # REPORT, keyed by the CANONICAL registered backend name (a variant
        # such as "Uring(stub)" resolves to "Uring" via canonical_backend_key).
        # Per-run meta existence/identity is already enforced by
        # _classify_shared_run — a run without exactly one valid meta line can
        # never be PASS; this dict is only the report's display view.
        self.meta = {}
        registered = [b.name for b in M.BACKENDS]
        for name, rr in self.shared_by_backend.items():
            if rr.stdout:
                for meta_backend, info in parse_meta_lines(rr.stdout).items():
                    ck = canonical_backend_key(meta_backend, registered)
                    if ck is not None:
                        self.meta[ck] = info

        return self._report()

    def _run_shared_suite(self, ev: M.Evidence) -> None:
        """Drive the shared suite once per registered backend in isolation.

        Preflight (target existence + build) is done ONCE; each backend then
        runs in its own `xmake run` subprocess with SLUICE_TEST_FILTER set to
        its case name. A backend whose subprocess fails records RUN_FAIL for
        THAT backend only; the others run and report independently.
        """
        # Preflight target existence + build once (cheap, shared).
        if not xmake_target_exists(ev.target):
            missing = RunResult(ev.evidence_id, ev.target, MISSING_TARGET,
                                detail="xmake show -t reports not a valid target")
            for b in M.BACKENDS:
                self.shared_by_backend[b.name] = missing
            return
        if self.args is not None and not getattr(self.args, "no_build", False):
            ok, log = xmake_build_target(ev.target)
            if not ok:
                bf = RunResult(ev.evidence_id, ev.target, BUILD_FAIL,
                               detail="xmake build failed", stdout=log)
                for b in M.BACKENDS:
                    self.shared_by_backend[b.name] = bf
                return

        # One isolated subprocess per registered backend. The case name
        # (BackendEntry.driver_case) is the SLUICE_TEST_CASE the driver
        # registers for that backend (see backend_conformance_driver_test.cpp).
        # Each run is classified fail-closed: PASS only when it provably ran
        # exactly the driver case and emitted exactly one valid meta line.
        for b in M.BACKENDS:
            if not b.driver_case:
                # A registered backend with no shared-suite driver case must
                # NEVER be run unfiltered — that would execute every backend's
                # cases in one process and fabricate attribution. Record
                # INCOMPLETE instead (fail-closed; the manifest self-test
                # also requires a non-empty driver_case for every backend).
                self.shared_by_backend[b.name] = RunResult(
                    f"{ev.evidence_id}:{b.name}", ev.target, INCOMPLETE,
                    detail="empty driver_case: shared suite not driven "
                           "for this backend")
                continue
            rc, out = xmake_run_target(ev.target, env_filter=b.driver_case)
            state, detail = self._classify_shared_run(b, rc, out)
            self.shared_by_backend[b.name] = RunResult(
                f"{ev.evidence_id}:{b.name}", ev.target, state,
                detail=detail, stdout=out)

    def _run_capacity_suite(self, ev: M.Evidence) -> None:
        """Phase C2a: drive the shared capacity suite once per backend that has
        a capacity driver case, each in its own subprocess. Uring real mode runs
        the shared semantics; its stub branch is build/API evidence and becomes
        INCOMPLETE, never PASS. Uses the same preflight shape as
        _run_shared_suite (target existence + build).
        """
        if not xmake_target_exists(ev.target):
            missing = RunResult(ev.evidence_id, ev.target, MISSING_TARGET,
                                detail="xmake show -t reports not a valid target")
            for b in M.BACKENDS:
                if b.capacity_driver_case:
                    self.capacity_by_backend[b.name] = missing
            return
        if self.args is not None and not getattr(self.args, "no_build", False):
            ok, log = xmake_build_target(ev.target)
            if not ok:
                bf = RunResult(ev.evidence_id, ev.target, BUILD_FAIL,
                               detail="xmake build failed", stdout=log)
                for b in M.BACKENDS:
                    if b.capacity_driver_case:
                        self.capacity_by_backend[b.name] = bf
                return

        # One isolated subprocess per backend with a capacity driver case. The
        # capacity cases assert ONLY AsyncIoContext-observable state, so a run
        # is PASS only when it provably ran exactly the driver case and emitted
        # exactly one valid [conformance-meta] line (same fail-closed shape as
        # the shared suite). A run that reports a failing capacity case prints
        # "[conformance] capacity FAIL <backend> :: <case>" and exits non-zero
        # -> RUN_FAIL for that backend only.
        for b in M.BACKENDS:
            if not b.capacity_driver_case:
                continue
            rc, out = xmake_run_target(ev.target,
                                       env_filter=b.capacity_driver_case)
            state, detail = self._classify_shared_run(
                b, rc, out, expected_case=b.capacity_driver_case)
            if state == PASS and b.profile == "KernelIoProfile":
                metas = parse_meta_line_list(out)
                if len(metas) == 1 and metas[0][2] != "real":
                    state = INCOMPLETE
                    detail = (f"capacity execution requires mode='real'; got "
                              f"mode={metas[0][2]!r} (build/API evidence only)")
            self.capacity_by_backend[b.name] = RunResult(
                f"{ev.evidence_id}:{b.name}", ev.target, state,
                detail=detail, stdout=out)

    def _run_close_drain_suite(self, ev: M.Evidence) -> None:
        """Phase C2e: drive the shared close/drain suite once per backend that
        HAS a close_admission driver case (Fake, ThreadPool), each in its own
        subprocess. Backends without the seam (Uring before D4) have no
        close/drain driver case and are skipped here — their gap is the
        manifest's uring_c2e_close_drain_not_implemented record, surfaced in
        the verdict via applicable_evidence_for_backend(). Uses the same
        preflight shape as _run_shared_suite / _run_capacity_suite (target
        existence + build once).
        """
        if not xmake_target_exists(ev.target):
            missing = RunResult(ev.evidence_id, ev.target, MISSING_TARGET,
                                detail="xmake show -t reports not a valid target")
            for b in M.BACKENDS:
                if b.close_drain_driver_case:
                    self.close_drain_by_backend[b.name] = missing
            return
        if self.args is not None and not getattr(self.args, "no_build", False):
            ok, log = xmake_build_target(ev.target)
            if not ok:
                bf = RunResult(ev.evidence_id, ev.target, BUILD_FAIL,
                               detail="xmake build failed", stdout=log)
                for b in M.BACKENDS:
                    if b.close_drain_driver_case:
                        self.close_drain_by_backend[b.name] = bf
                return

        # One isolated subprocess per backend with a close/drain driver case.
        # Same fail-closed classification as the shared/capacity suites: PASS
        # only when the run provably executed exactly the driver case and
        # emitted exactly one valid [conformance-meta] line. A run that
        # reports a failing close/drain case prints
        # "[conformance] close/drain FAIL <backend> :: <case>" and exits
        # non-zero -> RUN_FAIL for that backend only.
        for b in M.BACKENDS:
            if not b.close_drain_driver_case:
                continue  # Uring: no close seam; gap is the manifest record.
            rc, out = xmake_run_target(ev.target,
                                       env_filter=b.close_drain_driver_case)
            state, detail = self._classify_shared_run(
                b, rc, out, expected_case=b.close_drain_driver_case)
            self.close_drain_by_backend[b.name] = RunResult(
                f"{ev.evidence_id}:{b.name}", ev.target, state,
                detail=detail, stdout=out)

    def _classify_shared_run(self, backend: M.BackendEntry, rc: int, out: str,
                             expected_case: str = "") -> tuple[str, str]:
        """Classify ONE backend's isolated shared-suite subprocess run.

        Fail-closed: a run is PASS only when ALL of the following hold:

          * returncode == 0 (else RUN_FAIL);
          * the harness executed exactly the manifest's driver_case and
            nothing else (the `[run]` lines must be exactly [expected_case]);
          * exactly one [conformance-meta] line was emitted;
          * its backend, canonicalized (Uring(stub) -> Uring), equals this
            backend;
          * its profile equals the manifest-declared profile; and
          * its mode is allowed for that profile (M.PROFILE_MODES).

        Zero/extra selected cases, missing/duplicate/foreign meta, a wrong
        backend/profile, or a disallowed mode are INCOMPLETE — never PASS.
        This is what closes the "filter matched nothing still exits 0" false
        green: a typo'd or renamed driver_case can no longer report PASS.

        `expected_case` defaults to backend.driver_case (the shared suite). For
        the Phase C2a capacity suite pass backend.capacity_driver_case so the
        selected-case check uses the capacity driver case name.
        """
        if not expected_case:
            expected_case = backend.driver_case
        if rc != 0:
            return RUN_FAIL, f"exit {rc} (filter={expected_case})"
        selected = parse_run_lines(out)
        if selected != [expected_case]:
            return INCOMPLETE, (
                f"filter={expected_case!r} selected cases "
                f"{selected!r}; expected exactly [{expected_case!r}]")
        metas = parse_meta_line_list(out)
        if len(metas) != 1:
            return INCOMPLETE, (
                f"expected exactly one [conformance-meta] line, got "
                f"{len(metas)} (filter={expected_case!r})")
        meta_backend, profile, mode = metas[0]
        registered = [b.name for b in M.BACKENDS]
        if canonical_backend_key(meta_backend, registered) != backend.name:
            return INCOMPLETE, (
                f"[conformance-meta] backend={meta_backend!r} does not match "
                f"registered backend {backend.name!r}")
        if profile != backend.profile:
            return INCOMPLETE, (
                f"[conformance-meta] profile={profile!r} != manifest profile "
                f"{backend.profile!r}")
        allowed = M.PROFILE_MODES[backend.profile]
        if mode not in allowed:
            return INCOMPLETE, (
                f"[conformance-meta] mode={mode!r} not in allowed "
                f"{sorted(allowed)} for profile {backend.profile!r}")
        return PASS, (f"exit 0; selected {selected}; meta backend="
                      f"{meta_backend} profile={profile} mode={mode}")

    def _drive(self, ev: M.Evidence) -> RunResult:
        if ev.target.startswith("__script__:"):
            if not os.path.isfile(os.path.join(
                    REPO_ROOT, "scripts", ev.target[len("__script__:"):])):
                return RunResult(ev.evidence_id, ev.target, MISSING_TARGET,
                                 detail="script not found")
            rc, out = run_shell_script(ev.target)
            state = PASS if rc == 0 else RUN_FAIL
            return RunResult(ev.evidence_id, ev.target, state,
                             detail=f"exit {rc}", stdout=out)

        # xmake target path.
        if not xmake_target_exists(ev.target):
            return RunResult(ev.evidence_id, ev.target, MISSING_TARGET,
                             detail="xmake show -t reports not a valid target")

        if self.args is not None and not getattr(self.args, "no_build", False):
            ok, log = xmake_build_target(ev.target)
            if not ok:
                return RunResult(ev.evidence_id, ev.target, BUILD_FAIL,
                                 detail="xmake build failed", stdout=log)

        rc, out = xmake_run_target(ev.target)
        state = _state_for_rc(rc)
        detail = f"exit {rc}"
        if state == PASS and ev.required_modes:
            metas = parse_evidence_meta_lines(out)
            if len(metas) != 1:
                state = INCOMPLETE
                detail = ("expected exactly one [evidence-meta] line for "
                          f"{ev.evidence_id}, got {len(metas)}")
            else:
                evidence_id, mode = metas[0]
                if evidence_id != ev.evidence_id:
                    state = INCOMPLETE
                    detail = (f"[evidence-meta] evidence={evidence_id!r} does not "
                              f"match {ev.evidence_id!r}")
                elif mode not in ev.required_modes:
                    state = INCOMPLETE
                    detail = (f"[evidence-meta] mode={mode!r} not in required "
                              f"{list(ev.required_modes)!r}")
                else:
                    detail = (f"exit 0; [evidence-meta] evidence={evidence_id} "
                              f"mode={mode}")
        return RunResult(ev.evidence_id, ev.target, state,
                         detail=detail, stdout=out)

    # --- Per-backend verdict computation -----------------------------------

    def _backend_run_state(self, ev: M.Evidence, backend_name: str,
                           backend_profile: str = "") -> str:
        """The run state of an evidence record FROM THE PERSPECTIVE of one
        backend.

        Per-backend isolation (the C1 corrective): the shared suite is driven
        once PER backend in a separate subprocess (see _run_shared_suite), so a
        backend's shared-suite state is read from THIS backend's own subprocess
        result, never inferred from a single shared exit code. One backend's
        RUN_FAIL therefore cannot become another backend's state.

        IMPORTANT: backend-agnostic arena/lifecycle evidence proves the
        RequestSlot CONTRACT, NOT that a given backend conforms to it. Uring
        completed the D1 RequestArena migration, but its D3 identity/cancel/
        borrow/waiter and D4 close/drain evidence are still incomplete. We
        therefore report generic KernelIo lifecycle/backend-specific records
        as INCOMPLETE unless a narrowly tagged real-mode phase record has its
        own command-backed PASS. The hard-coded overall KernelIo verdict stays
        fail-closed until D4.
        """
        # The shared suite: this backend's OWN subprocess result.
        if ev.evidence_id == "shared_suite":
            r = self.shared_by_backend.get(backend_name)
            if r is None:
                return NOT_RUN
            return r.state

        # Phase C2a: the shared CAPACITY suite — this backend's OWN subprocess
        # result (conformance_capacity_fake / conformance_capacity_threadpool).
        if ev.evidence_id == "shared_capacity_suite":
            r = self.capacity_by_backend.get(backend_name)
            if r is None:
                # A backend with a capacity seam that the gate never drove is
                # a harness error.
                return NOT_RUN
            return r.state

        # Phase C2e: the shared CLOSE/DRAIN suite — this backend's OWN
        # subprocess result (conformance_close_drain_fake /
        # conformance_close_drain_threadpool).
        if ev.evidence_id == "c2e_shared_close_drain_suite":
            r = self.close_drain_by_backend.get(backend_name)
            if r is None:
                # A backend with a close/drain seam that the gate never drove:
                # NOT_RUN (a harness error). A backend with NO seam (Uring)
                # has no applicable implemented record (only the
                # uring_c2e_close_drain_not_implemented not_implemented
                # record), so this branch is not reached for it.
                return NOT_RUN
            return r.state

        r = self.results.get(ev.evidence_id)
        if r is None:
            return NOT_RUN
        if r.state in (MISSING_TARGET, BUILD_FAIL, NOT_RUN):
            return r.state

        # KernelIoProfile lifecycle/backend_specific evidence remains
        # INCOMPLETE by default while D3/D4 are open. A narrowly tagged,
        # real-only phase record may report its own PASS after _drive() validates
        # the required evidence mode; this does not lift the hard-coded overall
        # KernelIo NOT CONFORMING verdict below.
        if (backend_profile == "KernelIoProfile"
                and ev.layer in ("lifecycle", "backend_specific")):
            if ev.required_modes:
                return r.state
            # The uring-specific backend contract target (uring_backend_test)
            # runs against the stub/real binary; in stub it covers only the
            # stub subset, so it is INCOMPLETE for the kernel profile.
            return INCOMPLETE

        return r.state

    def _backend_verdict(self, backend: M.BackendEntry) -> tuple[str, list[str]]:
        """Compute (verdict, reasons) for one backend.

        Priority (P2 corrective) over the backend's applicable MANDATORY
        evidence:
          1. any RUN_FAIL                                -> NOT CONFORMING
             (evidence proves a violation);
          2. else any MISSING_TARGET / BUILD_FAIL /
             NOT_RUN / INCOMPLETE                        -> INCOMPLETE
             (insufficient evidence — never ELIGIBLE);
          3. else all PASS / legal NOT_APPLICABLE and
             every mandatory layer has an applicable
             record                                      -> ELIGIBLE.

        Non-mandatory evidence is diagnostic only: it can neither satisfy a
        mandatory layer nor block ELIGIBLE. A layer with one PASS and one
        MISSING_TARGET is INCOMPLETE, not ELIGIBLE — one PASS per layer is
        not enough.

        The verdict iterates applicable_evidence_for_backend()
        (implemented + not_implemented + not_applicable), so a not_implemented
        MANDATORY record forces INCOMPLETE in the backend's OWN verdict, not
        just in the global results dict. This is how remaining D3/D4 known
        gaps surface honestly.
        """
        reasons: list[str] = []
        # self.meta is keyed by canonical registered backend names (see run()).
        mode = self.meta.get(backend.name, {})
        mode_str = mode.get("mode", "unknown")

        # KernelIo profile remains fail-closed until D4 lifts this rule.
        if backend.profile == "KernelIoProfile":
            if mode_str == "stub":
                reasons.append("kernel profile built as stub (real execution unavailable)")
            elif mode_str == "real":
                reasons.append("kernel profile gate remains fail-closed "
                               "(D3 identity/cancel/borrow/waiter and D4 "
                               "wait/close/drain remain pending)")
            else:
                reasons.append(f"kernel profile mode={mode_str} (Phase D incomplete)")
            # Still enumerate applicable not_implemented mandatory records so a
            # remaining Phase-D gaps appear in the reasons for the report,
            # reinforcing (not replacing) the KernelIo NOT CONFORMING rule.
            for ev in M.applicable_evidence_for_backend(backend.name):
                if ev.mandatory and ev.status == M.STATUS_NOT_IMPLEMENTED:
                    reasons.append(
                        f"known gap: mandatory evidence '{ev.evidence_id}' "
                        f"({ev.target}): not_implemented")
            return NOT_CONFORMING, reasons

        # Mandatory APPLICABLE evidence only (implemented + not_implemented +
        # not_applicable). Non-mandatory records are diagnostic.
        states = [
            (ev, self._backend_run_state(ev, backend.name, backend.profile))
            for ev in M.applicable_evidence_for_backend(backend.name)
            if ev.mandatory
        ]

        # Priority 1: a mandatory evidence proved a violation.
        failed = [(ev, s) for ev, s in states if s == RUN_FAIL]
        if failed:
            for ev, s in failed:
                reasons.append(
                    f"mandatory evidence '{ev.evidence_id}' ({ev.target}): {s}")
            return NOT_CONFORMING, reasons

        # Priority 2: insufficient evidence (missing target, build failure,
        # not run, or stub-subset-only) — INCOMPLETE, never ELIGIBLE.
        insufficient = [
            (ev, s) for ev, s in states
            if s in (MISSING_TARGET, BUILD_FAIL, NOT_RUN, INCOMPLETE)
        ]
        if insufficient:
            for ev, s in insufficient:
                reasons.append(
                    f"mandatory evidence '{ev.evidence_id}' ({ev.target}): {s}")
            return INCOMPLETE, reasons

        # Priority 3: every mandatory layer must actually have applicable
        # evidence (all remaining states are PASS / legal NOT_APPLICABLE).
        for layer in M.MANDATORY_LAYERS_PER_BACKEND:
            if not any(ev.layer == layer for ev, _ in states):
                reasons.append(f"no evidence records for mandatory layer '{layer}'")
                return INCOMPLETE, reasons
        return ELIGIBLE, reasons

    # --- Reporting ---------------------------------------------------------

    def _report(self) -> int:
        overall_failures: list[str] = []
        print("=" * 72)
        print("Explicit-I/O Backend Conformance Gate (Phase C1)")
        print("=" * 72)
        print()

        # --- Per-backend report ---
        for backend in M.BACKENDS:
            # self.meta is keyed by canonical registered backend names.
            mode_info = self.meta.get(backend.name, {})
            profile_seen = mode_info.get("profile", "?")
            mode_seen = mode_info.get("mode", "?")
            verdict, reasons = self._backend_verdict(backend)

            print(f"Backend: {backend.name} ({backend.profile})")
            print(f"  mode (from meta): {mode_seen}  profile (from meta): {profile_seen}")
            for layer in M.MANDATORY_LAYERS_PER_BACKEND:
                # Phase C2a: report APPLICABLE evidence (implemented +
                # not_implemented + not_applicable) so a known gap surfaces in
                # the per-backend section, not just in the verdict reasons.
                #
                # Compute each applicable evidence's run state ONCE and reuse it
                # for both the per-layer summary and the per-evidence detail
                # lines. A previous version called _backend_run_state up to
                # three times per evidence (once for the states set, twice for
                # the detail loop) and relied on a tautological
                # `elif st not in ("", )` branch (the helper never returns "")
                # that duplicated the default detail line.
                ev_states = [
                    (ev, self._backend_run_state(ev, backend.name, backend.profile))
                    for ev in M.applicable_evidence_for_backend(backend.name)
                    if ev.layer == layer
                ]
                states = sorted({st for _, st in ev_states})
                label = layer.replace("_", " ")
                # For KernelIoProfile in a stub build, the shared suite covers
                # only the stub subset — relabel honestly. If a not_implemented
                # gap (e.g. capacity) is also in the layer, show it alongside.
                layer_parts: list[str] = []
                for ev, st in ev_states:
                    if (backend.profile == "KernelIoProfile"
                            and layer == "shared" and mode_seen == "stub"
                            and ev.evidence_id == "shared_suite" and st == PASS):
                        layer_parts.append(f"{ev.evidence_id}=PASS (stub subset)")
                    elif ev.status == M.STATUS_NOT_IMPLEMENTED:
                        layer_parts.append(
                            f"{ev.evidence_id}=INCOMPLETE (not_implemented)")
                    else:
                        layer_parts.append(f"{ev.evidence_id}={st}")
                summary = states[0] if len(states) == 1 else "/".join(states)
                print(f"  {label:<22} {summary}")
                for part in layer_parts:
                    print(f"    {part}")
            print(f"  overall               {verdict}")
            for r in reasons:
                print(f"    reason: {r}")
            print()

            if verdict != ELIGIBLE:
                # KernelIo NOT CONFORMING is EXPECTED in C1 (not a gate failure),
                # but a Reference/BlockingIo NOT CONFORMING IS a gate failure.
                if backend.profile != "KernelIoProfile":
                    overall_failures.append(
                        f"{backend.name} ({backend.profile}): {verdict} — "
                        + "; ".join(reasons))

        # --- External admission probe (NOT conformance) ---
        ext = self.results.get("external_backend_admission")
        print("External backend probe (admission, NOT conformance)")
        if ext:
            print(f"  extension admission    {ext.state}")
        else:
            print(f"  extension admission    {NOT_RUN}")
        auth_ext = self.results.get("authority_external_backend_negative_compile")
        if auth_ext:
            print(f"  authority boundary     {auth_ext.state}")
        else:
            print(f"  authority boundary     {NOT_RUN}")
        print("  conformance            NOT ASSESSED")
        print()
        if ext and ext.state not in SATISFACTORY:
            overall_failures.append(
                f"external backend admission: {ext.state}")
        if auth_ext and auth_ext.state not in SATISFACTORY:
            overall_failures.append(
                f"external backend authority: {auth_ext.state}")

        # --- Mandatory target-level failures (any backend) ---
        # Non-shared evidence is global (one result per evidence id). The shared
        # suite is per-backend (see _run_shared_suite), so its mandatory
        # failures are attributed to the SPECIFIC backend that failed — never
        # propagated to the other backends. This is the closed attribution
        # model: a single mandatory issue names exactly the backend/evidence
        # that failed.
        for ev in M.EVIDENCE:
            if ev.status != M.STATUS_IMPLEMENTED or not ev.mandatory:
                continue
            if ev.evidence_id == "shared_suite":
                # Per-backend shared suite: attribute each backend's own state.
                for b in M.BACKENDS:
                    rr = self.shared_by_backend.get(b.name)
                    if rr and rr.state in (MISSING_TARGET, BUILD_FAIL,
                                           RUN_FAIL):
                        overall_failures.append(
                            f"mandatory evidence '{ev.evidence_id}' "
                            f"backend {b.name} ({ev.target}): {rr.state}")
                continue
            if ev.evidence_id == "shared_capacity_suite":
                # Phase C2a: per-backend shared CAPACITY suite. Real Uring is
                # driven after D1; its stub branch is classified INCOMPLETE.
                for b in M.BACKENDS:
                    if not b.capacity_driver_case:
                        continue
                    rr = self.capacity_by_backend.get(b.name)
                    if rr and rr.state in (MISSING_TARGET, BUILD_FAIL,
                                           RUN_FAIL):
                        overall_failures.append(
                            f"mandatory evidence '{ev.evidence_id}' "
                            f"backend {b.name} ({ev.target}): {rr.state}")
                continue
            if ev.evidence_id == "c2e_shared_close_drain_suite":
                # Phase C2e: per-backend shared CLOSE/DRAIN suite. Only
                # backends with a close_admission seam (Fake, ThreadPool) are
                # driven; Uring's gap is the
                # uring_c2e_close_drain_not_implemented record, handled by
                # applicable_evidence_for_backend in the verdict.
                for b in M.BACKENDS:
                    if not b.close_drain_driver_case:
                        continue
                    rr = self.close_drain_by_backend.get(b.name)
                    if rr and rr.state in (MISSING_TARGET, BUILD_FAIL,
                                           RUN_FAIL):
                        overall_failures.append(
                            f"mandatory evidence '{ev.evidence_id}' "
                            f"backend {b.name} ({ev.target}): {rr.state}")
                continue
            r = self.results.get(ev.evidence_id)
            if r is None:
                overall_failures.append(
                    f"mandatory evidence '{ev.evidence_id}' "
                    f"({ev.target}): {NOT_RUN} (not evaluated)")
            elif r.state in (MISSING_TARGET, BUILD_FAIL, RUN_FAIL):
                overall_failures.append(
                    f"mandatory evidence '{ev.evidence_id}' "
                    f"({ev.target}): {r.state}")

        # Fail closed: every registered backend MUST have been evaluated for the
        # shared suite. A missing shared_by_backend entry means the gate did not
        # run that backend — that is a harness error, not a benign skip.
        for b in M.BACKENDS:
            if b.name not in self.shared_by_backend:
                overall_failures.append(
                    f"registered backend {b.name} shared-suite result MISSING "
                    f"(gate must evaluate every registered backend)")

        # Phase C2a fail-closed: every backend with a capacity seam MUST have a
        # capacity-suite result. A missing capacity_by_backend entry for a
        # backend that declares a capacity_driver_case is a harness error.
        for b in M.BACKENDS:
            if b.capacity_driver_case and b.name not in self.capacity_by_backend:
                overall_failures.append(
                    f"registered backend {b.name} capacity-suite result MISSING "
                    f"(gate must evaluate every capacity-capable backend)")

        # Phase C2e fail-closed: every backend with a close_admission seam MUST
        # have a close/drain-suite result. A missing close_drain_by_backend
        # entry for a backend that declares a close_drain_driver_case is a
        # harness error.
        for b in M.BACKENDS:
            if b.close_drain_driver_case and b.name not in self.close_drain_by_backend:
                overall_failures.append(
                    f"registered backend {b.name} close/drain-suite result "
                    f"MISSING (gate must evaluate every close-capable backend)")

        # --- Summary ---
        print("-" * 72)
        if overall_failures:
            print(f"RESULT: FAIL ({len(overall_failures)} mandatory issue(s))")
            for f in overall_failures:
                print(f"  - {f}")
            return 1
        print("RESULT: PASS (all runnable mandatory gates satisfied; "
              "KernelIo remains NOT CONFORMING until D3/D4 close).")
        return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--no-build", action="store_true",
                    help="skip `xmake build` (assume targets already built)")
    args = ap.parse_args()

    # Sanity: xmake available?
    rc, _, _ = run_cmd(["xmake", "--version"], timeout=30)
    if rc == 127:
        print("FATAL: xmake not found on PATH", file=sys.stderr)
        return 2

    gate = Gate(args=args)
    return gate.run()


if __name__ == "__main__":
    sys.exit(main())
