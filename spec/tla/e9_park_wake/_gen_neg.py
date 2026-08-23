#!/usr/bin/env python3
"""Generate the duplicated-snapshot E9 negatives from the correct E9ParkWake.

#192: two E9 negative controls are FULL DUPLICATED SNAPSHOTS of the positive
model (NegRetireDead: 1029 lines, BuggyNoBridge: 994 lines), so every
positive-model edit leaves them silently stale -- BuggyNoBridge had already
frozen at the pre-#190 model. This generator keeps each negative a
single-rule mutation expressed against the CURRENT positive model, in the
e12_async_mutex `_gen_neg.py` style but with rwlock-style exact-fragment
swaps (the mutation is declared as anchor-fragment -> replacement, not as a
full copied action body, so a future positive-model edit cannot leave the
generator's own copy of the action drifting).

Committed generated artifacts (freshness-gated; `--check` runs inside
scripts/formal/verify-e9-park-wake.sh BEFORE any TLC invocation):

  E9ParkWakeNegRetireDead  (#189 fail-closed witness control)
      mutation: RetireWorkerQuiescent's UNCHANGED reacquires
      `wakeEpoch, bridgePending` while the action body still conjoins
      BridgeEffect (which primes wakeEpoch'/bridgePending') -- the exact
      pre-#189 double-prime contradiction; the action is unsatisfiable
      again and never fires.
      detectors (both must HOLD -- the witness gate fails closed):
      NoReachRetireFired, NoReachQuiescentTerminate.
      documented co-victims: WF_vars(RetireWorkerQuiescent) goes vacuous
      again (FairRetire half-dead) and the NoReachQuiescentTerminate
      witness is unreachable alongside.

  E9ParkWakeBuggyNoBridge  (Phase G model-level M1)
      mutation: BridgeEffect never arms the bridge
      (`bridgePending' = bridgePending`); wake publications advance
      wakeEpoch but a parked split-wait participant can never observe them.
      detector: Inv8BridgeReachesBackendPark.
      documented co-victim (not in this cfg): Life7ExternalReadyEventually
      Drained -- the model text itself names Life7 "the bridge's liveness
      obligation (and the model-level M1 mutation detector)".

NOT generated (kept hand-written, do not add them here):
  E9ParkWakeBuggyPrePark / E9ParkWakeBuggyMixedSource -- snapshots of the
  pre-Phase-G E9-A protocol (a structurally different model: no SplitWait,
  no bridge, no run-lifetime park domain); frozen historical controls.
  E9ParkWakeBuggyDrainParks -- already-minimal EXTENDS-based mutant that
  auto-tracks the current positive model.

Usage:
    python3 _gen_neg.py            # regenerate the 4 committed artifacts
    python3 _gen_neg.py --check    # read-only freshness verification

`--check` renders the expected bytes in memory and compares them against the
committed files; it never writes to the working tree. It FAILS (non-zero)
on: a stale generated .tla/.cfg, a missing generated file, an unexpected
E9ParkWakeNeg*/E9ParkWakeBuggyNoBridge* artifact not produced by the current
CASES, positive-model anchor drift (a mutation fragment matching zero or
multiple locations), or a malformed CASE entry.
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
POSITIVE = HERE / "E9ParkWake.tla"
POSITIVE_MODULE = "E9ParkWake"
MODULE_DASHES = "-" * 31
ARTIFACT_GLOBS = ("E9ParkWakeNeg*", "E9ParkWakeBuggyNoBridge*")

CONSTANTS = """\
CONSTANTS
    Workers = {W0, W1}
    Fibers = {F0}
    W0 = W0
    W1 = W1
    F0 = F0
    NONE = NONE
    SplitWait = TRUE
"""


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


# --------------------------------------------------------------------------
# Mutation fragments (exact source text -> replacement). Each fragment must
# match the positive model EXACTLY ONCE; zero or multiple matches is
# positive-model anchor drift and aborts generation fail-closed.
# --------------------------------------------------------------------------

# NEG #189 dead retire: the pre-#189 RetireWorkerQuiescent pinned wakeEpoch
# and bridgePending in UNCHANGED while conjoining BridgeEffect, making the
# action unsatisfiable (wakeEpoch in {0,1}, BridgeEffect assigns
# wakeEpoch' = 1 - wakeEpoch # wakeEpoch). Restoring the pin kills the
# action again. (#191 note: the UNCHANGED tail now also carries the
# participant-exit witness ghosts -- they stay pinned by the replacement,
# exactly as the current model carries them.)
NEG_RETIRE_DEAD = (
    "                   workerPhase, observedEpoch,\n"
    "                   backendWaitParticipant, idleCount, runMode,\n"
    "                   participantExitFired, participantExitEndedRun>>",
    "                   wakeEpoch, bridgePending, workerPhase, observedEpoch,\n"
    "                   backendWaitParticipant, idleCount, runMode,\n"
    "                   participantExitFired, participantExitEndedRun>>",
)

# NEG #191 dead participant exit: the pre-#191 ParticipantNoProgressExit
# conjoined BridgeEffect (whose bridge branch primes bridgePending' = TRUE
# while a participant exists) while its own body assigned bridgePending' =
# FALSE (the one-shot consume) -- a double-prime contradiction that made the
# action unsatisfiable. Restoring BridgeEffect in place of the repaired
# direct epoch advance kills the action again.
NEG_PARTICIPANT_DEAD = (
    "    /\\ wakeEpoch' = 1 - wakeEpoch\n"
    "    /\\ backendWaitParticipant' = NONE\n"
    "    /\\ bridgePending' = FALSE",
    "    /\\ BridgeEffect(1 - wakeEpoch)\n"
    "    /\\ backendWaitParticipant' = NONE\n"
    "    /\\ bridgePending' = FALSE",
)

# Phase G M1 bridge-disabled: BridgeEffect keeps the epoch advance but never
# arms the bridge.
NEG_NO_BRIDGE = (
    "    /\\ bridgePending' = IF BridgeFiresFromParticipant THEN TRUE\n"
    "                        ELSE bridgePending",
    "    /\\ bridgePending' = bridgePending",
)

CASES = [
    {
        "module": "E9ParkWakeNegRetireDead",
        "desc": (
            "GENERATED NEGATIVE (#189 fail-closed witness control): the EXACT\n"
            "  pre-fix defect -- RetireWorkerQuiescent reacquires `wakeEpoch,\n"
            "  bridgePending` in its UNCHANGED while its body conjoins BridgeEffect\n"
            "  (which primes wakeEpoch'/bridgePending'), so the action is\n"
            "  unsatisfiable and never fires. Expected TLC verdict: PASS with\n"
            "  NoReachRetireFired / NoReachQuiescentTerminate both HOLDING -- the\n"
            "  permanent witness gate fails closed (a reintroduced dead retire is\n"
            "  detected). Documented co-victims: WF_vars(RetireWorkerQuiescent)\n"
            "  vacuous again, the quiescent-terminal witness unreachable. Every\n"
            "  other rule is the current E9ParkWake verbatim."
        ),
        "mutations": [NEG_RETIRE_DEAD],
        "cfg_comment": (
            "#189 fail-closed witness control (generated). The exact pre-fix\n"
            "dead-retire contradiction: both witnesses HOLD (PASS) -- a\n"
            "reintroduced dead retire is detected by the witness gate going\n"
            "green. See E9ParkWakeWitnessRetire.cfg for the violated side."
        ),
        "invariants": ["NoReachRetireFired", "NoReachQuiescentTerminate"],
    },
    {
        "module": "E9ParkWakeBuggyNoBridge",
        "desc": (
            "GENERATED NEGATIVE (Phase G model-level M1, bridge disabled):\n"
            "  BridgeEffect advances wakeEpoch but never arms bridgePending, so\n"
            "  every Scheduler wake publication strands a parked split-wait\n"
            "  backend participant. Expected TLC verdict: VIOLATION of\n"
            "  Inv8BridgeReachesBackendPark. Documented co-victim (liveness, not\n"
            "  in this cfg): Life7ExternalReadyEventuallyDrained -- the model\n"
            "  text names Life7 the model-level M1 mutation detector. Every other\n"
            "  rule is the current E9ParkWake verbatim."
        ),
        "mutations": [NEG_NO_BRIDGE],
        "cfg_comment": (
            "Phase G M1 negative control (generated): bridge disabled.\n"
            "Expected: Inv8BridgeReachesBackendPark VIOLATED."
        ),
        "invariants": ["Inv8BridgeReachesBackendPark"],
    },
    {
        "module": "E9ParkWakeNegParticipantDead",
        "desc": (
            "GENERATED NEGATIVE (#191 fail-closed witness control): the EXACT\n"
            "  pre-#191 defect -- ParticipantNoProgressExit conjoins BridgeEffect\n"
            "  (whose bridge branch primes bridgePending' = TRUE while a\n"
            "  participant exists) while its own body assigns bridgePending' =\n"
            "  FALSE (the one-shot consume), so the double-prime contradiction\n"
            "  makes the action unsatisfiable and it never fires. Expected TLC\n"
            "  verdict: PASS with NoReachParticipantExitFired /\n"
            "  NoReachPnpExitEndedRun both HOLDING -- the permanent witness gate\n"
            "  fails closed (a reintroduced dead participant is detected).\n"
            "  Documented co-victim: WF_vars(ParticipantNoProgressExit) goes\n"
            "  vacuous again (FairRetire half-dead on the participant side).\n"
            "  Every other rule is the current E9ParkWake verbatim."
        ),
        "mutations": [NEG_PARTICIPANT_DEAD],
        "cfg_comment": (
            "#191 fail-closed witness control (generated). The exact pre-fix\n"
            "dead-participant double-prime contradiction: both witnesses HOLD\n"
            "(PASS) -- a reintroduced dead participant exit is detected by the\n"
            "witness gate going green. See E9ParkWakeWitnessPnpExit.cfg and\n"
            "E9ParkWakeWitnessPnpEndedRun.cfg for the violated side."
        ),
        "invariants": ["NoReachParticipantExitFired", "NoReachPnpExitEndedRun"],
    },
]


def validate_cases() -> None:
    seen: set[str] = set()
    for case in CASES:
        mod = case["module"]
        if not mod.startswith(POSITIVE_MODULE):
            fail(f"malformed CASE metadata: module {mod!r} does not extend "
                 f"the positive module name")
        if mod in seen:
            fail(f"unexpected CASE duplication: {mod} appears twice in CASES")
        seen.add(mod)
        if not case["desc"] or not case["mutations"] or not case["invariants"]:
            fail(f"malformed CASE metadata: empty field in {mod}")
        if not case["cfg_comment"]:
            fail(f"malformed CASE metadata: empty cfg comment in {mod}")
        for anchor, replacement in case["mutations"]:
            if anchor == replacement:
                fail(f"malformed CASE metadata: no-op mutation in {mod}")


def render_case(case: dict, positive_text: str) -> dict:
    """Render one negative into its expected artifact bytes (in memory)."""
    mod = case["module"]
    if positive_text.count(f"MODULE {POSITIVE_MODULE} ") != 1:
        fail(f"positive-model anchor drift: 'MODULE {POSITIVE_MODULE} ' found "
             f"{positive_text.count(f'MODULE {POSITIVE_MODULE} ')} times "
             f"(expected exactly 1) in {POSITIVE.name}")
    body = positive_text[positive_text.index("EXTENDS"):]
    for anchor, replacement in case["mutations"]:
        n = body.count(anchor)
        if n != 1:
            fail(f"positive-model anchor drift: mutation fragment for {mod} "
                 f"matched {n} locations (expected exactly 1):\n{anchor!r}")
        body = body.replace(anchor, replacement, 1)
    tla = (
        f"{MODULE_DASHES} MODULE {mod} {MODULE_DASHES}\n"
        "(*\n"
        "  GENERATED ARTIFACT -- DO NOT EDIT.\n"
        "  Regenerate with: python3 spec/tla/e9_park_wake/_gen_neg.py\n"
        "  (freshness-gated: scripts/formal/verify-e9-park-wake.sh runs\n"
        "  `_gen_neg.py --check` before any TLC run, so a stale, missing, or\n"
        "  unexpected generated artifact fails the formal gate instead of\n"
        "  silently checking an outdated mutation.)\n"
        "\n"
        f"  {case['desc']}\n"
        "*)\n"
        + body
    )
    inv_lines = "\n".join(f"    {inv}" for inv in case["invariants"])
    comment = "\n".join(
        ("\\* " + ln.strip()).rstrip() for ln in case["cfg_comment"].splitlines()
    )
    cfg = (
        f"\\* GENERATED ARTIFACT -- DO NOT EDIT (`_gen_neg.py`).\n"
        f"{comment}\n"
        "SPECIFICATION Spec\n"
        "INVARIANTS\n"
        f"{inv_lines}\n"
        "\n"
        + CONSTANTS
    )
    return {f"{mod}.tla": tla, f"{mod}.cfg": cfg}


def expected_artifacts(positive_text: str) -> dict:
    artifacts: dict[str, str] = {}
    for case in CASES:
        rendered = render_case(case, positive_text)
        clash = artifacts.keys() & rendered.keys()
        if clash:
            fail(f"unexpected CASE duplication: {sorted(clash)} rendered twice")
        artifacts.update(rendered)
    return artifacts


def write_artifacts(artifacts: dict) -> None:
    for name, content in sorted(artifacts.items()):
        (HERE / name).write_text(content, encoding="utf-8")
    print(f"regenerated {len(artifacts)} artifacts "
          f"({len(artifacts) // 2} negative modules, .tla + .cfg) in {HERE.name}/")


def check_artifacts(artifacts: dict) -> int:
    """Read-only freshness verification: byte-compare the committed files
    against the in-memory render and reject unexpected generated artifacts.
    NEVER writes to the working tree."""
    problems: list[str] = []
    expected_names = set(artifacts)
    for glob in ARTIFACT_GLOBS:
        for p in sorted(HERE.glob(glob)):
            if p.name not in expected_names:
                problems.append(f"unexpected generated artifact {p.name} "
                                f"(not produced by the current CASES)")
    for name, content in sorted(artifacts.items()):
        path = HERE / name
        if not path.is_file():
            problems.append(f"missing generated artifact {name}")
            continue
        if path.read_text(encoding="utf-8") != content:
            problems.append(f"stale generated artifact {name}")
    if problems:
        print("error: generated-negative freshness check FAILED:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print("regenerate with: python3 spec/tla/e9_park_wake/_gen_neg.py",
              file=sys.stderr)
        return 1
    print(f"fresh: all {len(artifacts)} generated artifacts "
          f"({len(artifacts) // 2} negative modules) match the current positive "
          f"model + generator")
    return 0


def main() -> int:
    check_only = "--check" in sys.argv[1:]
    if not POSITIVE.is_file():
        fail(f"positive model not found: {POSITIVE}")
    validate_cases()
    artifacts = expected_artifacts(POSITIVE.read_text(encoding="utf-8"))
    if check_only:
        return check_artifacts(artifacts)
    write_artifacts(artifacts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
