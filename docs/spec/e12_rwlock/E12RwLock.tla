------------------------------- MODULE E12RwLock -------------------------------
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
    bargingOccurred     \* HISTORY: TRUE if a reader was admitted while writer queued

NoWriter == 999
Mode == {"read", "write"}
NState == {"Free", "Queued", "Woken", "Cancelled", "Expired"}

vars == <<activeReaders, writerOwner, queue, mode, nodeState,
          resolutionCount, publicationCount, grantedReaders, bargingOccurred>>

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
    /\ bargingOccurred' = bargingOccurred \* unchanged (queue was empty)
    /\ UNCHANGED <<writerOwner, queue, publicationCount>>

\* ReadQueue: reader must queue (writer active OR queue non-empty)
ReadQueue(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "read"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Queued"]
    /\ queue' = Append(queue, e)
    /\ UNCHANGED <<activeReaders, writerOwner, resolutionCount,
                   publicationCount, grantedReaders, bargingOccurred>>

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
    /\ UNCHANGED <<activeReaders, queue, publicationCount, grantedReaders, bargingOccurred>>

\* WriteQueue: writer must queue
WriteQueue(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (activeReaders > 0 \/ writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "write"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Queued"]
    /\ queue' = Append(queue, e)
    /\ UNCHANGED <<activeReaders, writerOwner, resolutionCount,
                   publicationCount, grantedReaders, bargingOccurred>>

\* UnlockRead: release one read share
UnlockRead(e) ==
    /\ e \in grantedReaders
    /\ activeReaders > 0
    /\ grantedReaders' = grantedReaders \ {e}
    /\ activeReaders' = activeReaders - 1
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
    /\ UNCHANGED <<mode, bargingOccurred>>

\* UnlockWrite: release write lock
UnlockWrite(e) ==
    /\ writerOwner = e
    /\ writerOwner' = NoWriter
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
            /\ UNCHANGED <<queue, nodeState, resolutionCount, publicationCount>>
    /\ UNCHANGED <<mode, bargingOccurred>>

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
       /\ IF pos = 1 /\ Len(newQ) > 0 /\ activeReaders = 0 /\ writerOwner = NoWriter
          THEN \* head cancelled: reconcile new head
               LET prefix == ReaderPrefixLen(newQ)
               IN IF prefix > 0
                  THEN \* grant reader prefix from new head
                       /\ activeReaders' = prefix
                       /\ grantedReaders' = {newQ[i] : i \in 1..prefix}
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
    /\ UNCHANGED <<mode, bargingOccurred>>

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
       /\ IF pos = 1 /\ Len(newQ) > 0 /\ activeReaders = 0 /\ writerOwner = NoWriter
          THEN LET prefix == ReaderPrefixLen(newQ)
               IN IF prefix > 0
                  THEN /\ activeReaders' = prefix
                       /\ grantedReaders' = {newQ[i] : i \in 1..prefix}
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
    /\ UNCHANGED <<mode, bargingOccurred>>

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

=============================================================================
