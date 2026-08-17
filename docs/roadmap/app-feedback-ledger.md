# App Feedback Ledger

Application-driven API feedback from the Sluice reference applications. One
application inconvenience does NOT authorize a general abstraction; a
convenience abstraction normally requires repeated demand from two independent
apps. A fundamental missing capability that makes Runtime I/O unusable MAY be
fixed now.

## Rules

- one application inconvenience does not authorize a general abstraction;
- a fundamental missing capability that makes Runtime I/O unusable may be fixed
  now;
- a convenience abstraction normally requires repeated demand from two
  independent apps.

## Ledger

| ID     | App         | Friction                             | Fundamental or convenience             | App-local workaround      | Core change      | Status              |
| ------ | ----------- | ------------------------------------ | -------------------------------------- | ------------------------- | ---------------- | ------------------- |
| AF-001 | sluice-copy | Runtime task cannot await Completion | fundamental                            | none acceptable           | winning race API | resolved (M1-A)     |
| AF-002 | sluice-copy | task has no typed return channel     | convenience / possible future capability | app result slot + CV      | no               | deferred            |
| AF-003 | sluice-copy | no public fd RAII wrapper            | convenience                            | app-local RAII (ScopedFd) | no               | deferred            |
| AF-004 | sluice-copy | no async write-all helper            | convenience                            | explicit write loop       | no               | requires second app |
| AF-005 | sluice-copy | no safe-output temp+rename helper    | Version C feature                      | documented partial output | no               | deferred            |

## Detail

### AF-001 — Runtime task cannot await Completion (resolved)

`RuntimeTaskContext` could submit async I/O but exposed no supported cooperative
wait for the caller-owned `Completion`. This was fundamental: without it, real
asynchronous I/O from a Runtime task was impossible without a forbidden
substitute (busy-poll, condition_variable on the Worker, std::future, raw
Scheduler access). Resolved in M1-A by adding
`RuntimeTaskContext::await_completion` (Candidate A of the horse race,
`docs/history/implementation-plans/m1-runtime-io-await-race.md`), which delegates to the already-
audited `Scheduler::await_completion_*` primitive. Core change: a private
`Scheduler*` in the production context + 2 public overloads.

### AF-002 — task has no typed return channel (deferred)

`ApplicationRuntime::submit()` runs a `void` task, so the app must publish a
typed result through an app-owned slot (mutex + optional<Result<T>> +
condition_variable) whose lifetime exceeds the task. This is a convenience
friction, not a defect: the main thread reads the slot only after `drain()`,
and the Runtime Worker never blocks on it. Redesigning Runtime task return
types would be a separate race and is NOT done in M1-A. Deferred until a
second app independently needs it.

### AF-003 — no public fd RAII wrapper (deferred)

The app uses an app-local `ScopedFd` RAII guard. Promoting an fd wrapper into
core based on one app is premature. Deferred.

### AF-004 — no async write-all helper (requires second app)

The copy implements an explicit write loop consuming each chunk (handling
partial writes and rejecting zero progress). A core async `write_all` helper
would reduce boilerplate, but per the rules a convenience abstraction needs
repeated demand from two independent apps. Requires confirmation from a second
app (e.g. `sluice-wal`).

### AF-005 — no safe-output temp+rename helper (deferred)

Version A documents a partial-output limitation (on mid-copy failure the
destination may be left partial). Atomic temp-file + rename safe output is a
Version C feature, intentionally out of scope for M1-A. Deferred.
