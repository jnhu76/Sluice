#include <sluice/sync.hpp>
struct S:sluice::SyncableWriter {sluice::Result<void> sync_data() override{return {};} sluice::Result<void> sync_all() override{return {};}};
int main(){S s; return !s.sync_data() || !s.sync_all();}
