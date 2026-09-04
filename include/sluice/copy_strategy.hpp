// sluice::CopyStrategy — explicit copy path selection layer.
//
// Path selection is explicit: a CopyOptions selects a CopyStrategy, copy_all
// executes the selected (existing) path, and a CopyDecision explains what was
// requested vs what ran. This stops copy_all from becoming a pile of hidden
// heuristics (such as an implicit buffered fast path triggered by a
// dynamic_cast probe) and makes each path testable.
//
// Auto / Scratch / BufferedFirst are implemented. A future copy mechanism
// (vector copy, copy_file_range, sendfile, splice) is added as a normal
// implemented strategy when it is actually built; no unimplemented reservation
// is part of the contract (SEMANTIC-DIET-0: speculative reserved slots removed
// — research/semantic-diet-0/SEMANTIC-DIET-0-REPORT.md).
#pragma once

#include <sluice/limit.hpp>

#include <cstdint>
#include <string_view>

namespace sluice {

// Which copy path to use. See docs/reference/api.md (CopyStrategy).
enum class CopyStrategy {
    Auto,          // default; currently behaves as BufferedFirst
    Scratch,       // force the scratch read/write loop; never use fast path
    BufferedFirst, // drain buffered bytes first, then scratch
};

// Caller-facing options. Existing copy_all overloads delegate with
// CopyOptions{limit, CopyStrategy::Auto}.
struct CopyOptions {
    CopyLimit limit = CopyLimit::unlimited();
    CopyStrategy strategy = CopyStrategy::Auto;
};

// Explains what strategy was requested vs what actually ran. Filled by copy_all
// when a non-null pointer is passed. Default-constructed == a plain Auto copy
// that moved no bytes yet.
struct CopyDecision {
    CopyStrategy requested = CopyStrategy::Auto;
    CopyStrategy selected = CopyStrategy::Auto;
    std::string_view reason = "auto";
    bool used_buffered_fast_path = false;
    bool used_scratch_path = false;
};

// Stable string views over string literals (so they outlive any temporary).
std::string_view to_string(CopyStrategy strategy);

} // namespace sluice
