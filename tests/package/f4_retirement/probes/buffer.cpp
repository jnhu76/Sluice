#include <sluice/buffer.hpp>
#include <sluice/fault.hpp>
#include <array>
int main(){auto r=sluice::MemoryReader::from_string("abc"); sluice::MemoryWriter w; std::array<std::byte,4> a{},b{},out{}; sluice::BufferedReader br(r,a); sluice::BufferedWriter bw(w,b); auto n=br.read_some(out); if(!n || n.value()!=3) return 1; if(!bw.write_all(std::span(out).first(3)) || !bw.flush() || w.bytes().size()!=3) return 2;}
