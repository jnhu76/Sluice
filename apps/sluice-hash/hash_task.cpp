#include "hash_task.hpp"

#include "sha256.hpp"

#include <sluice/blocking/file.hpp>

#include <limits>
#include <new>
#include <system_error>
#include <utility>
#include <vector>

namespace sluice_hash {

namespace {

using sluice::IoError;

IoError current_exception_error() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return IoError{IoError::Code::no_space};
    } catch (const std::system_error& error) {
        const int native = error.code().value();
        return IoError{sluice::detail::canonical_error_code(native), native};
    } catch (...) {
        return IoError{IoError::Code::backend_error};
    }
}

void fail_all(std::vector<HashInput>& inputs, std::vector<FileHash>& out, IoError err) {
    out.reserve(inputs.size());
    for (auto& in : inputs) {
        FileHash f;
        f.path = in.path;
        f.error = err;
        out.push_back(std::move(f));
    }
}

struct HashScan {
    std::vector<HashInput> inputs;
    std::vector<std::uint8_t> buffer;

    void hash_one(const HashInput& in, std::vector<FileHash>& results) {
        FileHash out;
        out.path = in.path;

        Sha256 hasher;
        std::uint8_t digest[Sha256::kDigestBytes];

        std::uint64_t offset = 0;
        for (;;) {
            auto rr = sluice::blocking::read_at(
                in.file, offset,
                std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()), buffer.size()));
            if (!rr.has_value()) {
                out.error = rr.error();
                break;
            }
            std::size_t n = rr.value();
            if (n == 0) {
                hasher.final(digest);
                char hex[65];
                sha256_hex(digest, hex);
                out.hex = hex;
                break;
            }
            if (offset > std::numeric_limits<std::uint64_t>::max() - n) {
                out.error = IoError{IoError::Code::invalid_state};
                break;
            }
            hasher.update(buffer.data(), n);
            out.bytes_hashed += n;
            offset += n;
        }
        results.push_back(std::move(out));
    }
};

}

std::vector<FileHash> hash_files(std::vector<HashInput> inputs, std::size_t buffer_size) {
    std::vector<FileHash> results;
    if (!valid_config(buffer_size)) {
        fail_all(inputs, results, IoError{IoError::Code::invalid_state});
        return results;
    }
    std::vector<std::uint8_t> buffer;
    try {
        buffer.assign(buffer_size, 0);
    } catch (const std::bad_alloc&) {
        fail_all(inputs, results, IoError{IoError::Code::no_space});
        return results;
    }
    HashScan scan{std::move(inputs), std::move(buffer)};
    results.reserve(scan.inputs.size());
    try {
        for (const auto& input : scan.inputs)
            scan.hash_one(input, results);
    } catch (...) {
        const IoError error = current_exception_error();
        for (std::size_t i = results.size(); i < scan.inputs.size(); ++i) {
            FileHash out;
            out.path = scan.inputs[i].path;
            out.error = error;
            results.push_back(std::move(out));
        }
    }
    return results;
}

}
