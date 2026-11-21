# AUD-006 comparator truth table (hand-authored)

Every expected value below was written from the documented harness semantics
before observing the comparator's answer. The suite enforces the whole table
at startup (`comparison_oracle_selfcheck`, tests/semantic_oracle_harness.hpp);
a BASE run exiting 0 means every observed value equaled its expected value.

`agree()` cases (scenario expectation vs observation):

| case | expected_verdict | expected_rejection_code | observed | expected agree() | observed at BASE |
|---|---|---|---|---|---|
| A_success_vs_success | execute | nullopt | accepted | true | true |
| B_success_vs_rejection | execute | nullopt | rejected(invalid_argument) | false | false |
| B_complete_empty_vs_rejection | complete_empty | nullopt | rejected(invalid_state) | false | false |
| C_rejection_vs_success | reject_range | invalid_argument | accepted | false | false |
| D_rejection_wrong_code | reject_range | invalid_argument | rejected(invalid_state) | false | false |
| D_rejection_exact_code | reject_range | invalid_argument | rejected(invalid_argument) | true | true |
| E_complete_empty_vs_dispatched | complete_empty | nullopt | accepted(no_op_without_dispatch=false) | false | false |
| E_complete_empty_vs_noop | complete_empty | nullopt | accepted(no_op_without_dispatch=true) | true | true |
| E_complete_empty_unobservable | complete_empty | nullopt | accepted(no_op unobservable) | true | true |
| noop_observation_ignored_for_execute | execute | nullopt | accepted(no_op_without_dispatch=false) | true | true |

`run_path()` synthetic controls (two hand-authored scenarios: `selfcheck_expected_execute`
expects execute; `selfcheck_expected_rejection` expects reject_closed/invalid_state):

| control | adapter behavior | expected | observed at BASE |
|---|---|---|---|
| honest path | answers per input: closed -> rejected(invalid_state), else accepted | 0 mismatches, 0 names | 0 mismatches, 0 names |
| known wrong observation | always reports accepted (wrong exactly for the rejection scenario) | exactly 1 mismatch, exactly the name `selfcheck_expected_rejection` | 1 mismatch, name `selfcheck_expected_rejection` |

`matches_recorded_divergences()` cases (observed vs recorded name sets):

| case | observed | recorded | expected | observed at BASE | observed pre-correction (pointer ==) |
|---|---|---|---|---|---|
| F_both_empty | {} | {} | true | true | true (size-only path) |
| F_identical | {a, b} | {a, b} same pointers | true | true | true |
| same_text_distinct_address | `zero_length_legal_offset` (char array 1) | `zero_length_legal_offset` (char array 2) | true | true | **false — the defect** |
| H_reorder | {b, a} | {a, b} | true | true | true (same pointers) |
| G_extra_observed | {a} | {} | false | false | false (size-only path) |
| G_missing_observed | {} | {a} | false | false | false (size-only path) |
| G_different_name | {a} | {c} | false | false | false |
| I_duplicate_observed | {a, a} | {a} | false | false | false |
| I_duplicate_recorded | {a} | {a, a} | false | false | false |
| I_partial_overlap | {a, b} | {a} | false | false | false |

The pre-correction column is recorded from the run in
`before-correction-uncorrected-comparator.txt`: the self-check, added before
the comparator was corrected, failed exactly on `same_text_distinct_address`
with both sides printing the identical name text — the pointer-equality
defect at e16960ff. All other pre-correction values coincide with the
corrected comparator because every in-tree acceptance call site passes an
empty recorded set, so the element-wise pointer comparison was unreachable
there (decided by the vector size check).

## Representation investigation conclusion

Divergence identity is the name text at every call site: observed names are
`kPrecedenceScenarios[i].name` pointers pushed by `run_path`
(tests/semantic_oracle_harness.hpp), while a recorded set is spelled at the
call site or in `request_zero_length_divergence_set()`
(tests/semantic_scenarios.hpp) — distinct storage that shares addresses with
the table only when the compiler pools identical string literals. Pointer
equality therefore cannot define divergence identity; the smallest correction
keeps the strcmp sort and compares elements by content
(`harness-matches-after.txt`).
