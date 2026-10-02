# Sluice v1 interactive architecture page

This directory contains a **derived, non-normative** interactive walkthrough of the Sluice v1 target contract.

- `index.html.gz.b64` — gzip-compressed, base64-encoded self-contained page source used by the Pages workflow.
- The deployed artifact is reconstructed as a normal `index.html` before upload to GitHub Pages.

Normative authority remains `docs/explicit-io-v1-final-decision.md`. If the visualization conflicts with that root specification, the root wins.

The current page is pinned to repository commit `291c8ec54c33a09306720cda1e6ee217edda9030` and root revision `v1-r3`.

The page defaults to the **130% readable preset**. Supporting UI text has an approximately **12 px minimum**, with larger 145% and 160% presets available. The fullscreen implementation uses an opaque viewport scrim and a persistent contract/exit control so the underlying page cannot bleed through.

After GitHub Pages is enabled with **GitHub Actions** as the publishing source, the expected URL is:

`https://jnhu76.github.io/Sluice/`

The visualization derives from `konraddzbik/architecture-diagram-skill` v1.3.0 at commit `486ac078705c873012239124a07e7ef2df8fe783`; its MIT notice is retained inside the page.