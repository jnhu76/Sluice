// to_string implementations for CopyStrategy.
// Returns string-literal-backed views (static storage duration), so the
// returned std::string_view is always valid.
#include <sluice/copy_strategy.hpp>

namespace sluice {

std::string_view to_string(CopyStrategy strategy) {
    switch (strategy) {
    case CopyStrategy::Auto:
        return "auto";
    case CopyStrategy::Scratch:
        return "scratch";
    case CopyStrategy::BufferedFirst:
        return "buffered_first";
    }
    return "unknown";
}

} // namespace sluice
