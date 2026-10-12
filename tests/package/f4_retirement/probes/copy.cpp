#include <sluice/copy.hpp>
#include <sluice/fault.hpp>
int main(){auto r=sluice::MemoryReader::from_string("abc"); sluice::MemoryWriter w; auto n=sluice::copy_all(r,w); return (!n || n.value()!=3 || w.bytes().size()!=3) ? 1 : 0;}
