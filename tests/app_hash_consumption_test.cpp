#include "hash_task.hpp"
#include "sha256.hpp"

#include <sluice/file_resource.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice_hash::HashInput;
using sluice_hash::Sha256;
using sluice_hash::sha256_hex;

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_app_hash_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    std::string out_path = path;
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(content.size())) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            ::close(fd);
            ::unlink(out_path.c_str());
            return {};
        }
        written += n;
    }
    ::close(fd);
    return out_path;
}

std::string digest_hex(const std::string& content) {
    Sha256 hasher;
    hasher.update(reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
    std::uint8_t digest[Sha256::kDigestBytes];
    hasher.final(digest);
    char hex[65];
    sha256_hex(digest, hex);
    return std::string(hex);
}

bool hash_files_streams_known_digest_over_file() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;

    std::vector<HashInput> inputs;
    inputs.push_back(HashInput{path, std::move(opened).value()});

    const auto results = sluice_hash::hash_files(std::move(inputs), 4096, 1);
    if (results.size() != 1)
        return false;
    if (results[0].error.has_value())
        return false;
    if (results[0].bytes_hashed != 3)
        return false;
    return results[0].hex ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
}

bool hash_files_multi_chunk_matches_direct_digest() {
    std::string content;
    content.reserve(10000);
    for (int i = 0; i < 10000; ++i)
        content.push_back(static_cast<char>(i & 0xFF));
    const std::string path = make_temp_file(content);
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;

    std::vector<HashInput> inputs;
    inputs.push_back(HashInput{path, std::move(opened).value()});

    const auto results = sluice_hash::hash_files(std::move(inputs), 4096, 1);
    if (results.size() != 1)
        return false;
    if (results[0].error.has_value())
        return false;
    if (results[0].bytes_hashed != 10000)
        return false;
    return results[0].hex == digest_hex(content);
}

bool hash_files_empty_file_yields_empty_digest() {
    const std::string path = make_temp_file("");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;

    std::vector<HashInput> inputs;
    inputs.push_back(HashInput{path, std::move(opened).value()});

    const auto results = sluice_hash::hash_files(std::move(inputs), 4096, 1);
    if (results.size() != 1)
        return false;
    if (results[0].error.has_value())
        return false;
    if (results[0].bytes_hashed != 0)
        return false;
    return results[0].hex ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
}

bool hash_files_reports_error_for_closed_file() {
    const std::string path = make_temp_file("abc");
    if (path.empty())
        return false;
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value())
        return false;
    File file = std::move(opened).value();
    if (!file.close().has_value())
        return false;

    std::vector<HashInput> inputs;
    inputs.push_back(HashInput{path, std::move(file)});

    const auto results = sluice_hash::hash_files(std::move(inputs), 4096, 1);
    if (results.size() != 1)
        return false;
    return results[0].error.has_value();
}

bool hash_files_preserves_input_order() {
    const std::string path_a = make_temp_file("abc");
    const std::string path_b = make_temp_file("def");
    if (path_a.empty() || path_b.empty()) {
        if (!path_a.empty())
            ::unlink(path_a.c_str());
        if (!path_b.empty())
            ::unlink(path_b.c_str());
        return false;
    }
    auto opened_a = File::open(path_a);
    auto opened_b = File::open(path_b);
    ::unlink(path_a.c_str());
    ::unlink(path_b.c_str());
    if (!opened_a.has_value() || !opened_b.has_value())
        return false;

    std::vector<HashInput> inputs;
    inputs.push_back(HashInput{path_a, std::move(opened_a).value()});
    inputs.push_back(HashInput{path_b, std::move(opened_b).value()});

    const auto results = sluice_hash::hash_files(std::move(inputs), 4096, 1);
    if (results.size() != 2)
        return false;
    if (results[0].path != path_a || results[1].path != path_b)
        return false;
    if (results[0].error.has_value() || results[1].error.has_value())
        return false;
    return results[0].hex == digest_hex("abc") && results[1].hex == digest_hex("def");
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"hash_files_streams_known_digest_over_file",
         hash_files_streams_known_digest_over_file},
        {"hash_files_multi_chunk_matches_direct_digest",
         hash_files_multi_chunk_matches_direct_digest},
        {"hash_files_empty_file_yields_empty_digest",
         hash_files_empty_file_yields_empty_digest},
        {"hash_files_reports_error_for_closed_file",
         hash_files_reports_error_for_closed_file},
        {"hash_files_preserves_input_order", hash_files_preserves_input_order},
    };

    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
    }
    std::printf("all %zu app hash consumption tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
