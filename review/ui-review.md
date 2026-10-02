# Sluice v1 walkthrough · UI review record

Scope: `visuals/sluice-v1/index.html` — the derived, non-normative
visualization of the v1 target contract. This record is evidence for the
visualization slice only; it changes no normative document.

Method: an independent reviewer agent hunted candidate defects in real
Chromium — viewports 1366x768 / 1440x900 / 1920x1080 / 2560x1440, text
presets standard / comfortable / large, all six flows at first/middle/last
step, both backend modes, drag / autoplay / keyboard / theme / localStorage /
fullscreen interactions, console and network logs. The main agent then
independently reproduced every accepted finding before changing code, and a
separate visual acceptance pass re-judged the final rendered states.
Screenshot evidence is local and untracked under `.playwright-mcp/`
(`p1-*.png` before the fullscreen fix, `review-*.png` reviewer captures,
`fix-*.png` post-fix acceptance captures).

## Findings and adjudication

| ID | Severity | Area | Adjudication | Resolution |
|----|----------|------|--------------|------------|
| F-01 | major | fullscreen / a11y | CONFIRMED — in fullscreen, Tab reached ~7 invisible controls beneath the overlay (provenance summary, modepick, fontpick, theme, reset, flow tabs); Enter on invisible flow tabs switched flows with no visible feedback | visibility:hidden on header/`.flowtabs`/`.scenario-note` and on `.controls > *:not(.fullscreen-toggle)` while `body.diagram-fullscreen`; re-sweep shows zero covered or body focus stops |
| F-02 | major | fullscreen / font scaling | CONFIRMED — 1366x768 + large preset fullscreen: host×request node overlap 33.4px, bottom row overflowing stage 18px, 3 nodes covering the annotation strip | `fitStageHeight()` derives the stage height from measured node heights (row-gap constraints) on every wire redraw; fullscreen large now grows 422→536px, zero overlap/overflow at all presets |
| F-03 | minor | layout / responsive | CONFIRMED — 1366x768 normal: bottom row overran the stage by 3.9/21/18.8px (standard/comfortable/large); adjudication additionally found a linux×backend node overlap of 13.6px at comfortable and 10.9px at large that the reviewer had missed | same `fitStageHeight()` mechanism (covers row-gap and bottom constraints); post-fix overflow ≤ -3.3px margin, no overlaps, no annotation coverage |
| F-04 | minor | fullscreen / overlap | CONFIRMED — fixed exit button overlapped the `#stepIdx` counter (5x16px at 1366, 20x14px at 2560) | `.canvas.is-fullscreen .canvas-head{padding-right:44px}`; post-fix clearance 39px |
| F-05 | minor | state / responsive | CONFIRMED — layout saved at 2560x1440 after a drag left the node 14.5px outside the stage at 1366x768 | `loadLayout()` re-clamps saved percentages against current stage/node geometry; post-fix overflow -2.6px (inside stage) |
| F-06 | minor | accessibility | CONFIRMED — `#btnTheme` was the only toggle without `aria-pressed` | `aria-pressed` added, updated in `toggleTheme()` and at boot restore; verified false→true→false |

No candidate was REJECTED or DUPLICATE; all six reproduced.

## User-requested changes (same slice)

- The active-handoff packet previously ran once and froze at the target node;
  it now loops continuously along the active wire (SMIL `repeatCount`
  indefinite, restarted per step, non-active packets stopped via
  `endElement()`). Verified: continuous motion past one period, exactly one
  visible packet at a time, clean handover on step/flow/mode switches,
  `prefers-reduced-motion` still disables it entirely.
- Pace: packet loop 1.5s→2s per pass after user feedback (3s rejected as too
  slow), autoplay step interval 3.2s→4s.

## Post-fix verification

- 1366x768, all three presets, normal and fullscreen: zero node overlaps,
  zero stage overflows, zero annotation coverage (margins ≥ 3.3px).
- Fullscreen at 1440x900 / 1920x1080 / 2560x1440: graph visible, opaque
  background, exit button visible and clear of the step counter, Escape and F
  correct, exit restores prior layout, wire SVG endpoints re-derived after
  enter/exit.
- Wire anchoring sweep: 39/39 handoff steps across all six flows anchored
  within 10px of their node edges (max 10px); exactly one active wire and one
  looping packet per handoff step; zero console errors/warnings/pageerrors
  across the entire matrix.

## Remaining nonblocking debt

- Fullscreen content intentionally may exceed one viewport at short viewports
  with the larger presets and scrolls inside the overlay; readability is
  preferred over fitting everything on one screen.
- Nodes dragged into the same band as another row can still overlap; the drag
  shows the user's explicit layout, and reset layout restores the default.

Result: **UI_REVIEW_PASS** — no confirmed blocking defect remains.
