#!/usr/bin/env python3
"""e9_trace_validate.py — #196 E9 trace-conformance validator.

Validates a semantic park/wake trace captured from a REAL deterministic C++
execution (tests/e9_trace_conformance_test.cpp) against the SAME-REVISION
as-built TLA+ model (spec/tla/e9_park_wake/E9ParkWake.tla — the PRISTINE
model; this validator never modifies it).

Mechanism (the repository model is the authority — this script is NOT a
second protocol implementation):

  1. Fail-closed schema/revision checks over the trace JSON.
  2. The documented event -> model-action COMPILATION (the mapping table,
     including the two fusions below).
  3. A generated TLC replay wrapper (EXTENDS E9ParkWake) in an isolated
     temp workspace: required steps fire in order — the compiled
     pre-history actions (if any) first, then the compiled trace actions;
     only the non-wake-advancing actions may fire silently (bounded
     budget); TLC searches for ANY behavior of the model that realizes the
     compiled action sequence. Invariant TraceIncomplete is VIOLATED iff a
     behavior consumes the whole trace (ACCEPT, with TLC's counterexample
     as the witness behavior); it HOLDS iff no behavior does (REJECT).

Event -> model action mapping (#196; C++ origins in
src/async/scheduler_park_wake.cpp + src/async/scheduler.cpp):

  ParkCommitted{w}      -> FinalParkRecheckAndCommit(W)   [baseline record]
  ParkEntered{w}        -> EnterPhysicalPark(W)           [cv-wait boundary]
  ParkReturned{w,imm=F} -> LeavePark(W)                   [blocking return]
  ParkReturned{w,imm=T} -> (fused into the preceding EnterPhysicalPark's
                            predicate-true branch — no separate action)
  ParkRefused{w}        -> AbandonParkCandidate(W)  [+ fused adjacent
                            WakePublished(refuse): the C++ refusal signals;
                            the model's action bundles the signal]
  WakePublished{external}       -> ExternalReadyPublish
  WakePublished{runnable_route} -> PublishRunnable
  WakePublished{terminate}      -> ShutdownSignal
  WakePublished{retire,w}       -> RetireWorkerQuiescent(W)
  WakePublished{idle_dance}     -> (fused into the FOLLOWING park pair's
                            EnterPhysicalPark — the model's R4 not-last
                            signal fires at ENTER; the C++ dance signal
                            fires pre-park)
  WakePublished{none}           -> REJECT (unattributed publication)

Terminal-collapse rules (T4's Drain terminate shape; grounded in the
model's E9-CORRECTIVE terminal semantics — runState # "Active" freezes
parked workers and disables both LeavePark and RetireWorkerQuiescent, by
design: the model ends the invocation; the C++ physically drains it):

  WakePublished{terminate}      -> ShutdownSignal (the run-level terminal
                                    publication)
  WakePublished{retire,w} AFTER a terminate wake -> dropped (the fused
                            second half of the C++ terminate+epilogue pair;
                            the #189/#191 fused-exit authority — the model
                            advances the epoch once for the pair)
  ParkReturned with "terminate" among its causes AFTER a terminate wake
                            -> dropped (post-terminal physical teardown;
                            causes=[] or [timeout] are NEVER dropped — a
                            causeless or un-armed-escape return is rejected
                            even post-terminal)

Pre-history (compiled, #202; R-F1 population prefix #223): C++ pre-run
fiber admission / pre-run backend submit publish NO wake; the model
reaches those states only through epoch-advancing producers. Each
declared pre-history is therefore COMPILED into those real model actions
and replayed from the TRUE Init as the FIRST required steps of the
wrapper — TLC must fire them under their actual guards, so the
pre-history state's reachability is mechanically established by the same
search (no hand-written state formula, no Init disjunct). The pre-history
is a PINNED prefix: no silent step may fire until it is consumed (the
C++ pre-run is a fixed contiguous sequence before the recorded window
opens; a silent interleave would drift the pre-history state — e.g. a
silent SubmitBackend turning the MWS3 wait into MWS2, which the
self-test REJECT leg guards against). Since the R-F1 startup refinement
(#223) the model Init has NO worker started (workerStarted = FALSE), so
EVERY replay begins with the population-establishment prefix
StartWorker(W0), StartWorker(W1) — the replay itself now causally
establishes full population before the recorded window opens (the #222
T4 obligation, mechanically enforced instead of fixture-declared):

  population           == Init -> StartWorker(W0) -> StartWorker(W1)
  external_wait_registered == population -> PublishRunnable -> RunFiber
                              -> SuspendFiber
  backend_outstanding      == population -> PublishRunnable -> RunFiber
                              -> SubmitBackend -> FinishFiber

Claim supported by an ACCEPT: TRACE-CONFORMANT (TESTED EXECUTIONS) for this
trace only — never implementation verification, never all executions.

Usage:
  e9_trace_validate.py --trace <file.json> [--expect accept|reject]
                       [--jar <tla2tools.jar>] [--keep]
  e9_trace_validate.py --self-test [--jar <tla2tools.jar>]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MODEL_DIR = os.path.join(os.path.dirname(HERE), "..", "spec", "tla",
                         "e9_park_wake")
MODEL_TLA = "E9ParkWake.tla"

SILENT_BUDGET = 24

KNOWN_EVENTS = {"ParkCommitted", "ParkEntered", "ParkRefused", "WakePublished",
                "ParkReturned"}
KNOWN_CAUSES = {"external", "runnable_route", "refuse", "terminate",
                "retire", "idle_dance"}
KNOWN_PREHISTORY = {"none", "external_wait_registered", "backend_outstanding"}


class Reject(Exception):
    """Fail-closed rejection with a human-readable reason."""


def fail(msg):
    raise Reject(msg)


def check_schema(doc):
    if not isinstance(doc, dict):
        fail("trace root is not a JSON object")
    for key in ("schema", "suite", "test", "cpp_revision", "model_revision",
                "split_wait", "run_mode", "prehistory", "events"):
        if key not in doc:
            fail(f"missing required key: {key}")
    if doc["schema"] != 1:
        fail(f"unknown schema version: {doc['schema']!r}")
    if doc["suite"] != "e9_park_wake":
        fail(f"unknown suite: {doc['suite']!r}")
    if not isinstance(doc["test"], str) or not doc["test"]:
        fail("missing test identity")
    for key in ("cpp_revision", "model_revision"):
        rev = doc[key]
        if not isinstance(rev, str) or len(rev) != 40 or \
                any(c not in "0123456789abcdef" for c in rev):
            fail(f"{key} is not a 40-hex revision: {rev!r}")
    if doc["cpp_revision"] != doc["model_revision"]:
        fail("revision mismatch: cpp_revision != model_revision "
             "(traces must be same-revision bound)")
    if not isinstance(doc["split_wait"], bool):
        fail("split_wait must be a boolean")
    if doc["run_mode"] not in ("live", "drain"):
        fail(f"unknown run_mode: {doc['run_mode']!r}")
    if doc["prehistory"] not in KNOWN_PREHISTORY:
        fail(f"unknown prehistory: {doc['prehistory']!r}")
    events = doc["events"]
    if not isinstance(events, list) or not events:
        fail("events must be a non-empty list")
    for i, ev in enumerate(events):
        if not isinstance(ev, dict) or "event" not in ev:
            fail(f"event {i + 1} is malformed")
        if ev["event"] not in KNOWN_EVENTS:
            fail(f"event {i + 1}: unknown event kind {ev['event']!r}")
        if ev.get("seq") != i + 1:
            fail(f"event {i + 1}: seq must be {i + 1}")
        w = ev.get("worker")
        if w is not None and (not isinstance(w, int) or not 0 <= w <= 1):
            fail(f"event {i + 1}: worker {w!r} outside the model domain "
                 "{W0, W1}")
    return events


def compile_actions(events):
    """The documented event->action compilation (see module docstring)."""
    actions = []  # list of (action_name, worker_or_None)

    def wake_cause(ev):
        cause = ev.get("cause")
        if cause not in KNOWN_CAUSES:
            fail(f"wake event has unknown cause: {cause!r}")
        return cause

    i = 0
    n = len(events)
    terminal_seen = False  # a terminate-family wake already compiled
    while i < n:
        ev = events[i]
        kind = ev["event"]
        if kind == "ParkCommitted":
            if "worker" not in ev or "armed" not in ev:
                fail(f"event {i + 1}: ParkCommitted requires worker+armed")
            actions.append(("FinalParkRecheckAndCommit", ev["worker"]))
        elif kind == "ParkEntered":
            if "worker" not in ev:
                fail(f"event {i + 1}: ParkEntered requires worker")
            actions.append(("EnterPhysicalPark", ev["worker"]))
        elif kind == "ParkReturned":
            if "worker" not in ev or "immediate" not in ev:
                fail(f"event {i + 1}: ParkReturned requires worker+immediate")
            causes = ev.get("causes")
            if not isinstance(causes, list):
                fail(f"event {i + 1}: ParkReturned requires a causes list")
            if terminal_seen and "terminate" in causes:
                # Post-terminal physical teardown: the model froze the parked
                # worker at runState # "Active". NEVER dropped for causes=[]
                # or timeout-only returns (those stay and are rejected).
                i += 1
                continue
            if ev["immediate"]:
                pass  # fused into the preceding EnterPhysicalPark
            else:
                actions.append(("LeavePark", ev["worker"]))
        elif kind == "ParkRefused":
            if "worker" not in ev:
                fail(f"event {i + 1}: ParkRefused requires worker")
            actions.append(("AbandonParkCandidate", ev["worker"]))
            # Fused refusal signal: the NEXT event, if a refuse wake, is the
            # same model action's bundled BridgeEffect.
            if i + 1 < n and events[i + 1]["event"] == "WakePublished" and \
                    wake_cause(events[i + 1]) == "refuse":
                i += 1
        elif kind == "WakePublished":
            cause = wake_cause(ev)
            if cause == "external":
                actions.append(("ExternalReadyPublish", None))
            elif cause == "runnable_route":
                actions.append(("PublishRunnable", None))
            elif cause == "terminate":
                actions.append(("ShutdownSignal", None))
                terminal_seen = True
            elif cause == "retire":
                if "worker" not in ev:
                    fail(f"event {i + 1}: retire wake requires worker")
                if terminal_seen:
                    # The fused second half of the terminate+epilogue pair
                    # (#189/#191 fused-exit authority).
                    i += 1
                    continue
                actions.append(("RetireWorkerQuiescent", ev["worker"]))
            elif cause == "refuse":
                fail(f"event {i + 1}: a refuse wake without an adjacent "
                     "ParkRefused (unmapped pattern)")
            elif cause == "idle_dance":
                # Fused into the FOLLOWING park pair's EnterPhysicalPark
                # (the model's R4 signal fires at ENTER).
                if i + 2 >= n or \
                        events[i + 1]["event"] != "ParkCommitted" or \
                        events[i + 2]["event"] != "ParkEntered":
                    fail(f"event {i + 1}: idle_dance wake not followed by "
                         "a park pair (unmapped pattern)")
                # Consume the pair below through the normal loop.
                pass
        i += 1
    if not actions:
        fail("trace compiles to an empty action sequence")
    return actions


def gen_wrapper(actions, split_wait, run_mode, prehistory, test):
    """Generates the TLC replay wrapper module + cfg text."""
    # The declared pre-history is compiled into the REAL model actions that
    # reach it from the true Init (see the module docstring): the C++ pre-run
    # — fiber admission / backend submit — publishes no wake, and the model
    # reaches those states only through these epoch-advancing producers.
    # TLC must fire them under their actual guards, so the pre-history
    # state's reachability is mechanically established by the same replay
    # search (no hand-written state formula, no Init disjunct).
    pre_actions = {
        "external_wait_registered": [
            ("PublishRunnable", None), ("RunFiber", None),
            ("SuspendFiber", None)],
        "backend_outstanding": [
            ("PublishRunnable", None), ("RunFiber", None),
            ("SubmitBackend", None), ("FinishFiber", None)],
    }.get(prehistory, [])

    # R-F1 (#223): the model Init has no worker started, so every replay
    # causally establishes the worker population (the per-thread startup
    # publication, scheduler.cpp:460) before anything else. Pinned like
    # the rest of the pre-history: the recorded window opens only after
    # full population establishment (the #222 T4 obligation).
    population_prefix = [("StartWorker", 0), ("StartWorker", 1)]

    prelude = population_prefix + pre_actions
    all_actions = prelude + actions
    steps = []
    for idx, (act, worker) in enumerate(all_actions, start=1):
        call = act if worker is None else f"{act}(W{worker})"
        steps.append(
            f"Step{idx} ==\n"
            f"    /\\ tstep = {idx - 1}\n"
            f"    /\\ {call}\n"
            f"    /\\ tstep' = {idx}\n"
            f"    /\\ UNCHANGED <<sbudget, failed>>\n")
    steps_txt = "\n".join(steps) + "\n\n" + "StepsDef ==\n    " + "\n    \\/ ".join(
        f"Step{i}" for i in range(1, len(all_actions) + 1))

    # The trace records the observed run mode; pin Init to it so the replay
    # realizes the trace under exactly the observed configuration. (Always
    # pinned since R-F1: the population-establishment prefix is unconditional,
    # so every replay carries a compiled pre-history.)
    init2 = ("Init2 == Init /\\ runMode = \"%s\" /\\ tstep = 0 /\\ "
             "sbudget = %d /\\ failed = FALSE"
             % (run_mode.capitalize(), SILENT_BUDGET))

    pre_comment = (
        f"   The first {len(population_prefix) + len(pre_actions)} steps are the COMPILED\n"
        "   PRE-HISTORY (Step1..Step%d) — real model actions replayed\n"
        "   from the TRUE Init as a PINNED prefix (no silent step may\n"
        "   fire until they are consumed, matching the contiguous C++\n"
        "   pre-run). The first two steps are the R-F1 population\n"
        "   establishment (#223); the trace steps follow.\n"
        % (len(population_prefix) + len(pre_actions)))

    module = f"""---------------------------- MODULE E9TraceReplay -------------------------------
(* GENERATED by scripts/formal/e9_trace_validate.py (#196) — do not edit.
   Replay wrapper for trace {test!r} over the PRISTINE E9ParkWake model.

   Existential trace conformance: TLC accepts iff SOME behavior of the
   model fires the compiled action sequence in order. {pre_comment}
   Required steps may interleave with SILENT steps — only actions that
   advance NO wake epoch and NO observable park boundary (the C++ trace
   observes every wake publication and every scheduler-domain park
   transition): fiber lifecycle, backend submit/ready, the candidate
   decision, and backend-branch park enter/leave (outside the pilot's
   vocabulary). GiveUp/FailedLoop keep every state successorful (no false
   deadlocks); DoneLoop terminates a completed trace. Invariant
   TraceIncomplete is violated iff the whole trace was consumed. *)
EXTENDS E9ParkWake

VARIABLES tstep, sbudget, failed

(* The compiled pre-history is a PINNED prefix: while tstep < PreLen only
   the pre-history steps may fire — the C++ pre-run (fiber admission /
   backend submit) is a fixed contiguous sequence before the recorded
   window opens, so no other model action may interleave and drift the
   pre-history state (e.g. a silent SubmitBackend turning the MWS3 wait
   into MWS2). Silent steps re-arm only after the pre-history is consumed. *)
PreLen == {len(prelude)}

TraceLen == {len(all_actions)}
{init2}

{steps_txt}

(* Silent steps: budgeted, never consume a trace position. The silent
   FinalParkRecheckAndCommit is the BACKEND-bound commit (the C++ MW-S2
   admission commit — no ParkCommitted event: only parks that reach
   park_on_wake_source are observed); every scheduler-domain park in the
   trace still requires its own trace-positioned commit. *)
SilentStep(w) ==
    \\/ BeginParkCandidate(w)
    \\/ FinalParkRecheckAndCommit(w)
    \\/ /\\ EnterPhysicalPark(w)
       /\\ backendWaitParticipant' = w
    \\/ /\\ LeavePark(w)
       /\\ backendWaitParticipant = w
    \\/ SubmitBackend
    \\/ BackendReadyPublish
    \\/ RunFiber
    \\/ SuspendFiber
    \\/ FinishFiber

Silent ==
    /\\ tstep >= PreLen
    /\\ sbudget > 0
    /\\ (\\E w \\in Workers : SilentStep(w))
    /\\ sbudget' = sbudget - 1
    /\\ tstep' = tstep
    /\\ failed' = FALSE

GiveUp ==
    /\\ tstep < TraceLen
    /\\ failed' = TRUE
    /\\ tstep' = tstep
    /\\ sbudget' = sbudget
    /\\ UNCHANGED vars

FailedLoop ==
    /\\ failed
    /\\ UNCHANGED <<vars, tstep, sbudget, failed>>

DoneLoop ==
    /\\ tstep = TraceLen
    /\\ UNCHANGED <<vars, tstep, sbudget, failed>>

Next2 ==
    StepsDef \\/ Silent \\/ GiveUp \\/ FailedLoop \\/ DoneLoop

Spec2 == Init2 /\\ [][Next2]_<<vars, tstep, sbudget, failed>>

TraceIncomplete == tstep # TraceLen

====
"""
    cfg = f"""SPECIFICATION Spec2
INVARIANT
    TraceIncomplete
CONSTANTS
    Workers = {{W0, W1}}
    Fibers = {{F0}}
    W0 = W0
    W1 = W1
    F0 = F0
    NONE = NONE
    SplitWait = {'TRUE' if split_wait else 'FALSE'}
"""
    return module, cfg


def run_tlc(jar, workdir, module, cfg, timeout=300):
    module_path = os.path.join(workdir, "E9TraceReplay.tla")
    cfg_path = os.path.join(workdir, "E9TraceReplay.cfg")
    with open(module_path, "w") as f:
        f.write(module)
    with open(cfg_path, "w") as f:
        f.write(cfg)
    try:
        proc = subprocess.run(
            ["java", "-XX:+UseParallelGC", "-cp", jar, "tlc2.TLC",
             "-nowarning", "-workers", "1", "-config", "E9TraceReplay.cfg",
             "E9TraceReplay"],
            cwd=workdir, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout + proc.stderr
    except subprocess.TimeoutExpired:
        return 124, "TLC timed out"


def validate_trace(path, jar, expect=None, keep=False):
    with open(path) as f:
        raw = f.read()
    try:
        doc = json.loads(raw)
    except json.JSONDecodeError as e:
        fail(f"malformed trace JSON: {e}")
    events = check_schema(doc)
    actions = compile_actions(events)
    module, cfg = gen_wrapper(actions, doc["split_wait"], doc["run_mode"],
                              doc["prehistory"], doc["test"])
    workdir = tempfile.mkdtemp(prefix="e9-trace.")
    shutil.copy(os.path.join(MODEL_DIR, MODEL_TLA), workdir)
    try:
        rc, out = run_tlc(jar, workdir, module, cfg)
        if "Invariant TraceIncomplete is violated" in out:
            verdict = "accept"
        elif "Model checking completed. No error has been found" in out:
            verdict = "reject"
        else:
            fail(f"TLC did not produce a verdict (rc={rc}):\n"
                 + "\n".join(out.splitlines()[-12:]))
    finally:
        if keep:
            print(f"[keep] workdir: {workdir}", file=sys.stderr)
        else:
            shutil.rmtree(workdir, ignore_errors=True)
    if expect is not None and verdict != expect:
        fail(f"verdict mismatch: expected {expect}, got {verdict}")
    return verdict, actions


# ---- embedded fail-closed fixtures (no TLC needed) -------------------------
def self_test(jar):
    base = {
        "schema": 1, "suite": "e9_park_wake", "test": "selftest",
        "cpp_revision": "a" * 40, "model_revision": "a" * 40,
        "split_wait": True, "run_mode": "live",
        "prehistory": "external_wait_registered",
        "events": [
            {"seq": 1, "event": "ParkCommitted", "worker": 0, "epoch": 0,
             "armed": False},
            {"seq": 2, "event": "ParkEntered", "worker": 0},
            {"seq": 3, "event": "WakePublished", "epoch": 1,
             "cause": "external"},
            {"seq": 4, "event": "ParkReturned", "worker": 0,
             "immediate": False, "causes": ["epoch"]},
        ],
    }

    def expect_reject(mutate, name, label):
        doc = json.loads(json.dumps(base))
        mutate(doc)
        try:
            check_schema(doc)
            compile_actions(doc["events"])
        except Reject:
            print(f"PASS  selftest {name} ({label})")
            return
        print(f"FAIL  selftest {name}: expected fail-closed rejection")
        sys.exit(1)

    def rev_mismatch(doc):
        doc["model_revision"] = "b" * 40
    expect_reject(rev_mismatch, "revision-mismatch", "cpp != model")
    expect_reject(lambda d: d.update(schema=2), "schema", "unknown schema")
    expect_reject(lambda d: d["events"][2].update(cause="mystery"),
                  "unknown-cause", "unmapped wake cause")
    expect_reject(lambda d: d["events"][2].update(cause="none"),
                  "unattributed-wake", "cause=none")
    expect_reject(
        lambda d: d["events"].append(
            {"seq": 5, "event": "Teleported", "worker": 0}),
        "unknown-event", "unknown event kind")
    expect_reject(lambda d: d.pop("cpp_revision"), "missing-revision",
                  "no cpp_revision")
    expect_reject(lambda d: d["events"][3].update(worker=7),
                  "worker-domain", "worker outside {W0,W1}")
    expect_reject(lambda d: d["events"].__setitem__(
        1, {"seq": 2, "event": "WakePublished", "epoch": 1,
            "cause": "refuse"}), "stray-refuse-wake",
        "refuse wake without ParkRefused")

    # Positive compilation sanity (schema + compile only).
    check_schema(base)
    actions = compile_actions(base["events"])
    expected = [("FinalParkRecheckAndCommit", 0), ("EnterPhysicalPark", 0),
                ("ExternalReadyPublish", None), ("LeavePark", 0)]
    if actions != expected:
        print(f"FAIL  selftest compile: {actions}")
        sys.exit(1)
    print("PASS  selftest compile (t1 sequence)")

    if jar is None:
        print("SKIP  selftest TLC legs (no --jar)")
        return

    # Full TLC legs: one ACCEPT (T1's real shape) and one REJECT (the
    # causeless-return mutant NEG-A) — non-vacuity of the verdict itself.
    with tempfile.TemporaryDirectory(prefix="e9-selftest.") as td:
        acc = os.path.join(td, "acc.json")
        with open(acc, "w") as f:
            json.dump(base, f)
        verdict, _ = validate_trace(acc, jar, expect="accept")
        print(f"PASS  selftest TLC accept leg ({verdict})")
        neg = json.loads(json.dumps(base))
        # NEG-A mutant: the wake is REMOVED and the return carries no cause
        # (a return the as-built cv predicate cannot make). Under the
        # compiled-pre-history wrapper this leg ALSO guards the PINNED
        # pre-history prefix (#202): without the pin, a silent SubmitBackend
        # could interleave between RunFiber and SuspendFiber, drift the MWS3
        # wait into MWS2 (backend-domain park), and fabricate this
        # acceptance.
        neg["events"] = neg["events"][:2] + [
            {"seq": 3, "event": "ParkReturned", "worker": 0,
             "immediate": False, "causes": []}]
        negp = os.path.join(td, "neg.json")
        with open(negp, "w") as f:
            json.dump(neg, f)
        verdict, _ = validate_trace(negp, jar, expect="reject")
        print(f"PASS  selftest TLC reject leg ({verdict})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--trace", help="trace JSON file to validate")
    ap.add_argument("--expect", choices=["accept", "reject"],
                    help="expected verdict (exit 1 on mismatch)")
    ap.add_argument("--jar", help="tla2tools.jar path "
                    "(or the TLA2TOOLS_JAR environment variable)")
    ap.add_argument("--self-test", action="store_true",
                    help="run the embedded fail-closed fixtures")
    ap.add_argument("--keep", action="store_true",
                    help="keep the generated TLC workspace")
    args = ap.parse_args()

    jar = args.jar or os.environ.get("TLA2TOOLS_JAR")
    if args.self_test:
        self_test(jar)
        print("=== self-test PASS ===")
        return 0
    if not args.trace:
        ap.error("--trace or --self-test is required")
    if jar is None:
        print("error: no TLC jar (--jar or TLA2TOOLS_JAR)", file=sys.stderr)
        return 2

    try:
        verdict, actions = validate_trace(args.trace, jar, args.expect,
                                          args.keep)
    except Reject as e:
        if args.expect == "reject":
            # A fail-closed rejection is the expected outcome.
            print(f"REJECT  {args.trace}: {e}")
            return 0
        print(f"REJECT  {args.trace}: {e}")
        return 1
    print(f"{verdict.upper():6} {args.trace}")
    print("       actions: " +
          ", ".join(a if w is None else f"{a}(W{w})" for a, w in actions))
    return 0


if __name__ == "__main__":
    sys.exit(main())
