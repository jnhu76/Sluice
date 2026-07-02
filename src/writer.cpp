// Implementation of Writer::write_all — loops over short writes, propagates
// errors, and treats zero progress on non-empty input as invalid_state.
#include <cppio/writer.hpp>

#include <cstddef>
#include <vector>

namespace cppio {

Result<void> Writer::write_all(std::span<const std::byte> src) {
    while (!src.empty()) {
        auto r = write_some(src);
        if (!r.has_value()) return make_unexpected(r.error());
        std::size_t n = r.value();
        if (n == 0) {
            return make_unexpected(IoError{IoError::Code::invalid_state});
        }
        if (n > src.size()) {
            // Defensive: a writer returning more than asked is broken.
            return make_unexpected(IoError{IoError::Code::invalid_state});
        }
        src = src.subspan(n);
    }
    return {};
}

// Default vector fallback: drive each non-empty slice through write_some in
// order. A conservative writev-style primitive (CPPIO-CORE-005B): it STOPS
// after the first short write (returns the partial total) or on the first
// error (propagated immediately, even after progress — consistent with
// write_all). This is NOT write_all over slices; write_all_vec is the
// all-or-error helper built on top. A concrete POSIX writer (FileWriter)
// overrides this with a single writev exposing the same stop-on-short-write
// semantics. All-empty input returns 0 with no write_some calls.
Result<std::size_t> Writer::write_vec(std::span<const ConstIoSlice> srcs) {
    std::size_t total = 0;
    for (const auto& s : srcs) {
        if (s.bytes.empty()) continue;  // empty slices are skipped, no syscall
        auto r = write_some(s.bytes);
        if (!r.has_value()) return make_unexpected<std::size_t>(r.error());
        std::size_t n = r.value();
        if (n > s.bytes.size()) {
            // Defensive: a writer returning more than asked is broken.
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
        }
        total += n;
        if (n < s.bytes.size()) break;  // short write: stop and report partial
    }
    return total;
}

// Vector derived: write every byte of every (non-empty) slice. Loops write_vec
// across the remaining tail, tracking a partial offset into the first remaining
// slice so a resume re-issues the tail-end of that slice rather than skipping or
// duplicating it. Rejects zero progress on non-empty remaining input as
// invalid_state (same rule as write_all). Bytes always written in order.
Result<void> Writer::write_all_vec(std::span<const ConstIoSlice> srcs) {
    std::size_t idx = 0;
    std::size_t head_offset = 0;  // bytes already written of srcs[idx]
    while (idx < srcs.size()) {
        // Drive the remaining tail through write_vec. The fast path (no
        // partial first slice) is a plain subspan with no allocation; only a
        // resume inside a partially-written slice needs a temporary vector so
        // the first entry is the tail of srcs[idx].
        std::size_t n_remaining = srcs.size() - idx;
        auto drive = [&](std::span<const ConstIoSlice> rem) {
            return write_vec(rem);
        };
        Result<std::size_t> r = [&]() -> Result<std::size_t> {
            if (head_offset == 0) {
                return drive(srcs.subspan(idx));
            }
            std::vector<ConstIoSlice> remaining;
            remaining.reserve(n_remaining);
            remaining.push_back(
                ConstIoSlice{srcs[idx].bytes.subspan(head_offset)});
            for (std::size_t i = idx + 1; i < srcs.size(); ++i) {
                remaining.push_back(srcs[i]);
            }
            return drive(std::span<const ConstIoSlice>(remaining));
        }();
        if (!r.has_value()) return make_unexpected(r.error());
        std::size_t written = r.value();
        if (written == 0) {
            // No progress. Only an error if there is still non-empty data left.
            bool any_left = false;
            for (std::size_t i = idx; i < srcs.size(); ++i) {
                std::size_t off = (i == idx) ? head_offset : 0;
                if (srcs[i].bytes.size() > off) { any_left = true; break; }
            }
            if (any_left) {
                return make_unexpected(IoError{IoError::Code::invalid_state});
            }
            break;  // nothing non-empty remains: done
        }
        // Walk written bytes forward through the slice list to find the new
        // (idx, head_offset). This keeps order and never skips/duplicates.
        while (written > 0 && idx < srcs.size()) {
            std::size_t left_in_slice = srcs[idx].bytes.size() - head_offset;
            if (written >= left_in_slice) {
                written -= left_in_slice;
                ++idx;
                head_offset = 0;
            } else {
                head_offset += written;
                written = 0;
            }
        }
    }
    return {};
}

}  // namespace cppio
