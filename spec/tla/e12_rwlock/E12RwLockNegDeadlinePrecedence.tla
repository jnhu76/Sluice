------------------------------- MODULE E12RwLockNegDeadlinePrecedence -------------------------------
\* sluice::async::AsyncRwLock -- writer-fair phase-batched RwLock SAFETY model
\*
\* NEGATIVE MODEL (audit-added, NEG-RW4): this variant inverts the audit
\* MODEL-003 precedence — the timed admission actions resolve "Expired" when
\* the deadline is already due EVEN THOUGH the resource is admissible
\* (deadline beats resource; the C++ rwlock_{read,write}_lock_until order is
\* the opposite: the resource claim is precedence 1). The mutation changes
\* ONLY the outcome: admissionSawResource stays TRUE and admissionSawDue
\* keeps the environment's due bit, so a due=TRUE admission leaves
\* TRUE/TRUE evidence with an Expired resolution. Expected TLC verdict:
\* VIOLATION of InvResourceFirstDeadline (cfg:
\* E12RwLockNegDeadlinePrecedence.cfg). All other laws remain intact so the
\* named check is exact.
\* (E12-F, authority docs/history/implementation-plans/e12-rwlock.md).
\*
\* Key safety properties:
\*   RW1  Mutual Exclusion        writer active => no readers; readers > 0 => no writer
\*   RW2  Writer-Fair Admission   new reader cannot barge past queued writer
\*   RW3  Reader Batch Correctness grant grants maximal reader prefix (stops at writer)
\*   RW4  Head Reconcile          cancel/expire of head immediately advances next
\*                                (writer grant also requires activeReaders = 0;
\*                                 reader-prefix grant merges into live readers)
\*   RW5  Terminal Uniqueness     each epoch resolves at most once
\*   RW6  Publication Uniqueness  each epoch published at most once
\*   RW7  No Linked Terminal      terminal epoch is not in queue
\*   RW8  ActiveReader Integrity  activeReaders never underflows
\*   RW9  Head-Reconcile Closure  an idle lock never strands a grantable head
\*   RW10 Reader Non-Revocation    a granted reader leaves only via its own
\*                                 UnlockRead (MODEL-002 negative control)
\*   RW11 Resource-First Deadline  a *_lock_until admission that is resource-
\*                                 admissible resolves Woken even when the
\*                                 deadline is already due (MODEL-003)
\*
\* SCOPE: SAFETY-ONLY. Admission is ONE atomic step (register+recheck+disposition).
\* Timed admission (*_lock_until) carries the environment-chosen deadlineDue
\* ghost plus admission-evidence latches, mirroring E12Semaphore (P7) and
\* E12AsyncMutex (M7) — audit #162 MODEL-003 parity closure.
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Epochs, E1, E2, E3

VARIABLES
    activeReaders,      \* Nat: number of active read-lock holders
    writerOwner,        \* Epoch \cup {NoWriter}: current writer epoch
    queue,              \* Seq(Epoch): FIFO wait queue
    mode,               \* [Epoch -> {"read","write","unset"}]: requested mode
    nodeState,          \* [Epoch -> {"Free","Queued","Woken","Cancelled","Expired"}]
    resolutionCount,    \* [Epoch -> 0..1]: terminal resolution count
    publicationCount,   \* [Epoch -> 0..1]: runnable publication count
    grantedReaders,     \* SUBSET Epochs: epochs currently holding read lock
    bargingOccurred,    \* HISTORY: TRUE if a reader was admitted while writer queued
    writerWasQueued,    \* HISTORY: TRUE if a writer has ever been queued (for barging obs)
    revocationOccurred, \* HISTORY: TRUE if a granted reader was revoked without its
                        \*          own UnlockRead (C++ impossible; the ghost is the
                        \*          audit MODEL-002 negative-control probe)
    deadlineDue,        \* [Epoch -> BOOLEAN]: env-chosen *_lock_until deadline
                        \*          already due at the admission recheck (P7/M7
                        \*          pattern; audit #162 MODEL-003)
    admissionSawResource, \* HISTORY latch: the admission saw an admissible
                        \*          resource state (precedence 1 was applicable)
    admissionSawDue     \* HISTORY latch: the admission saw an already-due
                        \*          deadline (precedence 2 was applicable)

NoWriter == 999
Mode == {"read", "write"}
NState == {"Free", "Queued", "Woken", "Cancelled", "Expired"}

vars == <<activeReaders, writerOwner, queue, mode, nodeState,
          resolutionCount, publicationCount, grantedReaders, bargingOccurred,
          writerWasQueued, revocationOccurred, deadlineDue,
          admissionSawResource, admissionSawDue>>

\* ---- Helpers ----
\* Head/Tail from Sequences module (standard)

\* Maximal reader prefix length from queue head
RECURSIVE ReaderPrefixLen(_)
ReaderPrefixLen(q) ==
    IF Len(q) = 0 THEN 0
    ELSE IF mode[Head(q)] = "write" THEN 0
    ELSE 1 + ReaderPrefixLen(Tail(q))

\* ---- Initial State ----
Init ==
    /\ activeReaders = 0
    /\ writerOwner = NoWriter
    /\ queue = <<>>
    /\ mode = [e \in Epochs |-> "unset"]
    /\ nodeState = [e \in Epochs |-> "Free"]
    /\ resolutionCount = [e \in Epochs |-> 0]
    /\ publicationCount = [e \in Epochs |-> 0]
    /\ grantedReaders = {}
    /\ bargingOccurred = FALSE
    /\ writerWasQueued = FALSE
    /\ revocationOccurred = FALSE
    /\ deadlineDue = [e \in Epochs |-> FALSE]
    /\ admissionSawResource = [e \in Epochs |-> FALSE]
    /\ admissionSawDue = [e \in Epochs |-> FALSE]

\* ---- Transitions ----

\* ReadAdmit: immediate read admission (no writer, queue empty)
ReadAdmit(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ writerOwner = NoWriter
    /\ Len(queue) = 0
    /\ mode' = [mode EXCEPT ![e] = "read"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Woken"]
    /\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]
    /\ grantedReaders' = grantedReaders \cup {e}
    /\ activeReaders' = activeReaders + 1
    /\ deadlineDue' = [deadlineDue EXCEPT ![e] = FALSE]
    /\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = TRUE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = FALSE]
    \* In the correct model, ReadAdmit requires Len(queue)=0, so no writer is
    \* queued at admission time — barging cannot occur here. bargingOccurred
    \* stays unchanged (the negative model overrides ReadAdmit to record the
    \* violation when it drops the Len(queue)=0 guard).
    /\ UNCHANGED <<writerOwner, queue, publicationCount, bargingOccurred,
                   writerWasQueued, revocationOccurred>>

\* ReadQueue: reader must queue (writer active OR queue non-empty)
ReadQueue(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "read"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Queued"]
    /\ queue' = Append(queue, e)
    /\ deadlineDue' = [deadlineDue EXCEPT ![e] = FALSE]
    /\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = FALSE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = FALSE]
    /\ UNCHANGED <<activeReaders, writerOwner, resolutionCount,
                   publicationCount, grantedReaders, bargingOccurred,
                   writerWasQueued, revocationOccurred>>

\* WriteAdmit: immediate write admission (no readers, no writer, queue empty)
WriteAdmit(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ activeReaders = 0
    /\ writerOwner = NoWriter
    /\ Len(queue) = 0
    /\ mode' = [mode EXCEPT ![e] = "write"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Woken"]
    /\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]
    /\ writerOwner' = e
    /\ deadlineDue' = [deadlineDue EXCEPT ![e] = FALSE]
    /\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = TRUE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = FALSE]
    /\ UNCHANGED <<activeReaders, queue, publicationCount, grantedReaders,
                   bargingOccurred, writerWasQueued, revocationOccurred>>

\* WriteQueue: writer must queue
WriteQueue(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (activeReaders > 0 \/ writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "write"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Queued"]
    /\ queue' = Append(queue, e)
    /\ writerWasQueued' = TRUE
    /\ deadlineDue' = [deadlineDue EXCEPT ![e] = FALSE]
    /\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = FALSE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = FALSE]
    /\ UNCHANGED <<activeReaders, writerOwner, resolutionCount,
                   publicationCount, grantedReaders, bargingOccurred,
                   revocationOccurred>>

\* ---- Timed admission (*_lock_until; audit #162 MODEL-003) ----
\* Mirrors E12Semaphore (P7) / E12AsyncMutex (M7): the environment chooses the
\* deadlineDue bit at the admission recheck so BOTH precedence halves are
\* reachable. As-built authority (src/async/scheduler_rwlock.cpp
\* rwlock_read_lock_until / rwlock_write_lock_until):
\*   precedence 1 — resource-first claim: the fresh node is the queue head
\*     (model: Len(queue)=0 pre-state) and the lock is admissible (read: no
\*     writer, readers merge; write: no readers and no writer);
\*   precedence 2 — already-due deadline (E11 I5), evaluated only when
\*     precedence 1 did not apply.
\* The evidence latches are set atomically with the resolution so
\* InvResourceFirstDeadline is a prime-free state predicate. Admission remains
\* ONE atomic step: an inline resolution never becomes queue-visible and never
\* publishes runnable (the Fiber is Running; the C++ make_runnable on self is
\* a no-op). The transient timer registration is invisible at this
\* abstraction: it is created and retired within the same atomic step.

\* ReadUntilAdmit: resource-admissible read admission with an env-chosen
\* possibly-due deadline. The resource claim WINS over a due deadline.
ReadUntilAdmit(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ writerOwner = NoWriter
    /\ Len(queue) = 0
    /\ \E due \in BOOLEAN :
        /\ deadlineDue' = [deadlineDue EXCEPT ![e] = due]
        /\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = due]
    /\ mode' = [mode EXCEPT ![e] = "read"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Expired"]
    /\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]
    /\ grantedReaders' = grantedReaders \cup {e}
    /\ activeReaders' = activeReaders + 1
    /\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = TRUE]
    /\ UNCHANGED <<writerOwner, queue, publicationCount, bargingOccurred,
                   writerWasQueued, revocationOccurred>>

\* ReadUntilExpired: NOT resource-admissible + deadline already due -> Expired
\* at admission (precedence 2; production authority E11, modeled here to prove
\* the other half of the precedence). No publication: the Fiber never
\* suspended. The epoch never becomes queue-visible.
ReadUntilExpired(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "read"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Expired"]
    /\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]
    /\ deadlineDue' = [deadlineDue EXCEPT ![e] = TRUE]
    /\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = FALSE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = TRUE]
    /\ UNCHANGED <<activeReaders, writerOwner, queue, publicationCount,
                   grantedReaders, bargingOccurred, writerWasQueued,
                   revocationOccurred>>

\* ReadUntilSuspend: NOT resource-admissible + deadline NOT due -> park.
ReadUntilSuspend(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "read"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Queued"]
    /\ queue' = Append(queue, e)
    /\ deadlineDue' = [deadlineDue EXCEPT ![e] = FALSE]
    /\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = FALSE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = FALSE]
    /\ UNCHANGED <<activeReaders, writerOwner, resolutionCount,
                   publicationCount, grantedReaders, bargingOccurred,
                   writerWasQueued, revocationOccurred>>

\* WriteUntilAdmit: resource-admissible write admission with an env-chosen
\* possibly-due deadline. The resource claim WINS over a due deadline.
WriteUntilAdmit(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ activeReaders = 0
    /\ writerOwner = NoWriter
    /\ Len(queue) = 0
    /\ \E due \in BOOLEAN :
        /\ deadlineDue' = [deadlineDue EXCEPT ![e] = due]
        /\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = due]
    /\ mode' = [mode EXCEPT ![e] = "write"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Expired"]
    /\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]
    /\ writerOwner' = e
    /\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = TRUE]
    /\ UNCHANGED <<activeReaders, queue, publicationCount, grantedReaders,
                   bargingOccurred, writerWasQueued, revocationOccurred>>

\* WriteUntilExpired: NOT resource-admissible + deadline already due ->
\* Expired at admission (precedence 2). No publication.
WriteUntilExpired(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (activeReaders > 0 \/ writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "write"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Expired"]
    /\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]
    /\ deadlineDue' = [deadlineDue EXCEPT ![e] = TRUE]
    /\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = FALSE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = TRUE]
    /\ UNCHANGED <<activeReaders, writerOwner, queue, publicationCount,
                   grantedReaders, bargingOccurred, writerWasQueued,
                   revocationOccurred>>

\* WriteUntilSuspend: NOT resource-admissible + deadline NOT due -> park.
WriteUntilSuspend(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (activeReaders > 0 \/ writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "write"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Queued"]
    /\ queue' = Append(queue, e)
    /\ writerWasQueued' = TRUE
    /\ deadlineDue' = [deadlineDue EXCEPT ![e] = FALSE]
    /\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = FALSE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = FALSE]
    /\ UNCHANGED <<activeReaders, writerOwner, resolutionCount,
                   publicationCount, grantedReaders, bargingOccurred,
                   revocationOccurred>>

\* UnlockRead: release one read share
UnlockRead(e) ==
    /\ e \in grantedReaders
    /\ activeReaders > 0
    /\ IF activeReaders - 1 = 0 /\ Len(queue) > 0
       THEN \* reconcile: grant from head
            /\ LET prefix == ReaderPrefixLen(queue)
               IN IF prefix > 0
                  THEN \* grant reader prefix
                       /\ activeReaders' = prefix
                       /\ grantedReaders' = {queue[i] : i \in 1..prefix}
                       /\ nodeState' = [i \in Epochs |->
                            IF \E j \in 1..prefix : queue[j] = i
                            THEN "Woken" ELSE nodeState[i]]
                       /\ resolutionCount' = [i \in Epochs |->
                            IF \E j \in 1..prefix : queue[j] = i
                            THEN 1 ELSE resolutionCount[i]]
                       /\ publicationCount' = [i \in Epochs |->
                            IF \E j \in 1..prefix : queue[j] = i
                            THEN 1 ELSE publicationCount[i]]
                       /\ queue' = SubSeq(queue, prefix + 1, Len(queue))
                       /\ writerOwner' = NoWriter
                  ELSE \* head is writer: grant writer
                       /\ activeReaders' = 0
                       /\ grantedReaders' = {}
                       /\ writerOwner' = Head(queue)
                       /\ nodeState' = [nodeState EXCEPT ![Head(queue)] = "Woken"]
                       /\ resolutionCount' = [resolutionCount EXCEPT ![Head(queue)] = 1]
                       /\ publicationCount' = [publicationCount EXCEPT ![Head(queue)] = 1]
                       /\ queue' = Tail(queue)
       ELSE \* no reconcile needed
            /\ UNCHANGED <<writerOwner, queue, nodeState, resolutionCount, publicationCount>>
            /\ grantedReaders' = grantedReaders \ {e}
            /\ activeReaders' = activeReaders - 1
    /\ UNCHANGED <<mode, bargingOccurred, writerWasQueued, revocationOccurred,
                   deadlineDue, admissionSawResource, admissionSawDue>>

\* UnlockWrite: release write lock
UnlockWrite(e) ==
    /\ writerOwner = e
    /\ IF Len(queue) > 0
       THEN LET prefix == ReaderPrefixLen(queue)
            IN IF prefix > 0
               THEN \* grant reader prefix
                    /\ activeReaders' = prefix
                    /\ grantedReaders' = {queue[i] : i \in 1..prefix}
                    /\ nodeState' = [i \in Epochs |->
                         IF \E j \in 1..prefix : queue[j] = i
                         THEN "Woken" ELSE nodeState[i]]
                    /\ resolutionCount' = [i \in Epochs |->
                         IF \E j \in 1..prefix : queue[j] = i
                         THEN 1 ELSE resolutionCount[i]]
                    /\ publicationCount' = [i \in Epochs |->
                         IF \E j \in 1..prefix : queue[j] = i
                         THEN 1 ELSE publicationCount[i]]
                    /\ queue' = SubSeq(queue, prefix + 1, Len(queue))
                    /\ writerOwner' = NoWriter
               ELSE \* grant head writer
                    /\ activeReaders' = 0
                    /\ grantedReaders' = {}
                    /\ writerOwner' = Head(queue)
                    /\ nodeState' = [nodeState EXCEPT ![Head(queue)] = "Woken"]
                    /\ resolutionCount' = [resolutionCount EXCEPT ![Head(queue)] = 1]
                    /\ publicationCount' = [publicationCount EXCEPT ![Head(queue)] = 1]
                    /\ queue' = Tail(queue)
       ELSE \* empty queue
            /\ activeReaders' = 0
            /\ grantedReaders' = {}
            /\ writerOwner' = NoWriter
            /\ UNCHANGED <<queue, nodeState, resolutionCount, publicationCount>>
    /\ UNCHANGED <<mode, bargingOccurred, writerWasQueued, revocationOccurred,
                   deadlineDue, admissionSawResource, admissionSawDue>>

\* CancelQueued: cancel a queued epoch + reconcile the exposed head.
\*
\* AS-BUILT (src/async/scheduler_rwlock.cpp rwlock_cancel ->
\* rwlock_grant_from_head_locked): the canceled node e is resolved Cancelled
\* and unlinked, then the new head is granted iff
\*   - head mode read   : writerOwner = NoWriter -> maximal reader prefix is
\*                        granted and MERGES into the live reader set
\*                        (activeReaders += prefix; C++ reader batch);
\*   - head mode write  : activeReaders = 0 /\ writerOwner = NoWriter
\*                        -> exactly that one writer is granted
\*                        (C++ guard `if (active_readers > 0 || writer_active)
\*                        return;` at scheduler_rwlock.cpp:119);
\*   - otherwise        : no grant; the exposed head stays queued.
\* Each branch assigns every state variable exactly once (audit #162
\* MODEL-001/MODEL-002 single-assignment repair — the pre-fix duplicate
\* primed assignments made the reconcile branches unsatisfiable).
\* revocationOccurred is a HISTORY ghost: only a writer-grant that clears a
\* live reader set could set it, which the activeReaders = 0 guard excludes.
CancelQueued(e) ==
    /\ nodeState[e] = "Queued"
    /\ \E i \in 1..Len(queue) : queue[i] = e
    /\ LET pos == CHOOSE i \in 1..Len(queue) : queue[i] = e
           newQ == SubSeq(queue, 1, pos - 1) \o SubSeq(queue, pos + 1, Len(queue))
       IN
       /\ IF pos = 1 /\ Len(newQ) > 0 /\ writerOwner = NoWriter /\
             mode[Head(newQ)] = "read"
          THEN \* head cancelled: grant reader prefix from new head (merge)
               LET prefix == ReaderPrefixLen(newQ)
               IN /\ activeReaders' = activeReaders + prefix
                  /\ grantedReaders' = grantedReaders \cup
                       {newQ[i] : i \in 1..prefix}
                  /\ writerOwner' = NoWriter
                  /\ nodeState' = [i \in Epochs |->
                       IF i = e THEN "Cancelled"
                       ELSE IF \E j \in 1..prefix : newQ[j] = i
                       THEN "Woken" ELSE nodeState[i]]
                  /\ resolutionCount' = [i \in Epochs |->
                       IF i = e THEN 1
                       ELSE IF \E j \in 1..prefix : newQ[j] = i
                       THEN 1 ELSE resolutionCount[i]]
                  /\ publicationCount' = [i \in Epochs |->
                       IF i = e THEN 1
                       ELSE IF \E j \in 1..prefix : newQ[j] = i
                       THEN 1 ELSE publicationCount[i]]
                  /\ queue' = SubSeq(newQ, prefix + 1, Len(newQ))
                  /\ revocationOccurred' = revocationOccurred
          ELSE IF pos = 1 /\ Len(newQ) > 0 /\ writerOwner = NoWriter /\
                    mode[Head(newQ)] = "write" /\ activeReaders = 0
          THEN \* grant exactly one writer
               /\ activeReaders' = 0
               /\ grantedReaders' = {}
               /\ writerOwner' = Head(newQ)
               /\ nodeState' = [i \in Epochs |->
                    IF i = e THEN "Cancelled"
                    ELSE IF i = Head(newQ) THEN "Woken"
                    ELSE nodeState[i]]
               /\ resolutionCount' = [i \in Epochs |->
                    IF i = e THEN 1
                    ELSE IF i = Head(newQ) THEN 1
                    ELSE resolutionCount[i]]
               /\ publicationCount' = [i \in Epochs |->
                    IF i = e THEN 1
                    ELSE IF i = Head(newQ) THEN 1
                    ELSE publicationCount[i]]
               /\ queue' = Tail(newQ)
               /\ revocationOccurred' =
                    IF grantedReaders = {} THEN revocationOccurred ELSE TRUE
          ELSE \* cannot grant: only e is removed
               /\ activeReaders' = activeReaders
               /\ writerOwner' = writerOwner
               /\ grantedReaders' = grantedReaders
               /\ nodeState' = [nodeState EXCEPT ![e] = "Cancelled"]
               /\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]
               /\ publicationCount' = [publicationCount EXCEPT ![e] = 1]
               /\ queue' = newQ
               /\ revocationOccurred' = revocationOccurred
    /\ UNCHANGED <<mode, bargingOccurred, writerWasQueued, deadlineDue,
                   admissionSawResource, admissionSawDue>>

\* ExpireQueued: same semantics as cancel (different outcome label)
ExpireQueued(e) ==
    /\ nodeState[e] = "Queued"
    /\ \E i \in 1..Len(queue) : queue[i] = e
    /\ LET pos == CHOOSE i \in 1..Len(queue) : queue[i] = e
           newQ == SubSeq(queue, 1, pos - 1) \o SubSeq(queue, pos + 1, Len(queue))
       IN
       /\ IF pos = 1 /\ Len(newQ) > 0 /\ writerOwner = NoWriter /\
             mode[Head(newQ)] = "read"
          THEN LET prefix == ReaderPrefixLen(newQ)
               IN /\ activeReaders' = activeReaders + prefix
                  /\ grantedReaders' = grantedReaders \cup
                       {newQ[i] : i \in 1..prefix}
                  /\ writerOwner' = NoWriter
                  /\ nodeState' = [i \in Epochs |->
                       IF i = e THEN "Expired"
                       ELSE IF \E j \in 1..prefix : newQ[j] = i
                       THEN "Woken" ELSE nodeState[i]]
                  /\ resolutionCount' = [i \in Epochs |->
                       IF i = e THEN 1
                       ELSE IF \E j \in 1..prefix : newQ[j] = i
                       THEN 1 ELSE resolutionCount[i]]
                  /\ publicationCount' = [i \in Epochs |->
                       IF i = e THEN 1
                       ELSE IF \E j \in 1..prefix : newQ[j] = i
                       THEN 1 ELSE publicationCount[i]]
                  /\ queue' = SubSeq(newQ, prefix + 1, Len(newQ))
                  /\ revocationOccurred' = revocationOccurred
          ELSE IF pos = 1 /\ Len(newQ) > 0 /\ writerOwner = NoWriter /\
                    mode[Head(newQ)] = "write" /\ activeReaders = 0
          THEN /\ activeReaders' = 0
               /\ grantedReaders' = {}
               /\ writerOwner' = Head(newQ)
               /\ nodeState' = [i \in Epochs |->
                    IF i = e THEN "Expired"
                    ELSE IF i = Head(newQ) THEN "Woken"
                    ELSE nodeState[i]]
               /\ resolutionCount' = [i \in Epochs |->
                    IF i = e THEN 1
                    ELSE IF i = Head(newQ) THEN 1
                    ELSE resolutionCount[i]]
               /\ publicationCount' = [i \in Epochs |->
                    IF i = e THEN 1
                    ELSE IF i = Head(newQ) THEN 1
                    ELSE publicationCount[i]]
               /\ queue' = Tail(newQ)
               /\ revocationOccurred' =
                    IF grantedReaders = {} THEN revocationOccurred ELSE TRUE
          ELSE /\ activeReaders' = activeReaders
               /\ writerOwner' = writerOwner
               /\ grantedReaders' = grantedReaders
               /\ nodeState' = [nodeState EXCEPT ![e] = "Expired"]
               /\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]
               /\ publicationCount' = [publicationCount EXCEPT ![e] = 1]
               /\ queue' = newQ
               /\ revocationOccurred' = revocationOccurred
    /\ UNCHANGED <<mode, bargingOccurred, writerWasQueued, deadlineDue,
                   admissionSawResource, admissionSawDue>>

\* ---- Specification ----
Next ==
    \/ \E e \in Epochs : ReadAdmit(e) \/ ReadQueue(e)
    \/ \E e \in Epochs : WriteAdmit(e) \/ WriteQueue(e)
    \/ \E e \in Epochs : ReadUntilAdmit(e) \/ ReadUntilExpired(e)
                       \/ ReadUntilSuspend(e)
    \/ \E e \in Epochs : WriteUntilAdmit(e) \/ WriteUntilExpired(e)
                       \/ WriteUntilSuspend(e)
    \/ \E e \in grantedReaders : UnlockRead(e)
    \/ \E e \in Epochs : writerOwner = e /\ UnlockWrite(e)
    \/ \E e \in Epochs : CancelQueued(e) \/ ExpireQueued(e)

Spec == Init /\ [][Next]_vars

\* ---- Invariants ----

\* RW1: Mutual exclusion
MutualExclusion ==
    (writerOwner # NoWriter) => (activeReaders = 0 /\ grantedReaders = {})

\* RW1b: writer and readers never overlap
NoReaderWriterOverlap ==
    activeReaders > 0 => writerOwner = NoWriter

\* RW2: No reader barging - bargingOccurred must never become TRUE
\* In the correct model, ReadAdmit requires Len(queue)=0, so no reader
\* can be admitted while a writer is queued.
NoReaderBarging ==
    bargingOccurred = FALSE

\* RW3: Reader batch correctness (granted readers count matches activeReaders)
ReaderBatchCorrectness ==
    activeReaders = Cardinality(grantedReaders)

\* RW5: Terminal uniqueness
TerminalUniqueness ==
    \A e \in Epochs : resolutionCount[e] <= 1

\* RW6: Publication uniqueness
PublicationUniqueness ==
    \A e \in Epochs : publicationCount[e] <= 1

\* RW7: No linked terminal node
NoLinkedTerminal ==
    \A i \in 1..Len(queue) : nodeState[queue[i]] = "Queued"

\* RW8: ActiveReader integrity
ActiveReaderIntegrity ==
    activeReaders >= 0 /\ activeReaders = Cardinality(grantedReaders)

\* Queue well-formedness (no duplicates)
QueueNoDuplicates ==
    \A i, j \in 1..Len(queue) : i # j => queue[i] # queue[j]

\* Writer owner consistency
WriterOwnerConsistency ==
    (writerOwner # NoWriter) => nodeState[writerOwner] = "Woken"

\* RW4/RW9 head-reconcile closure (audit 2026-08-18): an IDLE lock never
\* strands a grantable queued head. Every path that leaves the lock idle
\* (last-reader unlock, writer unlock, head-cancel/expire reconcile) grants
\* the next head in the same atomic step. A settled state with no writer,
\* zero readers, and a Queued head is a stalled queue — exactly what a
\* two-step "release then wake" refactor (dropping
\* rwlock_grant_from_head_locked on the release path) produces.
InvNoStrandedGrantableHead ==
    \/ writerOwner # NoWriter
    \/ activeReaders > 0
    \/ Len(queue) = 0
    \/ nodeState[Head(queue)] # "Queued"

\* RW10: Reader non-revocation (audit #162 MODEL-002 closure).
\* A granted reader may disappear only through its OWN UnlockRead. The ghost
\* revocationOccurred is settable only by a writer-grant reconcile that clears
\* a live reader set — excluded by the C++ guard `active_readers > 0 ||
\* writer_active` (scheduler_rwlock.cpp:119) which the cancel/expire writer
\* branches above encode as `activeReaders = 0`. The negative model
\* E12RwLockNegWriterRevoke drops that guard and this invariant must fail.
ReaderRevocationFree ==
    revocationOccurred = FALSE

\* RW11: resource-first deadline admission (audit #162 MODEL-003 closure).
\* A *_lock_until admission that saw an admissible resource AND an already-due
\* deadline must resolve Woken: precedence 1 (resource claim,
\* scheduler_rwlock.cpp rwlock_{read,write}_lock_until) beats precedence 2
\* (due deadline, E11 I5). The Expired-at-admission actions latch
\* admissionSawResource = FALSE, so a precedence inversion — a due deadline
\* winning over an admissible resource — violates this invariant. Parity with
\* E12Semaphore InvPermitFirstDeadline (P7) and E12AsyncMutex (M7); the
\* evidence latches make it a prime-free state predicate.
InvResourceFirstDeadline ==
    \A e \in Epochs :
        (admissionSawResource[e] = TRUE /\ admissionSawDue[e] = TRUE)
        => nodeState[e] = "Woken"

\* ---- Reachability witnesses (non-vacuity, audit #162 Phase 4) ----
\* Reverse-invariant form (suite pattern): NoReachX == ~ReachXState must be
\* VIOLATED by TLC — the CEX trace IS the witness that the state is genuinely
\* reachable. The reach cfgs add INVARIANT NoReachX one per cfg.

\* Cancel-of-head reconcile actually granted a reader prefix that MERGED with
\* a live reader set (T14 topology: E1 holding, E2 canceled writer head,
\* E3 queued reader granted while E1 still holds).
ReachCancelReaderPrefixMergeState ==
    \E c \in Epochs : nodeState[c] = "Cancelled" /\
        \E r \in Epochs : r # c /\ mode[r] = "read" /\
            nodeState[r] = "Woken" /\ r \in grantedReaders /\
            activeReaders >= 2
NoReachCancelReaderPrefixMerge == ~ReachCancelReaderPrefixMergeState

\* Expire-of-head reconcile merged a reader prefix into a live reader set.
ReachExpireReaderPrefixMergeState ==
    \E c \in Epochs : nodeState[c] = "Expired" /\
        \E r \in Epochs : r # c /\ mode[r] = "read" /\
            nodeState[r] = "Woken" /\ r \in grantedReaders /\
            activeReaders >= 2
NoReachExpireReaderPrefixMerge == ~ReachExpireReaderPrefixMergeState

\* Cancel exposed a WRITER head that remains blocked while active readers stay
\* (R1 topology: E1 holding, E2 canceled, E3 writer head still Queued).
ReachCancelWriterRefusedState ==
    \E c \in Epochs : nodeState[c] = "Cancelled" /\
        \E w \in Epochs : w # c /\ mode[w] = "write" /\
            nodeState[w] = "Queued" /\ activeReaders > 0 /\
            writerOwner = NoWriter
NoReachCancelWriterRefused == ~ReachCancelWriterRefusedState

\* Expire exposed a WRITER head that remains blocked while active readers stay.
ReachExpireWriterRefusedState ==
    \E c \in Epochs : nodeState[c] = "Expired" /\
        \E w \in Epochs : w # c /\ mode[w] = "write" /\
            nodeState[w] = "Queued" /\ activeReaders > 0 /\
            writerOwner = NoWriter
NoReachExpireWriterRefused == ~ReachExpireWriterRefusedState

\* A writer is queued while active readers remain (the Writer-Wall admission
\* discipline; reachable without any cancel — kept as a contrast gate that
\* even the dead-reconcile mutant satisfies via plain WriteQueue).
ReachWriterBlockedByReadersState ==
    \E w \in Epochs : mode[w] = "write" /\ nodeState[w] = "Queued" /\
        activeReaders > 0 /\ writerOwner = NoWriter
NoReachWriterBlockedByReaders == ~ReachWriterBlockedByReadersState

\* Timed admission exercised precedence 1 with a genuinely due deadline
\* (MODEL-003 non-vacuity): Woken with the due latch set is precisely a
\* ReadUntilAdmit/WriteUntilAdmit resolution where due = TRUE — the resource
\* beat the deadline. (A parked epoch that later wakes suspends with
\* admissionSawDue = FALSE and the latches are never touched again.)
ReachUntilResourceBeatDueState ==
    \E e \in Epochs :
        /\ admissionSawDue[e] = TRUE
        /\ nodeState[e] = "Woken"
NoReachUntilResourceBeatDue == ~ReachUntilResourceBeatDueState

\* The admission-expire half (precedence 2) genuinely fires: Expired with the
\* due latch set is precisely the inline ReadUntilExpired/WriteUntilExpired
\* resolution (a parked epoch that a timer later expires suspended with
\* admissionSawDue = FALSE; ExpireQueued never touches the latches).
ReachUntilExpiredState ==
    \E e \in Epochs :
        /\ admissionSawDue[e] = TRUE
        /\ nodeState[e] = "Expired"
NoReachUntilExpired == ~ReachUntilExpiredState

============
