--------------------------- MODULE E12RwLockNegReaderBypass ---------------------------
\* NEGATIVE MODEL: Reader bypasses queued writer (violates writer-fair admission).
\* The ReadAdmit action drops the Len(queue)=0 guard, allowing readers to barge
\* ahead of queued writers. This MUST violate WriterFairness.
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Epochs, E1, E2, E3

VARIABLES
    activeReaders, writerOwner, queue, mode, nodeState,
    resolutionCount, publicationCount, grantedReaders,
    writerWasQueued  \* history: has a writer ever been queued?

NoWriter == 999
vars == <<activeReaders, writerOwner, queue, mode, nodeState,
          resolutionCount, publicationCount, grantedReaders, writerWasQueued>>

RECURSIVE ReaderPrefixLen(_)
ReaderPrefixLen(q) ==
    IF Len(q) = 0 THEN 0
    ELSE IF mode[Head(q)] = "write" THEN 0
    ELSE 1 + ReaderPrefixLen(Tail(q))

Init ==
    /\ activeReaders = 0
    /\ writerOwner = NoWriter
    /\ queue = <<>>
    /\ mode = [e \in Epochs |-> "unset"]
    /\ nodeState = [e \in Epochs |-> "Free"]
    /\ resolutionCount = [e \in Epochs |-> 0]
    /\ publicationCount = [e \in Epochs |-> 0]
    /\ grantedReaders = {}
    /\ writerWasQueued = FALSE

\* BUG: ReadAdmit drops Len(queue)=0 check (allows barging)
ReadAdmit(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ writerOwner = NoWriter
    \* BUG: missing Len(queue) = 0 guard
    /\ mode' = [mode EXCEPT ![e] = "read"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Woken"]
    /\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]
    /\ grantedReaders' = grantedReaders \cup {e}
    /\ activeReaders' = activeReaders + 1
    /\ UNCHANGED <<writerOwner, queue, publicationCount, writerWasQueued>>

ReadQueue(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "read"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Queued"]
    /\ queue' = Append(queue, e)
    /\ UNCHANGED <<activeReaders, writerOwner, resolutionCount,
                   publicationCount, grantedReaders, writerWasQueued>>

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
    /\ UNCHANGED <<activeReaders, queue, publicationCount, grantedReaders, writerWasQueued>>

WriteQueue(e) ==
    /\ nodeState[e] = "Free"
    /\ mode[e] = "unset"
    /\ (activeReaders > 0 \/ writerOwner # NoWriter \/ Len(queue) > 0)
    /\ mode' = [mode EXCEPT ![e] = "write"]
    /\ nodeState' = [nodeState EXCEPT ![e] = "Queued"]
    /\ queue' = Append(queue, e)
    /\ writerWasQueued' = TRUE  \* record that a writer was queued
    /\ UNCHANGED <<activeReaders, writerOwner, resolutionCount,
                   publicationCount, grantedReaders>>

UnlockRead(e) ==
    /\ e \in grantedReaders
    /\ activeReaders > 0
    /\ grantedReaders' = grantedReaders \ {e}
    /\ activeReaders' = activeReaders - 1
    /\ UNCHANGED <<writerOwner, queue, nodeState, resolutionCount,
                   publicationCount, mode, writerWasQueued>>

UnlockWrite(e) ==
    /\ writerOwner = e
    /\ writerOwner' = NoWriter
    /\ IF Len(queue) > 0
       THEN LET prefix == ReaderPrefixLen(queue)
            IN IF prefix > 0
               THEN /\ activeReaders' = prefix
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
               ELSE /\ activeReaders' = 0
                    /\ grantedReaders' = {}
                    /\ writerOwner' = Head(queue)
                    /\ nodeState' = [nodeState EXCEPT ![Head(queue)] = "Woken"]
                    /\ resolutionCount' = [resolutionCount EXCEPT ![Head(queue)] = 1]
                    /\ publicationCount' = [publicationCount EXCEPT ![Head(queue)] = 1]
                    /\ queue' = Tail(queue)
       ELSE /\ activeReaders' = 0
            /\ grantedReaders' = {}
            /\ UNCHANGED <<queue, nodeState, resolutionCount, publicationCount>>
    /\ UNCHANGED <<mode, writerWasQueued>>

Next ==
    \/ \E e \in Epochs : ReadAdmit(e) \/ ReadQueue(e)
    \/ \E e \in Epochs : WriteAdmit(e) \/ WriteQueue(e)
    \/ \E e \in grantedReaders : UnlockRead(e)
    \/ \E e \in Epochs : writerOwner = e /\ UnlockWrite(e)

Spec == Init /\ [][Next]_vars

\* Writer fairness: once a writer has been queued, no reader can be admitted
\* inline (ReadAdmit). In the buggy model, ReadAdmit can fire even when
\* writerWasQueued is TRUE, violating this invariant.
WriterFairness ==
    writerWasQueued => \A e \in grantedReaders : mode[e] = "write" \/ nodeState[e] # "Woken"

=============================================================================
