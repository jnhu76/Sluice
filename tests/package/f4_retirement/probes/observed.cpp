#include <sluice/observed.hpp>
#include <sluice/fault.hpp>
#include <array>
int main(){auto r=sluice::MemoryReader::from_string("abc"); sluice::ReaderStats s; sluice::ObservedReader o(r,s); std::array<std::byte,3> data{}; auto n=o.read_some(data); return (!n || n.value()!=3) ? 1 : 0;}
