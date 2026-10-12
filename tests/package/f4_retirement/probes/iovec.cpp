#include <sluice/iovec.hpp>
int main(){sluice::IoSlice i{}; sluice::ConstIoSlice c{}; return !i.bytes.empty() || !c.bytes.empty();}
