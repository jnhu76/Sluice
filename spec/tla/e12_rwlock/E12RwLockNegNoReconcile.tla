------------------------------- MODULE E12RwLockNegNoReconcile -------------------------------
\* NEGATIVE MODEL (audit-added, NEG-RW2): this variant intentionally DROPS the
\* grant-from-head reconcile on reader release — the two-step unlock
\* regression where the last reader completes but the next queued (grantable)
\* head is left stranded. It deliberately breaks RW4 Head Reconcile below;
\* the expected TLC verdict is a VIOLATION of InvNoStrandedGrantableHead
\* (cfg: E12RwLockNegNoReconcile.cfg). All other RW laws remain intact so
\* the named check is exact. The rest of this header is the shared base-model
\* scope description.
\*
\* sluice::async::AsyncRwLock -- writer-fair phase-batched RwLock SAFETY model
\* (E12-F, authority docs/e12-rwlock.md).
\*
\* Key safety properties:
\*   RW1  Mutual Exclusion        writer active => no readers; readers > 0 => no writer
\*   RW2  Writer-Fair Admission   new reader cannot barge past queued writer
\*   RW3  Reader Batch Correctness grant grants maximal reader prefix (stops at writer)
\*   RW4  Head Reconcile          cancel/expire of head immediately advances next
\*   RW5  Terminal Uniqueness     each epoch resolves at most once
\*   RW6  Publication Uniqueness  each epoch published at most once
\*   RW7  No Linked Terminal      terminal epoch is not in queue
\*   RW8  ActiveReader Integrity  activeReaders never underflows
\*
\* SCOPE: SAFETY-ONLY. Admission is ONE atomic step (register+recheck+disposition).
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
    writerWasQueued     \* HISTORY: TRUE if a writer has ever been queued (for barging obs)

NoWriter == 999
Mode == {"read", "write"}
NState == {"Free", "Queued", "Woken", "Cancelled", "Expired"}

vars == <<activeReaders, writerOwner, queue, mode, nodeState,
          resolutionCount, publicationCount, grantedReaders, bargingOccurred,
          writerWasQueued>>

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
    \* In the correct model, ReadAdmit requires Len(queue)=0, so no writer is
    \* queued at admission time — barging cannot occur here. bargingOccurred
    \* stays unchanged (the negative model overrides ReadAdmit to record the
    \* violation when it drops the Len(queue)=0 guard).
    /\ UNCHANGED <<writerOwner, queue, publicationCount, bargingOccurred, writerWasQueued>>

\* ReadQueue: reader must queue (writer active OR queue non-empty)
ReadQueue(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "read"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Queued"]
    /\ queue' = Append(queue, e)
    /\ UNCHANGED <<activeReaders, writerOwner, resolutionCount,
                   publicationCount, grantedReaders, bargingOccurred, writerWasQueued>>

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
    /\ UNCHANGED <<activeReaders, queue, publicationCount, grantedReaders, bargingOccurred, writerWasQueued>>

\* WriteQueue: writer must queue
WriteQueue(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (activeReaders > 0 \/ writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "write"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Queued"]
    /\ queue' = Append(queue, e)
    /\ writerWasQueued' = TRUE
    /\ UNCHANGED <<activeReaders, writerOwner, resolutionCount,
                   publicationCount, grantedReaders, bargingOccurred>>

\* UnlockRead: release one read share
UnlockRead(e) ==
    /\ e \in grantedReaders
    /\ activeReaders > 0
    /\ IF FALSE \* BUG (NEG-RW2): last-reader unlock drops the head reconcile
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
    /\ UNCHANGED <<mode, bargingOccurred, writerWasQueued>>

\* UnlockWrite: release write lock
UnlockWrite(e) ==
    /\ writerOwner = e
    /\ IF FALSE \* BUG (NEG-RW2): writer unlock drops the head reconcile
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
    /\ UNCHANGED <<mode, bargingOccurred, writerWasQueued>>

\* CancelQueued: cancel a queued epoch + reconcile head
CancelQueued(e) ==
    /\ nodeState[e] = "Queued"
    /\ \E i \in 1..Len(queue) : queue[i] = e
    /\ LET pos == CHOOSE i \in 1..Len(queue) : queue[i] = e
           newQ == SubSeq(queue, 1, pos - 1) \o SubSeq(queue, pos + 1, Len(queue))
       IN
       /\ nodeState' = [nodeState EXCEPT ![e] = "Cancelled"]
       /\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]
       /\ publicationCount' = [publicationCount EXCEPT ![e] = 1]
       /\ IF pos = 1 /\ Len(newQ) > 0 /\ writerOwner = NoWriter
          THEN \* head cancelled: reconcile new head (admit reader prefix even
               \* if activeReaders > 0 — preserve existing readers and merge)
               LET prefix == ReaderPrefixLen(newQ)
               IN IF prefix > 0
                  THEN \* grant reader prefix from new head (merge into existing)
                       /\ activeReaders' = activeReaders + prefix
                       /\ grantedReaders' = grantedReaders \cup {newQ[i] : i \in 1..prefix}
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
                  ELSE IF mode[Head(newQ)] = "write"
                  THEN \* grant head writer
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
                  ELSE \* cannot grant
                       /\ queue' = newQ
                       /\ UNCHANGED <<activeReaders, writerOwner, grantedReaders>>
          ELSE \* non-head cancel or cannot grant
               /\ queue' = newQ
               /\ UNCHANGED <<activeReaders, writerOwner, grantedReaders>>
    /\ UNCHANGED <<mode, bargingOccurred, writerWasQueued>>

\* ExpireQueued: same semantics as cancel (different outcome label)
ExpireQueued(e) ==
    /\ nodeState[e] = "Queued"
    /\ \E i \in 1..Len(queue) : queue[i] = e
    /\ LET pos == CHOOSE i \in 1..Len(queue) : queue[i] = e
           newQ == SubSeq(queue, 1, pos - 1) \o SubSeq(queue, pos + 1, Len(queue))
       IN
       /\ nodeState' = [nodeState EXCEPT ![e] = "Expired"]
       /\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]
       /\ publicationCount' = [publicationCount EXCEPT ![e] = 1]
       /\ IF pos = 1 /\ Len(newQ) > 0 /\ writerOwner = NoWriter
          THEN LET prefix == ReaderPrefixLen(newQ)
               IN IF prefix > 0
                  THEN /\ activeReaders' = activeReaders + prefix
                       /\ grantedReaders' = grantedReaders \cup {newQ[i] : i \in 1..prefix}
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
                  ELSE IF mode[Head(newQ)] = "write"
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
                  ELSE /\ queue' = newQ
                       /\ UNCHANGED <<activeReaders, writerOwner, grantedReaders>>
          ELSE /\ queue' = newQ
               /\ UNCHANGED <<activeReaders, writerOwner, grantedReaders>>
    /\ UNCHANGED <<mode, bargingOccurred, writerWasQueued>>

\* ---- Specification ----
Next ==
    \/ \E e \in Epochs : ReadAdmit(e) \/ ReadQueue(e)
    \/ \E e \in Epochs : WriteAdmit(e) \/ WriteQueue(e)
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

============
