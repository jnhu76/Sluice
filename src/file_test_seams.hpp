













#pragma once

#include <cerrno>
#include <cstddef>
#include <utility>
#include <vector>

namespace sluice::file_testing {




class CloseScript {
  public:
    struct Step {
        int ret;
        int err;
    };

    explicit CloseScript(std::vector<Step> steps)
        : steps_(std::move(steps)), prev_(active()) {
        active() = this;
    }
    ~CloseScript() {
        if (active() == this) {
            active() = prev_;
        }
    }
    CloseScript(const CloseScript&) = delete;
    CloseScript& operator=(const CloseScript&) = delete;




    static CloseScript*& active() {
        static CloseScript* armed = nullptr;
        return armed;
    }







    int next(int ) {
        ++calls_;
        if (pos_ >= steps_.size()) {
            errno = EBADF;
            return -1;
        }
        Step s = steps_[pos_++];
        errno = s.err;
        return s.ret;
    }

    std::size_t calls() const { return calls_; }

  private:
    std::vector<Step> steps_;
    CloseScript* prev_ = nullptr;
    std::size_t pos_ = 0;
    std::size_t calls_ = 0;
};

}
