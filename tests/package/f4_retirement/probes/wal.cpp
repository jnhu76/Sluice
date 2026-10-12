#include <sluice/wal.hpp>
#include <sluice/fault.hpp>
#include <array>
int main(){sluice::MemoryWriter w; sluice::wal::WalWriter wal(w); std::array<std::byte,2> data{std::byte{1},std::byte{2}}; if(!wal.write_record(data) || !wal.flush()) return 1; auto r=sluice::MemoryReader::from_bytes(w.bytes()); auto v=sluice::wal::read_record(r); return (!v || v.value()!=std::vector<std::byte>(data.begin(),data.end())) ? 2 : 0;}
