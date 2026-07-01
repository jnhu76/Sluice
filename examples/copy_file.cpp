// copy_file: copy src -> dst using copy_all, with explicit flush.
#include <cppio/file.hpp>
#include <cppio/copy.hpp>

#include <cstdio>
#include <span>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <src> <dst>\n", argv[0]);
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

    std::vector<std::byte> scratch(8192);
    auto res = cppio::copy_all(reader, writer, std::span<std::byte>(scratch));
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
