// cat: read a file and stream its bytes to stdout. Demonstrates FileReader +
// FileWriter composition + observed stats.
#include <cppio/file.hpp>
#include <cppio/observed.hpp>
#include <cppio/copy.hpp>

#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
    }
    std::string path = argv[1];
    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "%s: no such file\n", path.c_str());
        return 1;
    }

    cppio::FileReader reader(path);
    cppio::FileWriter writer(STDOUT_FILENO);  // adopt fd 1; /dev/stdout can't be opened on some systems

    cppio::ReaderStats stats{};
    cppio::ObservedReader observed(reader, stats);

    std::vector<std::byte> scratch(8192);
    auto res = cppio::copy_all(observed, writer, std::span<std::byte>(scratch));
    (void)writer.flush();
    if (!res.has_value()) {
        std::fprintf(stderr, "copy failed: %s\n",
                     cppio::to_string(res.error().code).data());
        return 1;
    }

    std::fprintf(stderr, "cat: copied %llu bytes, read_calls=%llu eof_count=%llu\n",
                 static_cast<unsigned long long>(res.value()),
                 static_cast<unsigned long long>(stats.read_calls),
                 static_cast<unsigned long long>(stats.eof_count));
    return 0;
}
