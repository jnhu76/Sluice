// sluice::SyncableWriter — opt-in capability interface for explicit OS-level
// persistence of already-written file data (CPPIO-CORE-008B). This is the sync
// counterpart to the drain-style Writer::flush(): flush moves user-space
// buffered bytes down the stack, sync asks the OS to persist them. The two are
// deliberately separate — flush never calls sync — so callers cannot
// accidentally believe drained bytes are durable. See
// docs/flush-sync-durability.md.
//
// It is a separate mixin interface (like BufferedReadable), not a virtual on
// the Writer base class, so writers with no sync concept (MemoryWriter,
// BufferedWriter, FaultWriter, ...) carry zero overhead and no dead virtuals.
// Detection is a dynamic_cast (see WalWriter, 008E).
#pragma once

#include <sluice/result.hpp>

namespace sluice {

class SyncableWriter {
  public:
    virtual ~SyncableWriter() = default;

    // Request persistence of file *data* (maps to fdatasync on POSIX where
    // available). Does not imply metadata persistence. Never called by flush().
    virtual Result<void> sync_data() = 0;

    // Request persistence of file data *and* metadata (maps to fsync). Never
    // called by flush().
    virtual Result<void> sync_all() = 0;
};

} // namespace sluice
