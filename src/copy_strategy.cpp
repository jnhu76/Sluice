


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

}
