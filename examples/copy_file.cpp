// copy_file: copy src -> dst using copy_all, with explicit flush.
// Optional 3rd argument caps the copy at N bytes (CopyLimit::bytes(N)).
#include <cppio/file.hpp>
#include <cppio/copy.hpp>
#include <cppio/limit.hpp>

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "usage: %s <src> <dst> [max_bytes]\n", argv[0]);
        return 2;
    }
    cppio::FileReader reader(argv[1]);
    cppio::FileWriter writer(argv[2]);
    if (!reader.opened()) {
        std::fprintf(stderr, "cannot open source: %s\n", argv[1]);
        return 1;
    }
    if (!writer.opened()) {
        std::fprintf(stderr, "cannot open destination: %s\n", argv[2]);
        return 1;
    }

    cppio::CopyLimit limit = cppio::CopyLimit::unlimited();
    if (argc == 4) {
        char* end = nullptr;
        unsigned long long n = std::strtoull(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0') {
            std::fprintf(stderr, "invalid max_bytes: %s\n", argv[3]);
            return 2;
        }
        limit = cppio::CopyLimit::bytes(n);
    }

    std::vector<std::byte> scratch(8192);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch), limit);
    auto flush_res = writer.flush();
    if (!res.has_value()) {
        std::fprintf(stderr, "copy failed: %s\n",
                     cppio::to_string(res.error().code).data());
        return 1;
    }
    if (!flush_res.has_value()) {
        std::fprintf(stderr, "flush failed: %s\n",
                     cppio::to_string(flush_res.error().code).data());
        return 1;
    }
    std::printf("copied %llu bytes\n", static_cast<unsigned long long>(res.value()));
    return 0;
}
