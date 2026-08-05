#!/usr/bin/env python3
# verify-backend-conformance.py
#
# Phase C1 aggregate backend-conformance gate.
#
# Reads scripts/backend_conformance_manifest.py and produces an honest per-
# backend, per-section report. It:
#   * preflights each target via `xmake show -t` (target existence) then
#     `xmake build` (build) then `xmake run` (run) — NO Lua-source grep;
#   * parses ONLY the stable [conformance-meta] lines (backend/profile/mode)
#     from the shared-suite driver output — never display names, skip text,
#     "PASS" strings, or build-directory contents;
#   * classifies each backend's verdict (ELIGIBLE / INCOMPLETE / NOT
#     CONFORMING) from the manifest's closed profiles and the run results;
#   * exits non-zero on any mandatory MISSING_TARGET / BUILD_FAIL / RUN_FAIL,
#     or when a registered backend's mandatory case-set is not covered.
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
#       failure, or a backend's mandatory case-set uncovered).
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
ELIGIBLE = "ELIGIBLE"
NOT_CONFORMING = "NOT CONFORMING"


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
    var so the in-binary test harness runs ONLY the comma/space-separated
    case-name substrings it names (see tests/harness.hpp). This is how the
    aggregate gate drives each registered backend's shared-suite case in a
    SEPARATE subprocess: one backend's failure cannot affect another
    backend's process exit code or [conformance-meta] emission.
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

# Match the display name even when it contains "(stub)" — backend= value is
# captured greedily up to whitespace, so "Uring(stub)" is captured whole.


def parse_meta_lines(driver_output: str) -> dict[str, dict[str, str]]:
    """Parse [conformance-meta] lines into {backend: {profile, mode}}.

    backend is the value after backend= EXACTLY as printed (e.g. "Uring(stub)").
    """
    meta: dict[str, dict[str, str]] = {}
    for line in driver_output.splitlines():
        m = META_RE.match(line.strip())
        if m:
            backend, profile, mode = m.group(1), m.group(2), m.group(3)
            meta[backend] = {"profile": profile, "mode": mode}
    return meta


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
            # The shared suite is driven per-backend in _run_shared_suite below.
            if ev.evidence_id == "shared_suite":
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

        # Parse [conformance-meta] from every per-backend shared run; each run
        # emits exactly one meta line for its own backend.
        self.meta = {}
        for name, rr in self.shared_by_backend.items():
            if rr.stdout:
                self.meta.update(parse_meta_lines(rr.stdout))

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
        for b in M.BACKENDS:
            rc, out = xmake_run_target(ev.target, env_filter=b.driver_case)
            self.shared_by_backend[b.name] = RunResult(
                f"{ev.evidence_id}:{b.name}", ev.target, _state_for_rc(rc),
                detail=f"exit {rc} (filter={b.driver_case})", stdout=out)

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

        if not self.args.no_build:
            ok, log = xmake_build_target(ev.target)
            if not ok:
                return RunResult(ev.evidence_id, ev.target, BUILD_FAIL,
                                 detail="xmake build failed", stdout=log)

        rc, out = xmake_run_target(ev.target)
        state = _state_for_rc(rc)
        return RunResult(ev.evidence_id, ev.target, state,
                         detail=f"exit {rc}", stdout=out)

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
        RequestSlot CONTRACT, NOT that a given backend conforms to it. The
        Uring KernelIo backend has NOT been migrated onto RequestArena (Phase D
        pending), so it MUST NOT claim lifecycle/backend_specific PASS — the
        contract exists, but Uring does not yet implement it. We report
        INCOMPLETE for KernelIoProfile lifecycle/backend_specific regardless of
        the arena tests passing, because those tests do not exercise Uring.
        """
        # The shared suite: this backend's OWN subprocess result.
        if ev.evidence_id == "shared_suite":
            r = self.shared_by_backend.get(backend_name)
            if r is None:
                return NOT_RUN
            return r.state

        r = self.results.get(ev.evidence_id)
        if r is None:
            return NOT_RUN
        if r.state in (MISSING_TARGET, BUILD_FAIL, NOT_RUN):
            return r.state

        # KernelIoProfile lifecycle/backend_specific: the contract evidence is
        # real but Uring does not implement it yet. Never report PASS.
        if (backend_profile == "KernelIoProfile"
                and ev.layer in ("lifecycle", "backend_specific")):
            # The uring-specific backend contract target (uring_backend_test)
            # runs against the stub/real binary; in stub it covers only the
            # stub subset, so it is INCOMPLETE for the kernel profile.
            return INCOMPLETE

        return r.state

    def _backend_verdict(self, backend: M.BackendEntry) -> tuple[str, list[str]]:
        """Compute (verdict, reasons) for one backend."""
        reasons: list[str] = []
        mode = self.meta.get(backend.name) or self.meta.get(
            self._meta_name_for(backend.name), {})
        mode_str = mode.get("mode", "unknown")

        # KernelIo profile: never ELIGIBLE in C1 (Phase D not implemented).
        if backend.profile == "KernelIoProfile":
            if mode_str == "stub":
                reasons.append("kernel profile built as stub (Phase D not implemented)")
            elif mode_str == "real":
                reasons.append("kernel profile real-path migration NOT IMPLEMENTED "
                               "(Phase D: RequestArena/RequestKey identity)")
            else:
                reasons.append(f"kernel profile mode={mode_str} (Phase D pending)")
            return NOT_CONFORMING, reasons

        # Reference / BlockingIo: must have PASS in every mandatory layer.
        for layer in M.MANDATORY_LAYERS_PER_BACKEND:
            applicable = [
                ev for ev in M.evidence_for_backend(backend.name)
                if ev.layer == layer and ev.status == M.STATUS_IMPLEMENTED
            ]
            if not applicable:
                reasons.append(f"no evidence records for mandatory layer '{layer}'")
                continue
            layer_states = {
                self._backend_run_state(ev, backend.name, backend.profile)
                for ev in applicable
            }
            if not (layer_states & SATISFACTORY):
                reasons.append(
                    f"mandatory layer '{layer}' not satisfied: {sorted(layer_states)}")
            elif RUN_FAIL in layer_states and not (layer_states & {PASS}):
                reasons.append(f"mandatory layer '{layer}' has failures")
        # Any mandatory lifecycle/backend_specific target failed -> not eligible.
        any_fail = any(
            self._backend_run_state(ev, backend.name, backend.profile) == RUN_FAIL
            for ev in M.evidence_for_backend(backend.name)
        )
        if reasons or any_fail:
            return NOT_CONFORMING, reasons
        return ELIGIBLE, reasons

    @staticmethod
    def _meta_name_for(registered_name: str) -> str:
        # The driver prints "Uring(stub)" in stub builds; the meta dict is
        # keyed by whatever was printed. Provide the stub-variant lookup.
        return registered_name + "(stub)" if registered_name == "Uring" else registered_name

    # --- Reporting ---------------------------------------------------------

    def _report(self) -> int:
        overall_failures: list[str] = []
        print("=" * 72)
        print("Explicit-I/O Backend Conformance Gate (Phase C1)")
        print("=" * 72)
        print()

        # --- Per-backend report ---
        for backend in M.BACKENDS:
            mode_info = self.meta.get(backend.name) or self.meta.get(
                self._meta_name_for(backend.name), {})
            profile_seen = mode_info.get("profile", "?")
            mode_seen = mode_info.get("mode", "?")
            verdict, reasons = self._backend_verdict(backend)

            print(f"Backend: {backend.name} ({backend.profile})")
            print(f"  mode (from meta): {mode_seen}  profile (from meta): {profile_seen}")
            for layer in M.MANDATORY_LAYERS_PER_BACKEND:
                applicable = [
                    ev for ev in M.evidence_for_backend(backend.name)
                    if ev.layer == layer and ev.status == M.STATUS_IMPLEMENTED
                ]
                states = sorted({
                    self._backend_run_state(ev, backend.name, backend.profile)
                    for ev in applicable
                })
                label = layer.replace("_", " ")
                # For KernelIoProfile in a stub build, the shared suite covers
                # only the stub subset — relabel honestly.
                if (backend.profile == "KernelIoProfile" and layer == "shared"
                        and mode_seen == "stub" and PASS in states):
                    print(f"  {label:<22} PASS (stub subset)")
                else:
                    print(f"  {label:<22} {states[0] if len(states)==1 else '/'.join(states)}")
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

        # --- Summary ---
        print("-" * 72)
        if overall_failures:
            print(f"RESULT: FAIL ({len(overall_failures)} mandatory issue(s))")
            for f in overall_failures:
                print(f"  - {f}")
            return 1
        print("RESULT: PASS (all mandatory gates satisfied; "
              "KernelIo NOT CONFORMING is expected before Phase D).")
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
