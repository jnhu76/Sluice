# F3 app-host scoped candidate receipt

Actual application matrix source: `613f9205308ecbcc7635f946fe0061f57b862e71`.
Four fresh Debug/Release × no-liburing/real-liburing configurations PASS in
`f3-matrix-01/evidence.json`; command argv, exit, stdout/stderr and hashes are preserved.
Importing merged F4 master changes no measured application/library inputs; see input-equivalence.json.
No fresh result is relabeled to a later documentation/import commit.

| Obligation | Independent observation and boundary |
|---|---|
| D-H1 | GREP actual delimiter/output oracle; direct CLI oracle preserves literal colon output |
| D-H2 | COPY shared valid_config preflight now rejects out-of-range/overflow before target open; 32 new syscall-oracle cases; baseline24 genuine failures |
| D-H3 | COPY pipeline oracle cancellation, exception cleanup, held borrow, primary error precedence; no unreachable waiter-failure claim |
| D-H4 | TAIL Result propagation, lifecycle and CLI signal/partial-line oracles; join/thread-creation injection unclaimed |
| D-H5 / T2-08 | Each app owns invocation/driver choice; no public ApplicationRuntime restored |
| R1 | HASH/GREP CLI oracles, COPY CLI smoke and preflight, TAIL34 preflight cases/help |
| R2 | RequestScope/shutdown, COPY held-borrow cleanup and TAIL lifecycle tests |
| R3 | Short-I/O semantic reference, COPY pipeline primary error and real RLIMIT_FSIZE failure |
| R4 | TAIL SIGINT/SIGTERM/stop/wait CLI and lifecycle; watchdog is observed test bound, not unconditional settlement guarantee |
| R5 | COPY atomic/non-atomic RLIMIT_FSIZE artifact oracle, directory-fsync error/EINTR oracle |
| R6 / OB-1 | COPY VersionB independent full-read-window, ordered writer, short-I/O/exception/cancel/borrow interleavings |
| OB-2 | Safe output plus directory-fsync and actual failure artifacts |
| OB-3 | TAIL lastN, descriptor-follow, truncate, long/partial lines, signal/stop/borrow/join witnesses |

The seven DOC_CLAIM dispositions are: COPY workers remain actual ThreadPool workers;
TAIL workers reject/remove candidate awaits separate human decision; GREP/HASH workers
reject removed --workers with migration diagnostics and omit it from help; COPY bounded inflight requests and single ordered writer
are witnessed; buffer×depth bounds use the shared overflow-safe predicate; primary error
plus drain is exercised. Current README descriptions match these scopes.

Existing normal matrix tests remain retained; no canonical test was deleted to obtain green.
This patch changes preflight/configuration and stale product descriptions, not a borrow,
publication, driver or shutdown protocol. No protocol model is newly claimed.

TAIL policy remains REJECT_REMOVED_OPTION proposed, owner PENDING. Scripts must omit
--workers; external use UNKNOWN. The17 installed-header approval does not cover this CLI.
PR #513 remains Draft and #474 remains OPEN absent separate active human authorization.
