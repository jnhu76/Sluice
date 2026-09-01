# aligne0-offset-native-1 — session notes

PAGE-OFFSET-E0 diagnostic on native Linux (frozen offsets
{0,16,32,64,128,256,512,1024,2048}, sizes {4K,64K,1M}, depth 1, READ and
WRITE). 108 runs, 0 gate errors, 54 rows.

## READ

- 4K:    +16 = 1415 ns vs offsets 0/32..2048 = 1234-1334 ns → +16 slow by
  ~7-15% (offset 0 window noisy, mad 366).
- 64K:   +16 = 11674 ns vs 0/32/64/128/256/512/1024/2048 = 9169-10087 ns →
  +16 slow by ~16-27% (mad at +16 4401; neighbors cluster tightly).
- 1M:    +16 = 195.6 us vs others 168.9-186.0 us → +16 slow by ~5-16%,
  mostly inside window noise (mad 17-87 us at 1M).

Signature: the +16 page-offset point is the slow one; offsets 0/32/64/...
(0 or >=32 mod 32) are fast. This is the SAME structural signature as the
WSL2 offset sweep (only +16 slow, all other tested offsets fast), but the
native penalty is roughly one order of magnitude smaller. As on WSL2, the
measured set only proves "+16 slow, all offsets >= 32 B apart from the
slow residue fast" — it does not exhaustively prove the full mod-32
residue class.

## WRITE

Flat across all offsets at all three sizes (0.9-1.1 scatter, no offset
pattern; windows noisy at 64K/1M). No WRITE alignment/page-phase effect
on native.

## PMU

instructions arm-invariant per cell (e.g. 2382-2387 @4K, 30777-30783
@64K, 485101-485105 @1M); cycles:u remains unreliable (see ladder notes).

## Classification (evidence taxonomy)

- "+16 is the tested slow point, offsets 0/32/64/128/256/512/1024/2048
  fast" — DIRECTLY MEASURED on native.
- "32 B minimum TESTED effective" — DIRECTLY MEASURED (32-byte-separated
  offsets fast; sub-32 offsets other than +16 untested).
- Mechanism (kernel uaccess copy path) — UNRESOLVED on native; A/B data
  stands alone.