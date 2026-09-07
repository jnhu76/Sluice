#pragma once

#include <cstdint>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

namespace sluice::async::detail {

struct Tax0AblationModes {
    bool f01_gate_outstanding_eval = false;

    bool f02_skip_reap_seq = false;

    bool f07_skip_extent_reprobes = false;
};

inline Tax0AblationModes g_tax0_ablation_modes{};

inline Tax0AblationModes& tax0_ablation_modes() noexcept {
    return g_tax0_ablation_modes;
}
inline bool tax0_f01_gate_outstanding_eval() noexcept {
    return g_tax0_ablation_modes.f01_gate_outstanding_eval;
}
inline bool tax0_f02_skip_reap_seq() noexcept {
    return g_tax0_ablation_modes.f02_skip_reap_seq;
}
inline bool tax0_f07_skip_extent_reprobes() noexcept {
    return g_tax0_ablation_modes.f07_skip_extent_reprobes;
}

} // namespace sluice::async::detail

#endif
