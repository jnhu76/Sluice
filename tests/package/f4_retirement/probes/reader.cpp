#include <sluice/reader.hpp>
#include <sluice/writer.hpp>
#include <array>
struct R:sluice::Reader { bool done=false; sluice::Result<std::size_t> read_some(std::span<std::byte> s) override {if(done || s.empty()) return std::size_t{0}; s[0]=std::byte{7}; done=true; return std::size_t{1};}};
struct W:sluice::Writer {std::size_t count=0; sluice::Result<std::size_t> write_some(std::span<const std::byte> s) override {count+=s.size(); return s.size();} sluice::Result<void> flush() override{return {};}};
int main(){R r; W w; auto n=r.stream_to(w); return (!n || n.value()!=1 || w.count!=1) ? 1 : 0;}
