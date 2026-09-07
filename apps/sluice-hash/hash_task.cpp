







#include "hash_task.hpp"

#include "sha256.hpp"

#include <sluice/async/await_op_helpers.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace sluice_hash {

namespace {

using namespace sluice::async;
using sluice::IoError;


void fail_all(std::vector<HashInput>& inputs, std::vector<FileHash>& out,
              IoError err) {
    out.reserve(inputs.size());
    for (auto& in : inputs) {
        FileHash f;
        f.path = in.path;
        f.error = err;
        out.push_back(std::move(f));
    }
}

struct HashTask {
    std::vector<HashInput> inputs;
    std::vector<std::uint8_t> buffer;





    void hash_one(RuntimeTaskContext& ctx, const HashInput& in,
                  std::vector<FileHash>& results) {
        FileHash out;
        out.path = in.path;

        Sha256 hasher;
        std::uint8_t digest[Sha256::kDigestBytes];
        Completion<std::size_t> rc;

        std::uint64_t offset = 0;
        for (;;) {
            if (ctx.cancel_token().is_requested()) {
                out.error = IoError{IoError::Code::canceled};
                break;
            }
            auto rr = await_read_once(
                ctx, in.fd,
                std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()),
                                     buffer.size()),
                offset, rc);
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

    void operator()(RuntimeTaskContext& ctx,
                    TaskResultSlot<sluice::Result<std::vector<FileHash>>>& slot) {
        std::vector<FileHash> results;
        results.reserve(inputs.size());



        try {
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                if (ctx.cancel_token().is_requested()) {


                    FileHash out;
                    out.path = inputs[i].path;
                    out.error = IoError{IoError::Code::canceled};
                    results.push_back(std::move(out));
                    continue;
                }
                hash_one(ctx, inputs[i], results);
            }
        } catch (...) {
            auto translated = translate_task_exception<std::vector<FileHash>>();
            IoError err = translated.error();
            for (std::size_t i = results.size(); i < inputs.size(); ++i) {
                FileHash out;
                out.path = inputs[i].path;
                out.error = err;
                results.push_back(std::move(out));
            }
        }
        slot.publish(std::move(results));
    }
};

std::vector<FileHash> run_hash_engine(std::vector<HashInput> inputs,
                                      std::size_t buffer_size, unsigned workers,
                                      std::unique_ptr<AsyncBackend> backend) {

    if (buffer_size < kMinBufferSize || buffer_size > kMaxBufferSize ||
        workers == 0 || workers > kMaxWorkers || !backend) {
        std::vector<FileHash> out;
        fail_all(inputs, out, IoError{IoError::Code::invalid_state});
        return out;
    }



    std::vector<std::uint8_t> buffer;
    try {
        buffer.assign(buffer_size, 0);
    } catch (const std::bad_alloc&) {
        std::vector<FileHash> out;
        fail_all(inputs, out, IoError{IoError::Code::no_space});
        return out;
    }

    HashTask task{std::move(inputs), std::move(buffer)};






    auto result =
        run_task_to_result<std::vector<FileHash>>(workers, std::move(backend), task);
    if (!result.has_value()) {
        std::vector<FileHash> out;
        fail_all(task.inputs, out, result.error());
        return out;
    }
    return std::move(result.value());
}

}

std::vector<FileHash> hash_files(std::vector<HashInput> inputs,
                                 std::size_t buffer_size, unsigned workers) {
    return run_hash_engine(std::move(inputs), buffer_size, workers,
                           std::make_unique<ThreadPoolBackend>());
}

std::vector<FileHash> hash_files_with_backend(
    std::vector<HashInput> inputs, std::size_t buffer_size, unsigned workers,
    std::unique_ptr<sluice::async::AsyncBackend> backend) {
    return run_hash_engine(std::move(inputs), buffer_size, workers,
                           std::move(backend));
}

}
