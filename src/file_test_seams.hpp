#pragma once

// An exhausted script reports EBADF instead of falling through, so a test
// cannot silently lose its injection. Scripts must be destroyed in reverse
// order of construction, or the restore leaves the active pointer dangling.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace sluice::file_testing {

enum class NativeCall : std::uint8_t {
    close,
    read,
    pread,
    write,
    pwrite,
};

constexpr std::uint8_t call_bit(NativeCall call) noexcept {
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(call));
}

inline constexpr std::uint8_t kCloseCall = call_bit(NativeCall::close);
inline constexpr std::uint8_t kTransferCalls = call_bit(NativeCall::read) |
                                               call_bit(NativeCall::pread) |
                                               call_bit(NativeCall::write) |
                                               call_bit(NativeCall::pwrite);

class NativeScript {
  public:
    struct Step {
        long ret;
        int err;
    };

    struct Call {
        const void* buffer = nullptr;
        std::size_t count = 0;
        long offset = -1;
    };

    static constexpr std::size_t kMaxSteps = 8;

    NativeScript(std::uint8_t family, int fd, std::initializer_list<Step> steps) noexcept
        : family_(family), fd_(fd), previous_(active()) {
        active() = this;
        for (const Step& step : steps) {
            if (step_count_ == kMaxSteps) {
                std::fprintf(stderr, "NativeScript: more than %zu steps given\n", kMaxSteps);
                std::abort();
            }
            steps_[step_count_++] = step;
        }
    }

    ~NativeScript() {
        if (active() == this)
            active() = previous_;
    }

    NativeScript(const NativeScript&) = delete;
    NativeScript& operator=(const NativeScript&) = delete;

    bool intercepts(NativeCall call, int fd) const noexcept {
        return (family_ & call_bit(call)) != 0 && (fd_ < 0 || fd_ == fd);
    }

    // Precondition: intercepts(call, fd).
    long next(NativeCall call, int fd, const void* buffer = nullptr, std::size_t count = 0,
              long offset = -1) noexcept {
        (void)call;
        ++calls_;
        last_fd_ = fd;
        last_call_ = Call{buffer, count, offset};
        if (position_ >= step_count_) {
            errno = EBADF;
            return -1;
        }
        const Step step = steps_[position_++];
        errno = step.err;
        return step.ret;
    }

    std::size_t calls() const noexcept { return calls_; }

    int last_fd() const noexcept { return last_fd_; }

    const Call& last_call() const noexcept { return last_call_; }

    static NativeScript*& active() noexcept {
        static thread_local NativeScript* armed = nullptr;
        return armed;
    }

  private:
    std::uint8_t family_;
    int fd_;
    Step steps_[kMaxSteps] = {};
    std::size_t step_count_ = 0;
    std::size_t position_ = 0;
    std::size_t calls_ = 0;
    int last_fd_ = -1;
    Call last_call_{};
    NativeScript* previous_ = nullptr;
};

}
