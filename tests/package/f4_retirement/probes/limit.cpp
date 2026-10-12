#include <sluice/limit.hpp>
int main(){auto n=sluice::CopyLimit::bytes(7); return !n.is_limited() || n.remaining()!=7 || !sluice::CopyLimit::unlimited().is_unlimited();}
