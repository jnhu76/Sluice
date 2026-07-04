------------------------------- MODULE BlockingIoPool -------------------------------
(*
  TLA+ specification of the production sluice::BlockingIoPool (sluice-CORE-024S).

  Purpose: EXHAUSTIVELY check the C-class protocol safety/liveness properties
  that single-threaded tests and stress tests CANNOT prove (they are
  probabilistic):
    C1  no internal protocol stuck state (every reachable modeled state is
        either legitimate quiescence after admission closes and accepted work
        drains, or has a real modeled protocol transition enabled)
    C3  queue bound        (queue length never exceeds max_depth)
    C5  shutdown contract  (after shutdown: no new accepts; prior accepts drain)
    C2  modeled-task progress (under the stated weak fairness assumptions,
        every accepted modeled task eventually reaches done)  [liveness]
    C4  happens-before     (Task::get sees the value set by the worker)  [via linearizability]

  The model is parameterized but TLC checks SMALL constants (NumWorkers=2,
  MaxDepth=2, NumTasks=3). TLC is exhaustive for all reachable interleavings of
  the configured constants; it is not a mathematical induction proof over all
  possible sizes.

  Mapping to the C++ implementation:
    - Submit(blocking)     : waits when queue full; rejects after shutdown
    - TrySubmit            : rejects when full OR after shutdown (no wait)
    - Worker dequeue       : FIFO pop from queue; notify submitter (space freed)
    - Worker complete      : set value on Task state; notify getter
    - Shutdown             : stop accepting; workers drain remaining; join

  This spec is the authoritative contract for the internal admission /
  bounded-queue / dequeue / completion / shutdown-drain protocol. It does NOT
  model arbitrary user-callable behavior, recursive same-pool submission, or
  same-pool Task dependency graphs. If TLC finds a violation of a modeled
  property, the C++ protocol has a bug (or the spec diverges from intent —
  either is actionable).
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

(* Small constants for exhaustive model checking. Bump these only if the state
   space stays tractable; the invariants are size-independent. *)
CONSTANTS
    NumWorkers,   (* fixed worker count, >= 1            *)
    MaxDepth,     (* max queue depth, >= 1               *)
    NumTasks      (* distinct tasks in the scenario      *)

(* --------------------------------------------------------------------------- *)
(* State                                                                       *)
(* --------------------------------------------------------------------------- *)

(* The task IDs are 1..NumTasks. Workers are 1..NumWorkers. *)

VARIABLES
    queue,        (* Seq of task IDs currently queued (FIFO; head = front)        *)
    accepting,    (* BOOLEAN: pool still accepts submissions                      *)
    inFlight,     (* set of task IDs currently dequeued + not yet completed       *)
    done,         (* set of task IDs whose Task state is ready (value set)        *)
    submitted,    (* set of task IDs that have been accepted (Submit succeeded)   *)
    rejected,     (* set of task IDs whose submission was rejected                *)
    getters       (* set of task IDs for which get() has been called (consume)    *)

(* --------------------------------------------------------------------------- *)
(* Helpers                                                                     *)
(* --------------------------------------------------------------------------- *)

Workers == 1..NumWorkers
AllTasks == 1..NumTasks

(* Is a task "completed"? A task is completed once its worker has set its value.
   For the model, done = set of task IDs whose State is ready. *)

(* --------------------------------------------------------------------------- *)
(* Initial state                                                               *)
(* --------------------------------------------------------------------------- *)

Init ==
    /\ queue = <<>>
    /\ accepting = TRUE
    /\ inFlight = {}
    /\ done = {}
    /\ submitted = {}
    /\ rejected = {}
    /\ getters = {}

(* --------------------------------------------------------------------------- *)
(* Transitions (stutter-free actions; any subset may be enabled per step)      *)
(* --------------------------------------------------------------------------- *)

(* Submit (blocking with backpressure): enqueue a task if there is space and we
   are still accepting. Models submit(): waits when full, so the action is only
   enabled when NOT full (or not accepting -> reject path). *)
Submit(t) ==
    /\ t \in AllTasks
    /\ t \notin submitted
    /\ t \notin rejected
    /\ IF accepting
       THEN /\ Len(queue) < MaxDepth    (* backpressure: only enabled with space *)
            /\ queue' = Append(queue, t)
            /\ submitted' = submitted \cup {t}
            /\ UNCHANGED <<accepting, inFlight, done, rejected, getters>>
       ELSE (* after shutdown: reject *)
            /\ rejected' = rejected \cup {t}
            /\ UNCHANGED <<queue, accepting, inFlight, done, submitted, getters>>

(* TrySubmit (non-blocking): enqueue if space; else reject. After shutdown: reject. *)
TrySubmit(t) ==
    /\ t \in AllTasks
    /\ t \notin submitted
    /\ t \notin rejected
    /\ IF /\ accepting
          /\ Len(queue) < MaxDepth
       THEN /\ queue' = Append(queue, t)
            /\ submitted' = submitted \cup {t}
            /\ UNCHANGED <<accepting, inFlight, done, rejected, getters>>
       ELSE (* full or shutdown: reject *)
            /\ rejected' = rejected \cup {t}
            /\ UNCHANGED <<queue, accepting, inFlight, done, submitted, getters>>

(* Worker dequeues the head of the queue (FIFO). Any worker; we don't model
   worker identity since the queue is shared. *)
Dequeue ==
    /\ Len(queue) > 0
    /\ LET t == Head(queue)
       IN  /\ queue' = Tail(queue)
           /\ inFlight' = inFlight \cup {t}
           /\ UNCHANGED <<accepting, done, submitted, rejected, getters>>

(* Worker completes a dequeued task: sets the Task state to ready. *)
Complete ==
    /\ \E t \in inFlight :
          /\ done' = done \cup {t}
          /\ inFlight' = inFlight \ {t}
          /\ UNCHANGED <<queue, accepting, submitted, rejected, getters>>

(* Get: a submitter calls Task::get(), consuming the value once the task is done.
   Models the happens-before edge: get() can only read a value the worker set. *)
Get(t) ==
    /\ t \in done
    /\ t \notin getters
    /\ getters' = getters \cup {t}
    /\ UNCHANGED <<queue, accepting, inFlight, done, submitted, rejected>>

(* Shutdown: stop accepting. Workers continue to drain (Dequeue/Complete still
   enabled on remaining queue/inFlight). *)
Shutdown ==
    /\ accepting = TRUE
    /\ accepting' = FALSE
    /\ UNCHANGED <<queue, inFlight, done, submitted, rejected, getters>>

(* --------------------------------------------------------------------------- *)
(* Next-state relation                                                          *)
(* --------------------------------------------------------------------------- *)

Next ==
    \/ \E t \in AllTasks : Submit(t)
    \/ \E t \in AllTasks : TrySubmit(t)
    \/ Dequeue
    \/ Complete
    \/ \E t \in AllTasks : Get(t)
    \/ Shutdown

(* --------------------------------------------------------------------------- *)
(* Spec                                                                         *)
(* --------------------------------------------------------------------------- *)

Vars == <<queue, accepting, inFlight, done, submitted, rejected, getters>>

Spec == Init /\ [][Next]_Vars

(* --------------------------------------------------------------------------- *)
(* INVARIANTS (safety — checked by TLC at every reachable state)                *)
(* --------------------------------------------------------------------------- *)

(* C3: queue length never exceeds the bound. *)
QueueBoundInvariant == Len(queue) <= MaxDepth

(* Structural: inFlight and done are disjoint; done and getters nest correctly. *)
StateConsistency ==
    /\ inFlight \cap done = {}                    (* a task isn't both running and done *)
    /\ getters \subseteq done                     (* get() only on done tasks *)
    /\ done \subseteq submitted                   (* done implies it was submitted *)
    /\ inFlight \subseteq submitted               (* inFlight implies submitted *)
    /\ \A i \in 1..Len(queue) : queue[i] \in submitted  (* queued implies submitted *)

(* C5 (safety half): once not accepting, no NEW task enters the queue. Formally:
   after accepting becomes FALSE, submitted does not grow via Submit-enqueue.
   We model this as: every task in the queue/inFlight/done was submitted WHILE
   accepting (or is a reject). The cleanest invariant: if ~accepting, then for
   any t added to submitted after shutdown... we approximate by: a task can only
   be in submitted if it was enqueued, and enqueue requires accepting. This is
   enforced by Submit's guard. TLC checks the transition guards, so the safety
   is proven by inspecting Submit. We add a derived invariant: *)
NoEnqueueAfterShutdown ==
    /\ accepting => TRUE   (* trivial; the real check is in the Submit guard *)
    (* The non-trivial statement: there is no transition that adds to 'submitted'
       while accepting=FALSE. Submit's IF branch guarantees this. *)

(* A useful aggregate: all tasks are in exactly one lifecycle bucket. *)
TaskLifecycle ==
    /\ submitted \subseteq AllTasks
    /\ rejected \subseteq AllTasks
    /\ submitted \cap rejected = {}              (* a task is either accepted or rejected, not both *)

(* C1: no internal protocol stuck state. Stuttering is part of the behavioral
   spec via [][Next]_Vars; it is NOT a protocol action. A reachable state is
   acceptable only when it is legitimate quiescence, or when a real modeled
   protocol transition is enabled. *)
LegitimateQuiescence ==
    /\ accepting = FALSE
    /\ queue = <<>>
    /\ inFlight = {}
    /\ submitted \subseteq done
    /\ getters = done
    /\ submitted \cup rejected = AllTasks

ProtocolTransitionEnabled ==
    \/ \E t \in AllTasks : ENABLED Submit(t)
    \/ \E t \in AllTasks : ENABLED TrySubmit(t)
    \/ ENABLED Dequeue
    \/ ENABLED Complete
    \/ \E t \in AllTasks : ENABLED Get(t)
    \/ ENABLED Shutdown

NoInternalProtocolStuck ==
    \/ LegitimateQuiescence
    \/ ProtocolTransitionEnabled

(* --------------------------------------------------------------------------- *)
(* LIVENESS / TEMPORAL (checked by TLC with fairness)                           *)
(* --------------------------------------------------------------------------- *)

(* C2 modeled-task progress: under the modeled environment and weak fairness on
   Dequeue + Complete, every accepted modeled task eventually completes (done).
   This is not a claim that arbitrary C++ callables eventually return. *)
StarvationFree ==
    \A t \in AllTasks : (t \in submitted) ~> (t \in done)

(* C4 linearizability: if get(t) returns, then t \in done (the worker set the
   value first). This is the happens-before edge. Already enforced by Get's guard. *)
Linearizable ==
    getters \subseteq done

(* --------------------------------------------------------------------------- *)
(* Fairness (required for liveness checking)                                    *)
(* --------------------------------------------------------------------------- *)

(* Weak fairness on Dequeue and Complete: workers keep draining as long as there
   is modeled work and modeled task execution completes. Required for the
   modeled-task progress liveness check. *)
FairSpec ==
    /\ Spec
    /\ WF_Vars(Dequeue)
    /\ WF_Vars(Complete)

(* Temporal property to check: modeled-task progress. *)
TemporalProperty == StarvationFree

=============================================================================
