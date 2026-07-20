------------------------------ MODULE E13Select ------------------------------
(*
  Thin compatibility/root instance for the existing E13 Select scene configs.

  Stable contract:        E13SelectContract.tla
  Candidate A strategy:   E13SelectCentralClaim.tla
  Event/Timer adapters:   E13SelectEventTimer.tla
*)
EXTENDS E13SelectEventTimer

Spec == EventTimerSpec
Inv == EventTimerInv

=============================================================================
