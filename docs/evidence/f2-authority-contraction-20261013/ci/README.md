# GitHub CI — F2 branch

`runs.json` lists every CI run on `f2-clean-authority`; `head-run.json` is the run for
the implementation head recorded in `gates.json`, with its jobs. `collect-gates.py`
asserts that a run exists for that exact head SHA, so a run for any other revision
cannot stand in.

| Head | Result | Note |
|---|---|---|
| `4935f2b17053` | success | documentation-only |
| `4e02f227009a` | success | documentation-only |
| `996f434fd151` | success | implementation head (captured) |
| `3f750e3e2f82` | cancelled | superseded by a newer push |
| `98439970627c` | failure | the W-03 stall flake (see ../../blocker-w03-stall.md) |
| `e9e47ce87f5f` | success | documentation-only |

The workflow is `.github/workflows/ci.yml`: a C++ comment-authority guard, the F1
frozen-provenance verifier self-tests, and a Linux build-plus-`xmake test` matrix over
`debug` and `release`. It runs gcc on ubuntu-latest, so it does not cover the Clang
sanitizer profiles — those are captured locally in `../matrix/`.
