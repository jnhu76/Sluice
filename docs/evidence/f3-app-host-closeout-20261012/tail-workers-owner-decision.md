# TAIL workers CLI decision — unsigned candidate

Issue #474; this scope is separate from the 17 F4 installed interfaces.
Recommended policy: REJECT_REMOVED_OPTION. Human owner decision: PENDING.

At c29523d0c54c7bc85e2d5e7db73887acb991308c, `--workers 1..64` was
accepted and advertised as a compatibility hint, although one app-owned driver
executed regardless. The proposed change rejects every value/spelling with
usage exit 1 before target-file open, including file-first and help combinations.
Scripts invoking the former option must remove it. No deprecation window,
shim or multi-worker runtime is proposed. External CLI usage remains UNKNOWN.
COPY's real ThreadPool workers remain valid. HASH/GREP CLI semantics are unchanged.

The candidate removes CliArgs.workers, TailOptions.workers, parse_workers and
its inert validation; help and current README agree. Independent syscall traces
show no target open. Existing driver/stop/borrow/join protocols are unchanged.
This Draft must not be merged or #474 closed without a separate active human
owner decision. Approval of F4 header removal does not cover this scope.

```ini
OWNER = <human account and role>
DECISION_DATE = <active confirmation date>
DECISION_SCOPE = TAIL --workers CLI and inert app-private option fields
REVIEWED_PR_HEAD = <full final PR SHA>
TAIL_WORKERS_POLICY = <REJECT_REMOVED_OPTION or decline>
MIGRATION_DISCLOSURE = existing callers must omit --workers; usage exit 1 before open
EXTERNAL_USE = UNKNOWN
APPROVED_FOR_SCOPED_CLI_BREAK_AND_MERGE = PENDING
```
