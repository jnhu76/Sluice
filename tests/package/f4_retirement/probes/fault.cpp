#include <sluice/fault.hpp>
#include <array>
int main(){auto r=sluice::MemoryReader::from_string("abc"); sluice::FaultPlan plan; sluice::FaultReader f(r,plan); std::array<std::byte,3> data{}; auto n=f.read_some(data); return (!n || n.value()!=3) ? 1 : 0;}
