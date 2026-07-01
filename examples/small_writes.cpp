// small_writes: many tiny writes through BufferedWriter, observed for stats.
// Shows the composition pattern: file -> observed -> buffered.
#include <cppio/file.hpp>
#include <cppio/observed.hpp>
#include <cppio/buffer.hpp>

#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string out_path = argc >= 2 ? argv[1] : "/dev/null";
    cppio::FileWriter file(out_path);
    if (!file.opened()) {
        std::fprintf(stderr, "cannot open %s\n", out_path.c_str());
        return 1;
    }

    cppio::WriterStats stats{};
    cppio::ObservedWriter observed(file, stats);

    std::vector<std::byte> backing(64);
    cppio::BufferedWriter buffered(observed, std::span<std::byte>(backing));

    const int n = 1000;
    for (int i = 0; i < n; ++i) {
        char c = 'a' + (i % 26);
        std::byte b{static_cast<unsigned char>(c)};
        auto res = buffered.write_all(std::span<const std::byte>(&b, 1));
        if (!res.has_value()) {
            std::fprintf(stderr, "write failed: %s\n",
                         cppio::to_string(res.error().code).data());
            return 1;
        }
    }
    auto flush_res = buffered.flush();
    if (!flush_res.has_value()) {
        std::fprintf(stderr, "flush failed: %s\n",
                     cppio::to_string(flush_res.error().code).data());
        return 1;
    }

    std::printf("small_writes: %d app writes -> inner write_calls=%llu "
                "write_bytes=%llu short_writes=%llu flush_calls=%llu\n",
                n,
                static_cast<unsigned long long>(stats.write_calls),
                static_cast<unsigned long long>(stats.write_bytes),
                static_cast<unsigned long long>(stats.short_writes),
                static_cast<unsigned long long>(stats.flush_calls));
    return 0;
}
