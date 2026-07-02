// to_string implementations for CopyStrategy / UnsupportedStrategyPolicy.
// Returns string-literal-backed views (static storage duration), so the
// returned std::string_view is always valid.
#include <cppio/copy_strategy.hpp>

namespace cppio {

std::string_view to_string(CopyStrategy strategy) {
    switch (strategy) {
        case CopyStrategy::Auto: return "auto";
        case CopyStrategy::Scratch: return "scratch";
        case CopyStrategy::BufferedFirst: return "buffered_first";
        case CopyStrategy::VectorDeferred: return "vector_deferred";
        case CopyStrategy::FileRangeDeferred: return "file_range_deferred";
        case CopyStrategy::SendfileDeferred: return "sendfile_deferred";
        case CopyStrategy::SpliceDeferred: return "splice_deferred";
    }
    return "unknown";
}

std::string_view to_string(UnsupportedStrategyPolicy policy) {
    switch (policy) {
        case UnsupportedStrategyPolicy::ReturnInvalidState: return "return_invalid_state";
        case UnsupportedStrategyPolicy::FallbackToAuto: return "fallback_to_auto";
    }
    return "unknown";
}

}  // namespace cppio
