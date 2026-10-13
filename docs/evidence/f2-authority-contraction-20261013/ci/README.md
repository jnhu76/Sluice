# GitHub CI — F2 branch

`runs.json` lists the CI runs on branch `f2-clean-authority`; `head-run.json` is
the run for the implementation head recorded in `gates.json`, with its jobs.
`collect-gates.py` asserts that a run exists for that exact head SHA, so a run
recorded for any other revision cannot stand in.

The workflow is `.github/workflows/ci.yml`: a C++ comment-authority guard, the
F1 frozen-provenance verifier self-tests, and a Linux build-plus-`xmake test`
matrix over `debug` and `release`. It runs gcc on ubuntu-latest, so it does not
cover the Clang sanitizer profiles — those are captured locally in `matrix/`.
