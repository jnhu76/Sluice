
















#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sluice_grep {



struct MatchEvent {
    std::uint64_t line_no;
    std::string line;
};

class LineMatcher {
public:




    LineMatcher(std::string pattern, std::size_t max_line_bytes);




    void feed(const std::uint8_t* data, std::size_t len,
              std::vector<MatchEvent>& out);




    void finish(std::vector<MatchEvent>& out);



    bool dropped_long_lines() const { return dropped_long_; }



    std::uint64_t complete_lines() const { return line_no_; }

private:



    void scan_complete_region(const char* p, std::size_t i, std::size_t end,
                              std::vector<MatchEvent>& out);

    std::string pattern_;
    std::size_t max_line_bytes_;
    std::string carry_;
    std::size_t anchor_off_ = 0;
    bool pattern_has_nl_ = false;
    std::uint64_t line_no_ = 0;
    bool dropping_ = false;
    bool dropped_long_ = false;
};




bool line_contains(std::string_view line, std::string_view pattern);

}
