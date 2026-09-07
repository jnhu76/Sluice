













#pragma once

#include <cerrno>
#include <cstddef>
#include <utility>
#include <vector>

namespace sluice_copy::testing {




class DirFsyncScript {
  public:
    struct Step {
        int ret;
        int err;
    };

    explicit DirFsyncScript(std::vector<Step> steps)
        : steps_(std::move(steps)), prev_(active()) {
        active() = this;
    }
    ~DirFsyncScript() {
        if (active() == this) {
            active() = prev_;
        }
    }
    DirFsyncScript(const DirFsyncScript&) = delete;
    DirFsyncScript& operator=(const DirFsyncScript&) = delete;




    static DirFsyncScript*& active() {
        static DirFsyncScript* armed = nullptr;
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
    DirFsyncScript* prev_ = nullptr;
    std::size_t pos_ = 0;
    std::size_t calls_ = 0;
};

}
