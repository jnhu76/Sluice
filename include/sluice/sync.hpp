











#pragma once

#include <sluice/result.hpp>

namespace sluice {

class SyncableWriter {
  public:
    virtual ~SyncableWriter() = default;



    virtual Result<void> sync_data() = 0;



    virtual Result<void> sync_all() = 0;
};

}
