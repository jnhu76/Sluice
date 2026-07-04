// Shared RAII temp-file path helper for benches and examples (sluice-CORE-024S).
//
// Eliminates ~22 duplicate TempPath structs across bench/ and examples/. Each
// copy had its own counter, move-semantics quirks, and a `remove("")` smell on
// moved-from instances. This single header is the one source of truth: move-safe
// (moved-from dtor is a no-op), atomic counter (race-free under any future
// concurrent construction), taggable filename prefix, and noexcept cleanup.
//
// Usage:
//   TempPath tp("w1");      // -> /tmp/sluice_w1_<ptr>_<ctr>.tmp
//   std::string p = tp.str();
//   // tp's dtor removes the file (if it still owns the path).
//
// Pure C++17/20 (std::filesystem + std::atomic). No C runtime mixing.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>

namespace sluice::bench {

class TempPath {
  public:
    TempPath() : TempPath("sluice") {}

    // Construct with a filename tag (e.g. "w1", "mvp_copy"). The full name is
    // `<tag>_<ptr>_<counter>.tmp` under temp_directory_path(), unique per
    // instance + robust to concurrent construction.
    explicit TempPath(const char* tag) {
        std::ostringstream oss;
        oss << (tag ? tag : "sluice") << "_" << std::hex << reinterpret_cast<std::uintptr_t>(this)
            << "_" << counter_.fetch_add(1) << ".tmp";
        p_ = std::filesystem::temp_directory_path() / oss.str();
    }

    ~TempPath() {
        // Moved-from: p_ is empty (clear()ed by the move ctor) -> no-op. The
        // remove() overload is non-throwing; the try/catch is belt-and-suspenders
        // against a filesystem_error on a concurrent remove of the same path.
        if (!p_.empty()) {
            try {
                std::filesystem::remove(p_);
            } catch (...) {
                // Cleanup best-effort; never propagate from a dtor.
            }
        }
    }

    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;

    TempPath(TempPath&& o) noexcept : p_(std::move(o.p_)) { o.p_.clear(); }
    TempPath& operator=(TempPath&& o) noexcept {
        if (this != &o) {
            if (!p_.empty()) {
                try {
                    std::filesystem::remove(p_);
                } catch (...) {}
            }
            p_ = std::move(o.p_);
            o.p_.clear();
        }
        return *this;
    }

    const std::filesystem::path& path() const { return p_; }
    std::string str() const { return p_.string(); }

  private:
    std::filesystem::path p_;
    static inline std::atomic<std::size_t> counter_{0};
};

} // namespace sluice::bench
