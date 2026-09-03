# G1-CONTROL-C0-AUDIT — mechanism + as-built audit (#279)

Audited on branch `research/g1-control-c0-fixed-file` from master
`39f9d984e562a6396b58ebbe733d89513dd7242a` (PR #278 merge; clean worktree).

Purpose: answer, from source evidence only, (a) the real Linux/liburing
ordinary-fd vs fixed-file resource mechanism on THIS host kernel, (b) whether
any registered/fixed-FILE support exists in Sluice today, (c) which
research-only topology C0 must therefore use, and (d) whether the Issue #279
candidate authority split ("Sluice owns logical binding meaning; Linux owns
physical kernel-resource lifetime") is supported by the mechanism.

No guessing. Every mechanism claim below is pinned to a source file and
function of the exact running kernel `7.1.9-200.fc44.x86_64` (source fetched
from the Bootlin Cross Referencer `v7.1.9` tree into
`probes/kernel-7.1.9/`; the host UAPI header `/usr/include/linux/io_uring.h`
and the xrepo-pinned liburing 2.14 headers are local), or to the Sluice
as-built tree at the audit SHA.

---

## 1. Sluice as-built state: registered/fixed-FILE support

Search terms (case-insensitive, over `src/ include/ apps/ bench/ tests/`):
`IOSQE_FIXED_FILE`, `IORING_OP_READ_FIXED`, `IORING_OP_WRITE_FIXED`,
`io_uring_register_files`, `IORING_REGISTER_FILES`, `fixed_file`,
`file_index`, `read_fixed`, `write_fixed`, `buf_index`.

Findings (audit SHA):

- **Zero production call sites.** `src/async/uring_backend.cpp` SQE prep is
  ordinary `io_uring_prep_read` / `io_uring_prep_write` /
  `io_uring_prep_fsync` only (`uring_backend.cpp:861-872`); no
  `IOSQE_FIXED_FILE` flag is ever set and no `sqe->file_index` path exists.
  `IORING_OP_READ_FIXED`/`WRITE_FIXED` appear only in the RBUF-E0 research
  bench as fixed-**buffer** ops (orthogonal mechanism; see §4 note).
- `xmake/experimental.lua:30-59` declares `with-uring-registered-files`
  (default OFF) threading `SLUICE_URING_REGISTERED_FILES` onto
  `sluice_async` — **no source file consumes the define** (grep over
  `src/ include/`: zero `#ifdef SLUICE_URING_REGISTERED_FILES`). The gate is
  plumbing inherited from the Zig reference design with no consumer.
- `docs/architecture/divergence-registry.md` DIV-09 "Registered
  Buffers/Files Deferred" (Status: Accepted, introduced by
  ADR-async-io-model §5): kernel-pinned buffers/files "require a lifetime
  contract that Sluice has not yet designed. Explicitly deferred." Revisit
  trigger: "when lifetime contract is designed" — exactly the G1-Control
  target.

### Classification

```text
FIXED-FILE PRODUCTION SUPPORT: NO SUPPORT EXISTS
FIXED-FILE CONTROL CONTRACT:   NOT DESIGNED (DIV-09, deferred)
```

(Not "partial": an unconsumed build gate is not support. Per the campaign
mandate this also means: C0 must NOT add the capability to the production
backend. The experimental topology decision follows RBUF-E0 precedent:
`RESEARCH-ONLY DIRECT-LIBURING MECHANISM BENCH`.)

---

## 2. Terminology hazard (must not be conflated)

Two distinct kernel mechanisms share the word "fixed":

```text
FIXED FILE   IOSQE_FIXED_FILE + IORING_OP_READ/WRITE + sqe->fd = SLOT INDEX
             kernel: io_file_get_fixed() -> fixed FILE table -> struct file *
             (this is G1-Control's object)

FIXED BUFFER IORING_OP_READ_FIXED/WRITE_FIXED + buf_index = REGISTERED BUFFER
             kernel: io_import_reg_buf() -> registered BUFFER iovec
             (RBUF-E0's object; NOT G1-Control's object)
```

Evidence: `io_uring/opdef.c` (v7.1.9): `[IORING_OP_READ_FIXED]` has
`.prep = io_prep_read_fixed, .issue = io_read_fixed`; `io_read_fixed`
(`io_uring/rw.c:1230`) = `io_init_rw_fixed` + `io_read`, and
`io_init_rw_fixed` (`rw.c:373`) calls `io_import_reg_buf()` — a registered
BUFFER import, not a file lookup. `[IORING_OP_READ]` has `.issue = io_read`
and the fixed-FILE path is selected purely by the SQE flag. liburing 2.14
`io_uring_prep_read_fixed` (`liburing.h:661-668`) sets opcode
`IORING_OP_READ_FIXED` + `buf_index` and never sets `IOSQE_FIXED_FILE`.

RBUF-E0's negative result (registered BUFFER steady-state NOT MATERIAL,
#272/PR #273) therefore says nothing about fixed FILE lookup. They are
separate resource tables (`ctx->file_table` vs `ctx->buf_table`), separate
lookups, separate lifetime accounting.

---

## 3. M-A — ordinary fd resource path (v7.1.9)

Path: SQE with `IORING_OP_READ/WRITE`, no `IOSQE_FIXED_FILE`, `sqe->fd = fd`.

- `io_assign_file()` (`io_uring/io_uring.c:1365`) → `io_file_get_normal()`
  (`io_uring.c:1594`):
  ```c
  struct file *io_file_get_normal(struct io_kiocb *req, int fd)
  {
      struct file *file = fget(fd);
      ...
      return file;
  }
  ```
- `fget(fd)` (`fs/file.c:1111`) → `__fget(fd, FMODE_PATH)` →
  `__fget_files(current->files, fd, ...)` → `__fget_files_rcu()`
  (`fs/file.c:1018`):
  1. `rcu_read_lock()`;
  2. RCU-load `files->fdt` (the process fdtable);
  3. `array_index_mask_nospec(fd, fdt->max_fds)` — bounds check against the
     table size;
  4. load `fdt->fd[fd]`;
  5. `file_ref_get(&file->f_ref)` — atomic refcount increment (full barrier;
     retry loop when racing close);
  6. re-load the table entry and verify it did not change under us;
  7. `rcu_read_unlock()`.

Process-shape fact: `current->files` is the shared `files_struct` for every
thread of the process; `fget()` takes **no files lock** in the lookup path
and its RCU + atomic-refcount shape is identical whether the process has one
thread or many. The "threaded process makes ordinary-fd file reference
management more expensive" claim has **no direct mechanism in the steady-state
fget path** on this source; the plausible indirect costs (fdtable growth,
open/close `files_lock` contention) do not appear in a fixed-fd steady-state
loop. The threaded arm is therefore exploratory, with the default expectation
of no material mechanism delta.

Request lifetime: the request holds `req->file` (a normal `struct file *`
ref from `fget`) and releases it at request cleanup (`io_uring.c:372` area).

---

## 4. M-B — fixed file resource path (v7.1.9)

Path: SQE with `IORING_OP_READ/WRITE` + `sqe->flags |= IOSQE_FIXED_FILE`,
`sqe->fd = slot_index` (userspace; see §5 for liburing helpers).

- `io_init_req()` (`io_uring.c:1734`) at SQE consumption copies
  `req->flags = (__force io_req_flags_t) sqe->flags;` (so `REQ_F_FIXED_FILE`
  mirrors `IOSQE_FIXED_FILE`) and records `req->cqe.fd = READ_ONCE(sqe->fd)`
  (the slot). **No file is bound here.**
- File binding is **lazy, at issue time**: `io_issue_sqe()` (`io_uring.c:1416`)
  → `io_assign_file()` (`io_uring.c:1365`):
  ```c
  if (req->flags & REQ_F_FIXED_FILE)
      req->file = io_file_get_fixed(req, req->cqe.fd, issue_flags);
  else
      req->file = io_file_get_normal(req, req->cqe.fd);
  ```
  A second `io_assign_file()` call site (`io_uring.c:1499`) covers requests
  deferred to io-wq (async offload): those bind in the worker thread.
- `io_file_get_fixed()` (`io_uring.c:1575`):
  ```c
  io_ring_submit_lock(ctx, issue_flags);
  node = io_rsrc_node_lookup(&ctx->file_table.data, fd);   /* DIRECT array index */
  if (node) {
      node->refs++;
      req->file_node = node;        /* request-side retention */
      req->flags |= io_slot_flags(node);
      file = io_slot_file(node);
  }
  io_ring_submit_unlock(ctx, issue_flags);
  return file;
  ```
  - `io_rsrc_node_lookup()` (`io_uring/rsrc.h`) is a plain array index:
    `data->nodes[array_index_nospec(index, data->nr)]` — O(1), no fdtable,
    no RCU walk, no bounds-vs-fdtable.
  - The fixed table is `ctx->file_table` = `struct io_rsrc_data { nodes[] }`
    of `struct io_rsrc_node` (`rsrc.h`: `type`, `refs`, `tag`,
    `file_ptr`/`buf`).
  - Binding takes the per-ring submit lock (`uring_lock`; in inline
    submission the lock is already held by the submitter — the exact
    `io_ring_submit_lock` definition was not present in the fetched source
    snapshot, only its call sites; version-bound detail, non-load-bearing:
    the fixed lookup adds at most a lock-assert/already-held mutex, never a
    second lock acquisition in the common inline path).
- Request-side retention: the request holds `req->file_node` with
  `node->refs++`. At request cleanup `io_req_put_rsrc_nodes()`
  (`io_uring.c:1089`) calls `io_put_rsrc_node(ctx, req->file_node)`; when
  `refs` reaches zero `io_free_rsrc_node()` (`rsrc.c:495`) does
  `fput(io_slot_file(node))` (and, if `node->tag`, posts the retirement aux
  CQE — §8).

Data path: `io_read()`/`io_write()` operate on `req->file` identically for
fixed and ordinary (rw.c uses `req->file->f_op->read_iter/write_iter`). The
fixed-FILE mechanism delta is confined to the lookup/binding step. Fixed
BUFFERS are orthogonal (§2).

---

## 5. M-B — userspace (liburing 2.14) side

- Fixed-file I/O: there is **no dedicated liburing prep helper** for
  fixed-file reads. The canonical pattern is
  `io_uring_prep_read(sqe, slot, buf, len, off)` followed by
  `sqe->flags |= IOSQE_FIXED_FILE;` (the fd field then carries the slot).
  (Do NOT use `io_uring_prep_read_fixed` — that is the fixed-BUFFER op.)
- Registration:
  - `io_uring_register_files(ring, files, nr)` → `IORING_REGISTER_FILES`
    (uapi = 2);
  - `io_uring_register_files_update(ring, off, files, nr)` →
    `IORING_REGISTER_FILES_UPDATE` (uapi = 6) — the runtime replacement;
  - `io_uring_register_files_tags` / `_update_tag` →
    `IORING_REGISTER_FILES2/UPDATE2` (uapi = 13/14) — adds per-slot tags;
  - `io_uring_register_files_sparse(ring, nr)` — empty table of `nr` slots;
  - `io_uring_unregister_files(ring)`.
- UAPI facts (host `/usr/include/linux/io_uring.h`): `IORING_FILE_INDEX_ALLOC
  (~0U)` auto-allocation for direct-descriptor-creating opcodes;
  `IORING_REGISTER_FILES_SKIP (-2)` marks a no-change slot in updates;
  `IORING_FEAT_RSRC_TAGS` advertises tag support.

---

## 6. M-C — update semantics: slot S: A → B

`io_uring_register_files_update` →
`io_register_rsrc_update` → `__io_register_rsrc_update` →
`__io_sqe_files_update()` (`rsrc.c`), per slot:

```c
if (io_reset_rsrc_node(ctx, &ctx->file_table.data, i))   /* drop table's node ref */
    io_file_bitmap_clear(&ctx->file_table, i);
if (fd != -1) {
    file = fget(fd);                       /* NEW file ref (fget, §3) */
    node = io_rsrc_node_alloc(ctx, IORING_RSRC_FILE);   /* new node, refs=1 */
    ctx->file_table.data.nodes[i] = node;  /* table entry now points at B's node */
    io_fixed_file_set(node, file);
}
```

`io_reset_rsrc_node()` (`rsrc.h`) drops **only the table's** reference
(`io_put_rsrc_node`). The old node and the old `struct file *` (A) survive as
long as any in-flight request holds `req->file_node` (bound with `refs++`,
§4). When the last such request completes, the node refcount reaches zero and
`io_free_rsrc_node()` `fput`s A.

### Definitively answered (this kernel, by source)

```text
table entry meaning    : changes atomically at update (nodes[i] swap)
already-bound request  : keeps operating on A; A stays alive via node->refs
                         until that request's cleanup; update cannot yank it
new (unbound) request  : resolves the slot at issue time -> sees B
```

This is exactly the "table entry meaning vs already-bound request physical
resource" separation Issue #279 asks for, and it is **Linux-owned**.

### Binding boundary (M-D) — where "which file" is decided

```text
userspace prepares SQE (slot S, IOSQE_FIXED_FILE)   [fully userspace]
        |
io_uring_submit() / io_uring_enter()
        |
kernel io_get_sqe() copies SQE from SQ ring
        |
io_init_req(): snapshots opcode, flags (REQ_F_FIXED_FILE), cqe.fd = slot S
        |  <-- NO FILE BOUND YET -->
issue (inline, or io-wq worker): io_assign_file -> io_file_get_fixed(S)
        |  <-- BINDING LINEARIZATION POINT: reads the CURRENT table[S]
request owns/binds physical resource (node->refs++, req->file_node)
```

Consequences for a slot update S: A→B issued at each boundary:

```text
A. SQE prepared in userspace, not submitted:
   update before submit -> kernel later binds B.  (fully deterministic in
   userspace: prepare -> update -> submit)
B. submitted, kernel has not yet consumed:
   depends on when io_init_req runs relative to the update; both outcomes
   possible; NOT deterministically observable from userspace alone
C. kernel has consumed but not yet issued:
   io_init_req already snapshotted slot S, but binding is lazy -> a
   concurrent update can still change the bound file (request binds B)
D. kernel issued / bound:
   request bound A, holds node ref; update cannot change it (binds A)
```

Critical mechanism fact for C0-MINIMALITY: **the window is not just
userspace-prep→submit; the kernel itself resolves the slot at issue time,
after SQE consumption.** So a request that was already submitted (even
consumed) can bind the NEW file if the slot is updated before it issues.
L0 closes this by construction (no updates during RUN). L1 closes it if
replacement is gated on "no outstanding accepted request references the
slot" (the request that would bind must complete before the replace is
legal) — expressible with existing request-capacity/drain/reap authority,
without per-resource counters.

---

## 7. M-D / §22-23 — per-request live-use admission pre-check

The window in §6 exists and is kernel-internal. Whether it is a Sluice
correctness obligation depends entirely on the API contract:

- If Sluice's replacement operation requires **quiescence** — mechanically
  interpreted as "no accepted request that references the slot is
  outstanding" (drain all accepted requests, then replace) — then no
  accepted request can straddle a replacement, and the validation→binding
  window is closed by the lifecycle discipline. No per-request live-use
  counter needed.
- If Sluice allowed replacement **while** requests referencing the slot are
  in flight, then a request validated against A could bind B — but that is a
  design choice the explicit-I/O contract can forbid (replacement
  invalidates all outstanding uses of the slot), not a kernel obligation.
- Generation (L2) is needed only if the API allows a stale logical handle
  to survive replacement. If replacement invalidates all old handles, the
  slot integer is just reused with new meaning under the same handle-name —
  no incarnation needed.

The empirical witness to confirm the Linux side of this (FILE-ID-E0 fixed L0
arm) is preregistered separately (§9 of preregistration).

---

## 8. Resource tags are NOT generation (§26)

`IORING_REGISTER_FILES2` / `IORING_REGISTER_FILES_UPDATE2` attach a `tag`
(`u64`) to an `io_rsrc_node` (`rsrc.h`: `u64 tag`). The tag is:

- attached to a table node at register/update time (`rsrc.c:271,320`);
- emitted **once, at resource retirement**: `io_free_rsrc_node()`
  (`rsrc.c:501-502`) posts an aux CQE `io_post_aux_cqe(ctx, node->tag, 0, 0)`
  when the node's refcount reaches zero (i.e. after the last bound request
  and the table have both released it);
- **purely a notification**: nothing in the kernel reads a tag to validate,
  reject, or route a submit; it never rejects a stale submit and is not an
  incarnation check.

```text
tag attached to:   io_rsrc_node (table entry + its request references)
when emitted:      on node retirement (refs == 0)
what it identifies: the retired resource generation instance (informational)
rejects stale submit?: NO
merely reports retirement?: YES
```

`IORING_FEAT_RSRC_TAGS` (advertised) only means the kernel supports the
register-time tag plumbing. Sluice generation must NOT be modeled on it.

---

## 9. Candidate authority split adjudication (pre-registration position)

Issue #279's normative split:

> **Sluice owns logical binding meaning. Linux owns physical kernel-resource
> lifetime.**

Mechanism verdict from source evidence (to be completed by FILE-ID-E0):

```text
Linux owns physical kernel-resource lifetime of BOUND requests:   SUPPORTED
  (node->refs request retention, io_req_put_rsrc_nodes, fput at refs==0;
   update path cannot yank an already-bound resource)

Sluice owns logical binding meaning (slot ownership, affinity, replacement
legality): SUPPORTED as a logical contract — the kernel exposes only an
integer slot; replacement legality and stale-handle semantics are not
kernel-enforced (no generation check, no quiescence gate).

Sluice needs NEW machinery beyond L0/L1 discipline to own that meaning:
NOT SUPPORTED by mechanism (L0/L1 discipline is sufficient; pending the
minimality falsification and FILE-ID-E0)
```

---

## 10. C0 experimental topology decision

Because the production backend has no fixed-file capability and no research
seam could select fixed-file opcodes without production changes, C0 uses the
sanctioned RBUF-E0-style fallback:

```text
RESEARCH-ONLY DIRECT-LIBURING MECHANISM BENCH: bench/g1_control_c0_bench.cpp
```

Arms (prereg §4): F0 ordinary fd / F1 fixed file, each with a matched
threaded-process twin (F0-T / F1-T), over one shared engine; plus a
deterministic FILE-ID-E0 identity-witness mode (ordinary arm + fixed L0 arm)
and a behavioral capability probe. Production code untouched.

---

## 11. Open/version-bound items

```text
io_ring_submit_lock exact definition: not in the fetched source snapshot
  (only call sites); semantics inferred from call sites + 6.x lineage
  (already-held in inline submit; mutex in unlocked/io-wq paths).
  NON-LOAD-BEARING for C0 conclusions.
kernel v7.1.9 io_uring source: fetched from Bootlin v7.1.9 tree; local UAPI
  header cross-checked; liburing 2.14 headers local (xrepo + GitHub tag).
```
