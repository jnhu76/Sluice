# aligne0-offset-wsl2-1 — PAGE-OFFSET-E0 diagnostic (QUALIFIED_BUT_VIRTUALIZED)

Purpose: separate address alignment from page-relative phase. Triggered
because a0 (16-aligned, page offset 16) vs a1 (64-aligned, page offset 0)
changes BOTH properties. Exposed = page-aligned base + offset, offsets
{0,16,32,64,128,256,512,1024,2048}, sizes {4K,64K,1M}, dirs both, d1.

- 108 runs, 0 gate errors. minflt_io = 0 for all cells (no fault artifact).
- READ: ONLY offset +16 is slow (2.4x-4.3x); offsets 0, 32, 64, 128, ...,
  2048 all fast, at every size. So the slow configuration is address
  congruent to 16 mod 32 (16-byte aligned but NOT 32-byte aligned); 32-byte
  alignment at any page offset is already fast. The threshold is between 16
  and 32 bytes on WSL2 READ; exact boundary (e.g. 24 or the 16-mod-64
  classes 48/80/112...) untested -> UNRESOLVED at finer granularity.
- WRITE: flat across all offsets.
- mechanism: UNRESOLVED (kernel/uaccess branch attribution requires
  source/profile + native replication; PMU unreliable on WSL2). The +16
  signature may be a WSL2-virtualization-specific artifact; native
  replication is REQUIRED before any interpretation beyond WSL2.
