#include <sluice/buffered_readable.hpp>
struct B:sluice::BufferedReadable {std::span<const std::byte> peek_buffered() const override{return {};} sluice::Result<void> consume_buffered(std::size_t) override{return {};}};
int main(){B b; return !b.peek_buffered().empty() || !b.consume_buffered(0);}
