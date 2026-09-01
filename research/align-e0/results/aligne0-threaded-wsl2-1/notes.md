# aligne0-threaded-wsl2-1 — INVALID (harness bug, kept immutable)

SECONDARY TOPOLOGY DIAGNOSTIC first attempt. FAILED same-work gate (40
errors): thread_main looped over cfg.reps internally while the caller also
created a thread set per rep, so each thread did reps x kc ops per rep and
per-rep ops/bytes differed between R7 and R14 (7x vs 14x inflation). The
gate correctly failed closed. Fixed (thread body now does one rep = kc ops)
and re-run as aligne0-threaded-wsl2-2. This session is retained, unedited,
as the failed-attempt evidence; no analysis is drawn from it.
