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
| `69686d2f4ab6` | failure | the W-03 stall again at CI scale: `threadpool_external_loop_test` exit -1 after 5.16 s, aborting in the context destructor (see ../../w03-root-cause.md) |
| `b63a532ea6eb` | failure | build-only portability error in the corrective test itself: Ubuntu glibc marks write(2) `warn_unused_result` and the discarded return failed `-Werror`; fixed by `25734314` |
| `25734314f704` | success | corrected implementation head (W-03 closure) |
| `093332ba1b40` | success | final implementation head (corrective round; gates-bound) |
| `a03551feecbc` | success | final pushed head (documentation-only after the implementation head) |
| `a26d7c39b96d` | success | candidate closeout tip (documentation-only) |
| `acbd1a73c34c` | success | owner clean-break decision record (documentation-only) |

On master, run `38403388160` for `27a5041a17ed` — the PR #516 merge commit —
passed all four jobs; the merged tree is byte-identical to `acbd1a73`.

The workflow is `.github/workflows/ci.yml`: a C++ comment-authority guard, the F1
frozen-provenance verifier self-tests, and a Linux build-plus-`xmake test` matrix over
`debug` and `release`. It runs gcc on ubuntu-latest, so it does not cover the Clang
sanitizer profiles — those are captured locally in `../matrix/`.
