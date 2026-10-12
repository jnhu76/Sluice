#include <sluice/io_context.hpp>
int main(){sluice::BlockingIoContext c; auto r=c.open_reader("/dev/null"); auto w=c.open_writer("/dev/null"); return (!r || !w) ? 1 : 0;}
