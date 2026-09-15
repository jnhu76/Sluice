/-
Sluice Stage-0-V2 judgment layer (FCB1-METHOD-CORRECTIVE-1).

Defines, over the Stage-0-V2 calculus (`CalcV2.lean`):

  * trace guarantees (safety) and trace possessions (possibility evidence),
    over the disciplined trace languages (`TracesPrimS` / `TracesEncS`),
  * `Reduction`    -- observational trace equivalence between a primitive
                     and an encoding over the base `O` (the frozen
                     `BASE(P)` as a `BaseOpsSig`),
  * `ReducibleTo`  -- reduction with all declared obligations preserved;
                     `Reducible P O obls` answers `REDUCIBLE(P, BASE(P))`,
  * `IrreducibleTo` -- the per-encoding mismatch criterion: every encoding
                     over the base either under-produces (misses a legal
                     primitive trace) or over-produces (emits a trace the
                     primitive forbids).  Exactly `¬Reducible`, so it is the
                     complete shape of a non-reducibility verdict,
  * `SeparatedBy`  -- a strong sufficient criterion: one trace the
                     primitive produces and no encoding over the base
                     produces.  The universal quantifier ranges over the
                     closed class `Encoding O A`, so it is kernel-checked.

All primitive dispositions of the campaign must be instances of exactly
these judgments, with the stage's frozen `BASE(P)` as `O`.  Nothing here may
be redefined per stage (BRAKE-1 governs changes).
-/

import Sluice.Formal.CalcV2

namespace Sluice.Formal

/-! ## Classical small-scale helpers -/

theorem exists_not_of_not_forall {α : Type} (p : α → Prop) (h : ¬ ∀ x, p x) :
    ∃ x, ¬ p x :=
  Classical.byContradiction fun hn =>
    h (fun x => Classical.byContradiction fun hx => hn ⟨x, hx⟩)

theorem and_not_of_not_imp (a b : Prop) (h : ¬ (a → b)) : a ∧ ¬ b := by
  refine ⟨Classical.byContradiction fun hna => h (fun ha => absurd ha hna), ?_⟩
  intro hb
  exact h (fun _ => hb)

/-! ## Trace properties over the disciplined languages -/

/-- `Inv` is a safety guarantee of the primitive's disciplined trace
language. -/
def Guarantees {A : ApiSig} (P : PrimLTS2 A) (Inv : Trace A → Prop) : Prop :=
  ∀ t, TracesPrimS A P t → Inv t

/-- `Q` is realized by the primitive's disciplined trace language.
Possibility claims use this form so that a reduction cannot be discharged by
a vacuous primitive (an empty trace language trivially guarantees
everything but realizes nothing). -/
def Possesses {A : ApiSig} (P : PrimLTS2 A) (Q : Trace A → Prop) : Prop :=
  ∃ t, TracesPrimS A P t ∧ Q t

/-! ## Reduction against a frozen base -/

/-- One-directional trace refinement (for derived surfaces whose observable
behavior must be contained in their base). -/
def Refines {A : ApiSig} (sub sup : Trace A → Prop) : Prop :=
  ∀ t, sub t → sup t

/-- Observational trace equivalence between a primitive `P` and an encoding
`enc` over the base `O`: every disciplined primitive behavior is produced by
the encoding, and the encoding produces nothing the primitive does not. -/
structure Reduction {A : ApiSig} (P : PrimLTS2 A) {O : BaseOpsSig} (enc : Encoding O A) : Prop where
  /-- Every primitive trace is matched by the encoding. -/
  fwd : ∀ t, TracesPrimS A P t → TracesEncS O A enc t
  /-- The encoding invents no behavior outside the primitive contract. -/
  bwd : ∀ t, TracesEncS O A enc t → TracesPrimS A P t

/-- An obligation is preserved by a reduction when the encoding side both
guarantees and realizes everything the primitive side guarantees and
realizes. -/
def ObligationsPreserved {A : ApiSig} (P : PrimLTS2 A) {O : BaseOpsSig} (enc : Encoding O A)
    (obls : List (Trace A → Prop)) : Prop :=
  ∀ ob ∈ obls,
    (Guarantees P ob → ∀ t, TracesEncS O A enc t → ob t) ∧
    (Possesses P ob → ∃ t, TracesEncS O A enc t ∧ ob t)

/-- Obligation preservation is a corollary of observational equivalence:
guarantees travel along `bwd`, possibilities along `fwd`. -/
theorem obligations_preserved_of_reduction {A : ApiSig} {P : PrimLTS2 A} {O : BaseOpsSig}
    {enc : Encoding O A} (red : Reduction P enc) (obls : List (Trace A → Prop)) :
    ObligationsPreserved P enc obls := by
  intro ob _
  exact ⟨fun hguar t ht => hguar t (red.bwd t ht),
         fun hpos => match hpos with | ⟨t, ht, hob⟩ => ⟨t, red.fwd t ht, hob⟩⟩

/-- `P` is reducible to the base `O` through `enc`: observational
equivalence plus preservation of every declared obligation. -/
def ReducibleTo {A : ApiSig} (P : PrimLTS2 A) (O : BaseOpsSig) (enc : Encoding O A)
    (obls : List (Trace A → Prop)) : Prop :=
  Reduction P enc ∧ ObligationsPreserved P enc obls

/-- `REDUCIBLE(P, BASE(P))`: some encoding over the frozen base `O`
witnesses the reduction with all obligations preserved. -/
def Reducible {A : ApiSig} (P : PrimLTS2 A) (O : BaseOpsSig) (obls : List (Trace A → Prop)) : Prop :=
  ∃ enc, ReducibleTo P O enc obls

/-! ## Separating witnesses -/

/-- A separating witness is a concrete observation trace that the primitive
legally produces and that *no* encoding over the base `O` can produce.
Because encodings over `O` form a closed class, the universal quantifier is
a kernel-checked claim.  This shape is sufficient for non-reducibility but
not necessary. -/
def SeparatedBy {A : ApiSig} (P : PrimLTS2 A) (O : BaseOpsSig) (t : Trace A) : Prop :=
  TracesPrimS A P t ∧ ∀ enc : Encoding O A, ¬ TracesEncS O A enc t

/-- The separating-witness principle: a witness refutes every reduction
claim over the base, hence refutes reducibility outright. -/
theorem not_reducible_of_separated {A : ApiSig} {P : PrimLTS2 A} {O : BaseOpsSig}
    {obls : List (Trace A → Prop)} {t : Trace A} (h : SeparatedBy P O t) :
    ¬ Reducible P O obls := by
  rintro ⟨enc, red, -⟩
  exact h.2 enc (red.fwd t h.1)

/-! ## The complete non-reducibility criterion -/

/-- The encoding over `O` misses a trace the primitive legally produces. -/
def UnderProduces {A : ApiSig} (P : PrimLTS2 A) (O : BaseOpsSig) (enc : Encoding O A) : Prop :=
  ∃ t, TracesPrimS A P t ∧ ¬ TracesEncS O A enc t

/-- The encoding over `O` produces a trace the primitive forbids. -/
def OverProduces {A : ApiSig} (P : PrimLTS2 A) (O : BaseOpsSig) (enc : Encoding O A) : Prop :=
  ∃ t, TracesEncS O A enc t ∧ ¬ TracesPrimS A P t

/-- A per-encoding failure of observational equivalence, in either
direction. -/
def Mismatch {A : ApiSig} (P : PrimLTS2 A) (O : BaseOpsSig) (enc : Encoding O A) : Prop :=
  UnderProduces P O enc ∨ OverProduces P O enc

/-- `P` is irreducible to the base `O`: every encoding over the base
disagrees with `P` on some trace. -/
def IrreducibleTo {A : ApiSig} (P : PrimLTS2 A) (O : BaseOpsSig) : Prop :=
  ∀ enc : Encoding O A, Mismatch P O enc

/-- An irreducible primitive has no reduction over the base, whatever
obligations are declared. -/
theorem not_reducible_of_irreducible {A : ApiSig} {P : PrimLTS2 A} {O : BaseOpsSig}
    {obls : List (Trace A → Prop)} (h : IrreducibleTo P O) : ¬ Reducible P O obls := by
  rintro ⟨enc, red, -⟩
  rcases h enc with hunder | hover
  · obtain ⟨t, hP, hnotE⟩ := hunder
    exact hnotE (red.fwd t hP)
  · obtain ⟨t, henc, hnotP⟩ := hover
    exact hnotP (red.bwd t henc)

/-- Failing a reduction is a mismatch: a one-sided containment failure is
exactly an under- or over-production. -/
theorem mismatch_of_not_reduction {A : ApiSig} (P : PrimLTS2 A) (O : BaseOpsSig) (enc : Encoding O A)
    (h : ¬ Reduction P enc) : Mismatch P O enc := by
  by_cases hfwd : ∀ t, TracesPrimS A P t → TracesEncS O A enc t
  · have hbwd : ¬ ∀ t, TracesEncS O A enc t → TracesPrimS A P t := by
      intro hbwd
      exact h ⟨hfwd, hbwd⟩
    obtain ⟨t, ht⟩ := exists_not_of_not_forall _ hbwd
    obtain ⟨henc, hnotP⟩ := and_not_of_not_imp _ _ ht
    exact Or.inr ⟨t, henc, hnotP⟩
  · obtain ⟨t, ht⟩ := exists_not_of_not_forall _ hfwd
    obtain ⟨hP, hnotE⟩ := and_not_of_not_imp _ _ ht
    exact Or.inl ⟨t, hP, hnotE⟩

/-- To conclude `IrreducibleTo P O` it suffices to refute every reduction
over the base. -/
theorem irreducible_of_no_reduction {A : ApiSig} (P : PrimLTS2 A) (O : BaseOpsSig)
    (h : ∀ enc : Encoding O A, ¬ Reduction P enc) : IrreducibleTo P O :=
  fun enc => mismatch_of_not_reduction P O enc (h enc)

/-- Conversely, a primitive with no reduction over the base is irreducible:
obligation preservation is a corollary of `Reduction` itself. -/
theorem irreducible_of_not_reducible {A : ApiSig} {P : PrimLTS2 A} {O : BaseOpsSig}
    {obls : List (Trace A → Prop)} (h : ¬ Reducible P O obls) : IrreducibleTo P O := by
  refine irreducible_of_no_reduction P O fun enc red => h ⟨enc, red, ?_⟩
  exact obligations_preserved_of_reduction red obls

/-- The two criteria coincide with `Reducible` negated. -/
theorem irreducible_iff_not_reducible {A : ApiSig} {P : PrimLTS2 A} (O : BaseOpsSig)
    (obls : List (Trace A → Prop)) : IrreducibleTo P O ↔ ¬ Reducible P O obls :=
  ⟨not_reducible_of_irreducible, fun h => irreducible_of_not_reducible h⟩

/-- The strong single-trace witness demotes to a one-sided instance of the
complete criterion. -/
theorem irreducible_of_separated {A : ApiSig} {P : PrimLTS2 A} {O : BaseOpsSig} {t : Trace A}
    (h : SeparatedBy P O t) : IrreducibleTo P O :=
  fun enc => Or.inl ⟨t, h.1, h.2 enc⟩

/-- A primitive every encoding over the base over-produces is irreducible. -/
theorem irreducible_of_always_over {A : ApiSig} {P : PrimLTS2 A} {O : BaseOpsSig}
    (h : ∀ enc : Encoding O A, OverProduces P O enc) : IrreducibleTo P O :=
  fun enc => Or.inr (h enc)

end Sluice.Formal
