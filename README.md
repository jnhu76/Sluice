# Sluice Research Knowledge Archive

Knowledge-only archive of five closed research efforts: structure, code
references, and conclusions — no raw experimental data. Future
re-verification means writing new experiments from these records, not
replaying old ones.

| Directory | Issue | Topic | Verdict |
|---|---|---|---|
| research-575 | #575 | Atomic RMW counters on I/O hot paths | NO_CHANGE_JUSTIFIED |
| research-576 | #576 | ThreadPool zero-owed delivery scan | NO_CHANGE_JUSTIFIED |
| research-593 | #593 | Execution-path slicing method pilot | NOT_RUN (method handed to #594) |
| research-594 | #594 | Key-path K1–K4 counterfactual elision | K2/K4 elision candidate found (form-gated); product change NONE |
| research-612 | #612 | Direct P2C1 read compositions | MERGED (84e1b7d5); eofmix+cursor WIN, comp-read-4k INCONCLUSIVE |

Each `research-<issue>/README.md` answers: research question, method
structure, code references (Git SHAs/paths/PRs), conclusions with
applicability boundaries, and the next hypotheses worth measuring.
`code/` exists only where research code exists in no git history.
