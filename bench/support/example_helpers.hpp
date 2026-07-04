// Shared helpers for example programs.
#pragma once

#include <sluice/measurement.hpp>

#include <fstream>
#include <iterator>
#include <string>

namespace sluice::bench {

inline bool file_read_all(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

inline const char* stop_reason(const sluice::CopyStats& s) {
    if (s.eof_stops) {
        return "eof";
    }
    if (s.limit_stops) {
        return "limit";
    }
    if (s.reader_error_stops) {
        return "reader_error";
    }
    if (s.writer_error_stops) {
        return "writer_error";
    }
    return "none";
}

} // namespace sluice::bench
