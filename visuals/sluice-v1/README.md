# Sluice v1 · interactive architecture walkthrough

This directory holds the interactive, single-file walkthrough of the Sluice
v1 target contract (`index.html`). It is a **DERIVED / NON-NORMATIVE**
visualization: a teaching view of responsibilities, states and ownership
transitions. It is not a specification and never becomes a peer authority of
the root.

- Normative source: [`docs/explicit-io-v1-final-decision.md`](../../docs/explicit-io-v1-final-decision.md)
  (sole normative root)
- Root revision: v1-r3
- Source commit: `jnhu76/Sluice@291c8ec54c33a09306720cda1e6ee217edda9030`
- If this visualization conflicts with the root specification, the root
  specification wins.

The page is self-contained and offline: no external runtime dependencies,
system font stacks, inline SVG icons, data-URI favicon. Node positions are
draggable and persist per-browser in localStorage; this never changes the
depicted semantics.

Review evidence for the visualization slice: [`review/ui-review.md`](../../review/ui-review.md).

## Publication

`.github/workflows/pages.yml` stages `index.html` as the site root and
deploys via GitHub Actions, expected at
`https://jnhu76.github.io/Sluice/`. The repository setting
`Settings → Pages → Build and deployment → Source: GitHub Actions` must be
enabled for the deploy job to run.
