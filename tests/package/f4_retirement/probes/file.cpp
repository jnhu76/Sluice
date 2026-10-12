#include <sluice/file.hpp>
#include <array>
#include <unistd.h>
int main() { char path[]="/tmp/sluice-f4-XXXXXX"; int fd=mkstemp(path); if(fd<0) return 1; close(fd); struct Cleanup {const char* p; ~Cleanup(){unlink(p);}} cleanup{path}; std::array<std::byte,3> data{std::byte{1},std::byte{2},std::byte{3}}, out{}; sluice::FileWriter w{std::string(path)}; if(!w.opened() || !w.write_at_all(0,data) || !w.close()) return 2; sluice::FileReader r{std::string(path)}; if(!r.opened() || !r.read_at_exact(0,out) || !r.close() || out!=data) return 3; }
