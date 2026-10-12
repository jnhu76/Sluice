"""Render scoped owner-review packets from completed F4 technical evidence."""
import argparse
import hashlib
import json
from pathlib import Path

ap=argparse.ArgumentParser();ap.add_argument('evidence',type=Path);ap.add_argument('output',type=Path);ap.add_argument('--archive-sha256',required=True);args=ap.parse_args()
data=json.loads((args.evidence/'evidence.json').read_text())
args.output.mkdir(parents=True,exist_ok=True)
for group in (508,510):
 surfaces=[s for s in data['manifest']['surfaces'] if s['group']==group]
 risks=[dict(r) for r in data['risk'] if r['SURFACE_ID'] in {s['id'] for s in surfaces}]
 for risk in risks:
  risk['RETAINED_CANONICAL_WORKLOAD']={'result':'W01/W02/W03 ThreadPool and real-uring PASS; no-liburing uring UNAVAILABLE; W04 support NO','raw_reference':'run-02/evidence.json packages; command IDs, exit codes and log hashes','consumer_lifetime_fix':'#511; measured library production unchanged'}
  risk['CURRENT_IN_TREE_CONSUMER']={'command':risk['CURRENT_IN_TREE_CONSUMER']['command'],'raw_reference':'run-02/evidence.json exact git grep stdout; does not imply zero external users'}
 head=data['trees'][f'C{group}']['sha']
 title='BlockingIoPool' if group==508 else 'Legacy File/Reader/Writer/stream'
 lines=[f'# F0 §1.7 owner-review packet — #{group}: {title}', '',
 'Status: technical candidate; human owner PENDING. External use UNKNOWN for every row.', '',
 f'Measured PR HEAD: `{head}`. Raw evidence SHA256: `{args.archive_sha256}`.',
 f"Execution baseline: `{data['trees']['B0']['sha']}`; H0: `{data['trees']['H0']['sha']}`.",
 f"Combined tested SHA: `{data['trees']['C508510']['sha']}`; tree: `{data['trees']['C508510']['tree']}`.", '',
 'Authority: root v1-r4 PROD-03/W-01–W-03/MIG-01/02/03; F0 §1.7/1.8; adopted Phase M; #475 HOLD/KEEP.', '',
 'H0 contains all 17 headers in its public source include tree. Its libraries have no explicit header installation rules. The H0 API reference calls selected APIs stable-ish and requires deliberate deprecation; README calls the software experimental. Publication does not give every header the same stability promise. Source, API reference, README and rules are preserved verbatim. The pool implementation fragment fails standalone include but compiles through its public parent; a direct external include still breaks on removal.', '',
 'No compatibility shim is added. Canonical per-operation blocking I/O is not a generic task pool or stream-product replacement. Existing external source consumers must change includes, types and calls; previously compiled consumers may fail to relink. A static executable already linked does not automatically lose its copied code. No zero-external-users claim is made.', '',
 'The existing W-02 example contained a real borrow-lifetime defect. Original ASan UAF and TSan race diagnostics are retained. #511 moves buffers before RequestScope; #512 uses that corrected consumer against the exact unchanged candidate library trees. Adoption of the consumer correction is a separate evidence prerequisite. W-04 remains OPTIONAL_CANDIDATE and support is NO.', '',
 '| Surface | Header | Retired API | Migration or no equivalent | Proposed policy | Owner |',
 '|---|---|---|---|---|---|']
 for s in surfaces:
  lines.append(f"| {s['id']} | `{s['header']}` | {', '.join(s['types']+s['callables'])} | {s['migration']} | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |")
 if group==508:
  lines += ['', 'This removes two installed headers and the pool core object. Task<T>, PoolStats, pool factory, submission/admission and shutdown callables cease to be available. External task-pool users may need architectural refactoring; there is no v1 generic-pool equivalent. Historical source exposure is expressly disclosed, not silently erased.']
 else:
  lines += ['', 'Functional relationships: native FileReader/FileWriter resource ownership and Reader/Writer polymorphic stream calls retire together; IoContext/BlockingIoContext factories and MemoryIoContext leave with that stream closure. Copy/Buffer strategies and peek/consume buffers have no supplied v1 stream adapter. WAL codec/LSN tracking has no v1 equivalent. Fault/observed wrappers and memory doubles lose installed exposure. IoSlice/ConstIoSlice, CopyLimit and SyncableWriter auxiliary types also disappear. Canonical File plus blocking/file.hpp and AsyncIoContext offer partial scalar-file and durability migration only. Vectored/generic-stream operations are excluded. Removed sluice/async/file.hpp is not a migration target.']
 lines += ['', '## Per-interface machine records', '', 'These rows bind baseline compile/link/behavior witnesses and candidate negative probes to profile/mode, command IDs, exit codes and log hashes in evidence.json. Header-only rows explicitly have no exclusive callable ABI. Group strong-symbol audits and real baseline-object negative relinks cover compiled legacy callables.', '', '```json',json.dumps(risks,indent=2),'```', '', '## F0 §1.7 Owner Decision Record — unsigned template', '',
 'An identified human phase owner must actively publish or confirm this record. Fill each value and confirm a policy for every table row; no checkbox is preselected. A material code/risk scope change requires renewed confirmation. This agent has not signed the record.', '', '```ini',
 'OWNER = <human account and phase-owner role>', 'DECISION_DATE = <date of active confirmation>',
 f'DECISION_SCOPE = PR #{group}; enumerated surfaces '+', '.join(s['id'] for s in surfaces),
 'REVIEWED_PR_HEAD = <full final PR SHA actively reviewed>',
 f'REVIEWED_EVIDENCE_SHA256 = {args.archive_sha256}',
 'ACCEPTED_BREAKAGE_SURFACES = <enumerate per-row accepted surfaces>',
 'DEPRECATION_POLICY = <per-row ACCEPT_BREAKAGE_NOW / BOUNDED_DEPRECATION_WINDOW / BLOCK_AND_REVISE>',
 'MIGRATION_DISCLOSURE = <confirm each migration/no-equivalent and historical exposure consequence>',
 'APPROVED_FOR_SCOPED_REMOVAL = <human decision; presently PENDING>', '```', '']
 (args.output/f'f4-{group}-owner-decision.md').write_text('\n'.join(lines))
