#include <sluice/writer.hpp>
#include <array>
struct W:sluice::Writer {std::size_t count=0; sluice::Result<std::size_t> write_some(std::span<const std::byte> s) override {count+=s.size(); return s.size();} sluice::Result<void> flush() override{return {};}};
int main(){W w; std::array<std::byte,2> data{}; return (!w.write_all(data) || w.count!=2) ? 1 : 0;}
