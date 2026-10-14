#include <cstdio>
#include <cstring>

extern "C" const char* odr_description_a();
extern "C" const char* odr_description_b();
extern "C" const void* odr_chunk_entry_a();
extern "C" const void* odr_chunk_entry_b();
extern "C" const void* odr_unexpected_entry_a();
extern "C" const void* odr_unexpected_entry_b();

int main() {
    const bool described_alike = std::strcmp(odr_description_a(), odr_description_b()) == 0;
    const bool chunk_equal = odr_chunk_entry_a() == odr_chunk_entry_b();
    const bool unexpected_equal = odr_unexpected_entry_a() == odr_unexpected_entry_b();
    std::printf("a=%s\nb=%s\nchunk equal=%d\nunexpected equal=%d\n", odr_description_a(),
                odr_description_b(), int(chunk_equal), int(unexpected_equal));
    return (described_alike && chunk_equal && unexpected_equal) ? 0 : 1;
}
