#include <sluice/memory_io_context.hpp>
int main(){sluice::MemoryIoContext c; c.seed("x",{std::byte{1}}); auto r=c.open_reader("x"); auto w=c.open_writer("x"); return (!r || !w) ? 1 : 0;}
