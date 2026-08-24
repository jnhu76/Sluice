#!/usr/bin/env python3
"""Mutant generator for the #197 Completion publication weak-memory kernels.

Reads a kernel source, applies ONE named order-weakening mutation, and writes
the mutated source to stdout. Fail-closed by construction:

  * every mutation is defined by an exact pattern that MUST occur exactly once
    in the given kernel — drift in the kernel source (renamed variable, changed
    memory order, reformatted call) makes this generator exit non-zero instead
    of silently producing a wrong mutant;
  * mutants are generated at run time and never committed, so no stale
    negative can survive a kernel edit.

Usage:
  _gen_mutants.py <kernel-file> <mutation-name>
  _gen_mutants.py --list
"""

import sys
from pathlib import Path

# One mutation = (kernel file basename, exact source pattern, replacement,
# human description of the weakened production site).
# The pattern text intentionally includes the full call shape so a match pins
# the exact production access being weakened.
MUTATIONS = {
    # --- controls: must FAIL the checker under rc11/ra (runner enforces) ---
    "N1_pub_ready_store_relaxed": (
        "kernel_publication.cpp",
        "st.store(St::ready, std::memory_order_release);",
        "st.store(St::ready, std::memory_order_relaxed);",
        "A7 completion.hpp:414 — final ready store release -> relaxed "
        "(producer side of the publication edge)",
    ),
    "N1b_observer_load_relaxed": (
        "kernel_publication.cpp",
        "St s = st.load(std::memory_order_acquire);",
        "St s = st.load(std::memory_order_relaxed);",
        "B1 completion.hpp:200/217-222 — observer acquire load -> relaxed "
        "(consumer side of the publication edge)",
    ),
    "N3_commit_cas_relaxed": (
        "kernel_publication.cpp",
        "e, St::outstanding, std::memory_order_acq_rel, std::memory_order_acquire);",
        "e, St::outstanding, std::memory_order_relaxed, std::memory_order_acquire);",
        "A3 completion.hpp:328-346 — binding->outstanding commit CAS "
        "acq_rel -> relaxed (I2 payload-visibility edge)",
    ),
    "N2_late_observer_load_relaxed": (
        "kernel_reset_reuse.cpp",
        "if (st.load(std::memory_order_acquire) == St::ready) { ready2 = true; break; }",
        "if (st.load(std::memory_order_relaxed) == St::ready) { ready2 = true; break; }",
        "K2 round-2 observation load (the B1-equivalent on the reuse round) "
        "acquire -> relaxed; stale round-1 value / reset sentinel become "
        "observable",
    ),
    # N4 was pre-registered in the #197 audit comment as a PREDICTED-redundant
    # weakening ("the round-2 claim CASes re-establish ordering"). The checker
    # DISPROVED that prediction: an acquire RMW reading from a RELAXED store
    # establishes no synchronizes-with edge, so the caller's C3 clears race
    # with the round-2 claimant's writes. Upgraded to a control — the pilot's
    # clearest demonstration that the kernel constrains real orderings.
    "N4_reset_idle_store_relaxed": (
        "kernel_reset_reuse.cpp",
        "st.store(St::idle, std::memory_order_release);",
        "st.store(St::idle, std::memory_order_relaxed);",
        "C4 completion.hpp:273 — idle store release -> relaxed; the next "
        "claim's acquire CAS reads from this store, so relaxing it removes "
        "the caller->claimant synchronizes-with edge (pre-registered as "
        "redundant, disproved by the checker, upgraded to a control)",
    ),
    # --- documented redundancy observation: NOT a control; expected to PASS ---
    # A1's acquire reads the INITIAL state write (no cross-thread edge), and
    # its release half is subsumed by the later same-thread A3/A7 releases.
    "RA1_begin_binding_cas_relaxed": (
        "kernel_publication.cpp",
        "e, St::binding, std::memory_order_acq_rel, std::memory_order_acquire);",
        "e, St::binding, std::memory_order_relaxed, std::memory_order_acquire);",
        "A1 completion.hpp:321-327 — idle->binding CAS acq_rel -> relaxed; "
        "REDUNDANT in the kernel (acquire reads the initial write; release "
        "subsumed by later same-thread releases); recorded as a redundancy "
        "observation, not a control",
    ),
}

CONTROL_MUTATIONS = (
    "N1_pub_ready_store_relaxed",
    "N1b_observer_load_relaxed",
    "N3_commit_cas_relaxed",
    "N2_late_observer_load_relaxed",
    "N4_reset_idle_store_relaxed",
)
REDUNDANCY_MUTATIONS = ("RA1_begin_binding_cas_relaxed",)


def gen(kernel_path: Path, name: str) -> int:
    if name not in MUTATIONS:
        print(f"error: unknown mutation '{name}'", file=sys.stderr)
        return 2
    base, pattern, replacement, desc = MUTATIONS[name]
    if kernel_path.name != base:
        print(
            f"error: mutation '{name}' is defined for {base}, got "
            f"{kernel_path.name}",
            file=sys.stderr,
        )
        return 2
    src = kernel_path.read_text()
    n = src.count(pattern)
    if n != 1:
        print(
            f"error: mutation '{name}' pattern does not occur exactly once in "
            f"{kernel_path} (count={n}) — kernel/pattern drift; refusing to "
            f"generate a wrong mutant",
            file=sys.stderr,
        )
        return 3
    out = src.replace(pattern, replacement)
    print(f"// GENERATED MUTANT {name} — do not edit by hand.", file=sys.stdout)
    print(f"// Weakened site: {desc}", file=sys.stdout)
    print(f"// Base kernel: {base} (unmodified in the repository).", file=sys.stdout)
    print(out, end="", file=sys.stdout)
    return 0


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__, file=sys.stderr)
        return 2
    if args[0] == "--list":
        for name in MUTATIONS:
            kind = "control" if name in CONTROL_MUTATIONS else "redundancy"
            print(f"{name}\t{kind}\t{MUTATIONS[name][3]}")
        return 0
    if len(args) != 2:
        print("usage: _gen_mutants.py <kernel-file> <mutation-name>", file=sys.stderr)
        return 2
    return gen(Path(args[0]), args[1])


if __name__ == "__main__":
    sys.exit(main())
