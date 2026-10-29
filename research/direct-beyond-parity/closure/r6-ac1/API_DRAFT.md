# R6/AC-1 API/ABI Draft + Support Matrix + Fallback Design

**Status: DRAFT FOR OWNER DECISION — NOT AN ADOPTED API.** Nothing in this
file exists in the product tree; no header, source file or build target is
added by this PR. Adoption, naming, and the open decisions in
`OWNER_DECISION_PACKET.md` belong to the Owner (MISSION §1; #605).

## 1. Draft shape (working names)

```cpp
// Candidate sketch only — mirrors the existing direct blocking family
// (include/sluice/blocking/file.hpp), same Result/IOError vocabulary.
namespace sluice::blocking {

// One contiguous file extent [offset, offset + total) where
// total = Σ buffers[i].size(), filled from / gathered into the k caller
// buffers in array order. Primitive-class result: one confirmed count.
Result<std::size_t> read_at_vectored(const File& file, std::uint64_t offset,
                                     std::span<const std::span<std::byte>> buffers);

Result<std::size_t> write_at_vectored(const File& file, std::uint64_t offset,
                                      std::span<const std::span<const std::byte>> buffers);

}
```

Design notes:

- **Result shape stays primitive-class** (one count). Per-buffer prefix
  fill is derivable in array order from the count (measured: S2,
  `SEMANTICS.md`). A per-iov result vector would turn this into AC-2's
  per-member discipline — deliberately not proposed.
- Buffers are borrowed for the synchronous call, exactly like today's
  `std::span` arguments; no lifetime, ownership or resource change.
- Empty `buffers` (or all-zero lengths) → success 0, no OS call, range
  checks skipped (SEM-03 step 3). The kernel's own zero-total behavior
  (success 0, `r6ac1-null-empty.txt`) is never reached.
- The span-of-spans parameter has no stable-ABI consequence beyond one new
  symbol per operation in the `sluice::blocking` namespace; no existing
  symbol, header layout or `CompositionOutcome` ABI is touched. The
  installed-SDK surface grows only if the Owner adopts it.

## 2. Draft admission algorithm (reuses existing machinery)

```
precheck (extends precheck_data_op, include/sluice/detail/file_semantics.hpp:161-178):
  1. closed                              -> invalid_state
  2. access illegality                   -> invalid_argument      (SEM-03 table)
  3. total = Σ len_i  (size_t, checked)  ; total == 0 -> success 0, no OS call
  4. range_is_valid(offset, total) fails -> invalid_argument      (SEM-04, no overflow)
  5. buffers.size() > declared IOV_MAX   -> not_supported         (SEM-04 stricter
     (or split — OQ-M1-3)                                     limit, before acceptance)
  6. any individual len_i > kMaxNativeTransfer is covered by step 4's
     total check on 64-bit size_t; per-entry NULL bases are caller
     preconditions (not dynamically detected — SEM-03)

execute:
  native_offset = checked_posix_offset(offset)      // existing helper
  n = retry_on_eintr([&]{ ::preadv(fd, iov, k, native_offset); })   // or pwritev
  n < 0 -> from_errno_value(errno)                  // existing mapping, unchanged
  else  -> success(n)
```

Every step cites code that already exists in the tree; the only new
logic is the Σ accumulation with overflow check and the declared-limit
branch. Per the Mechanism Justification Gate (AGENTS.md): the iov-count
limit represents a declared execution limit owned by the platform
(`sysconf(_SC_IOV_MAX)`, measured 1024 — `r6ac1-env.txt`); it classifies
as a compatibility/physical-obligation check, not speculative defense —
the kernel rejects k=1025 with EINVAL (`r6ac1-iovmax.txt`), and the
library must answer before acceptance per SEM-04. It replaces `None`.

## 3. Support matrix

| Platform / profile | Availability | Candidate behavior | Source |
|---|---|---|---|
| Linux kernel ≥ 2.6.30 | `preadv`/`pwritev` native | direct use | `[TRANSCRIBED]` man 2 readv VERSIONS; host kernel 7.2.11 `[RUN]` |
| glibc ≥ 2.20 wrappers | direct syscalls, no fallback copy path | direct use | `[TRANSCRIBED]` man 2 readv "Historical C library/kernel differences"; host glibc 2.43; wrapper≡syscall parity measured `r6ac1-array-order.txt`, `r6ac1-iov-fault-read.txt` `[RUN]` |
| `IOV_MAX` discovery | `sysconf(_SC_IOV_MAX)` / `getconf IOV_MAX` | declared limit = sysconf value, no hardcoded 1024 | `r6ac1-env.txt` `[RUN]` |
| BSD family | `preadv`/`pwritev` available | out of E5 scope; same header surface | `[TRANSCRIBED]` man 2 readv (BSD listed) |
| POSIX | `readv`/`writev` are POSIX; **`preadv`/`pwritev` are not POSIX** | a non-Linux/non-BSD profile reports `not_supported` | `[TRANSCRIBED]` man 2 readv; M1_ANALYSIS §4 |
| WSL2 | unverified this round (mission forbids WSL2 runs) | WSL2 follow-up item | NOT_RUN_WITH_REASON, mission §1/§8.3 |
| Filesystem matrix (Linux) | tmpfs + btrfs measured this session | fault rows E10/E11 carry an explicit fs-dependence disclosure | `raw/r6ac1-*` `[RUN]` |

## 4. Fallback design (what is legal, what is forbidden)

1. **`lseek` + `readv`/`writev` lowering — forbidden.** SEM-04: "Hidden
   seek-plus-read/write is not a conforming lowering." Measured ground:
   the cursor is shared through `dup` (`r6ac1-cursor.txt`), so a hidden
   seek is caller-observable. This row is closed; there is no disclosure
   variant.
2. **k × `read_at`/`write_at` lowering — semantically legal, not a silent
   fallback.** k admissions and k syscalls change the observable
   syscall/granularity story and lose nothing semantically (that
   composition is today's only legal form, measured wall-parity with the
   raw loop — P3 M1 `avs−av0` +5.7 ns read / −15.4 ns write, sub-MDE
   `NO_CLAIM` `[TRANSCRIBED: STRUCTURAL_IO_MECHANISM_STUDY.md §1.3]`).
   If the Owner ever wants a portable profile where the vectored call
   exists but lowers, it must be a **declared** profile
   (documented per-platform), never a runtime hidden path; the v1 draft
   proposes `not_supported` on platforms without the native primitive
   instead, so no silent behavioral divergence can appear.
3. **Split-below-`IOV_MAX` execution** (`buffers.size() > IOV_MAX` →
   ceil(k/1024) native calls behind one public result) — semantically
   preservable (prefix fill is compositional; short counts and errors
   accumulate with confirmed-bytes discipline) but it is
   **composition-class execution wearing a primitive signature**: EINTR
   handling, no-progress writes, and unknown-remainder effects after a
   failed split must follow SEM-05 composition rules, and the contract
   must say so explicitly. Cost/semantics comparison in
   `OWNER_DECISION_PACKET.md` §3 — Owner decision, not chosen here.
4. **No fallback may promise semantics the platform does not deliver.**
   The whole-extent atomicity question is closed by measurement: no form
   (single `preadv` or k calls) provides a cross-page snapshot on this
   host (`SEMANTICS.md` §3) — no fallback may claim one either.

## 5. Explicitly out of scope (unchanged from #605)

- Cursor-relative `readv`/`writev` forms (OQ-M1-4) — no sketch, separate
  decision.
- `preadv2`/`pwritev2` flag surface (OQ-M1-5) — no sketch.
- Scattered-offset gathering (AC-2), io_uring execution (D-P3-01),
  `O_DIRECT` shapes (SNE-4 / D-P3-02).
