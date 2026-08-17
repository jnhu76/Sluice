// sluice-hash hashing engine implementation.
//
// Shape follows sluice-copy's copy_task.cpp (the audited in-repository
// pattern): one Runtime task drives positional reads + await_completion; the
// terminal outcome is published through an app-owned slot; the main thread
// waits for the task to finish BEFORE request_stop/drain/join, so a
// run-to-completion workload is never aborted by root cancellation.
#include "hash_task.hpp"

#include "sha256.hpp"

#include <sluice/async/threadpool_backend.hpp>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace sluice_hash {

namespace {

using namespace sluice::async;
using sluice::IoError;

struct HashTask {
    std::vector<HashInput> inputs;
    std::size_t buffer_size;
    std::vector<std::uint8_t> buffer;
    std::vector<FileHash> results;

    std::mutex& mtx;
    bool& done;
    std::condition_variable& done_cv;

    void publish() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            done = true;
        }
        done_cv.notify_all();
    }

    // Hash one file: submit positional reads into the shared buffer, await
    // each, feed the streaming hasher. Short reads just advance the offset
    // (any n > 0 is progress); n == 0 is EOF. Writes FileHash into results.
    void hash_one(RuntimeTaskContext& ctx, const HashInput& in) {
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
            auto sr = ctx.submit_read(
                ReadOp{in.fd,
                       reinterpret_cast<std::byte*>(buffer.data()),
                       buffer.size(), offset},
                rc);
            if (!sr.has_value()) {
                out.error = sr.error();
                break;
            }
            auto wr = ctx.await_completion(rc);
            if (!wr.has_value()) {
                out.error = wr.error();
                break;
            }
            auto rr = rc.result();
            rc.reset();
            if (!rr.has_value()) {
                out.error = rr.error();
                break;
            }
            std::size_t n = rr.value();
            if (n == 0) {
                // EOF: finalize.
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

    void operator()(RuntimeTaskContext& ctx) {
        // App-local exception boundary (same rationale as copy_task): the
        // Runtime swallows task exceptions at the Group boundary, so any
        // escaping exception must be translated into per-file errors before
        // publish() or the caller's done_cv wait would hang forever. Every
        // input gets exactly one result entry on every path.
        try {
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                if (ctx.cancel_token().is_requested()) {
                    // Cancellation stops the WORK: remaining files are marked
                    // canceled without any I/O.
                    FileHash out;
                    out.path = inputs[i].path;
                    out.error = IoError{IoError::Code::canceled};
                    results.push_back(std::move(out));
                    continue;
                }
                hash_one(ctx, inputs[i]);
            }
        } catch (...) {
            for (std::size_t i = results.size(); i < inputs.size(); ++i) {
                FileHash out;
                out.path = inputs[i].path;
                out.error = IoError{IoError::Code::backend_error};
                results.push_back(std::move(out));
            }
        }
        publish();
    }
};

std::vector<FileHash> run_hash_engine(std::vector<HashInput> inputs,
                                      std::size_t buffer_size, unsigned workers,
                                      std::unique_ptr<AsyncBackend> backend) {
    // Argument validation BEFORE any allocation or Runtime build.
    if (buffer_size < kMinBufferSize || buffer_size > kMaxBufferSize ||
        workers == 0 || workers > kMaxWorkers || !backend) {
        std::vector<FileHash> out;
        out.reserve(inputs.size());
        for (auto& in : inputs) {
            FileHash f;
            f.path = in.path;
            f.error = IoError{IoError::Code::invalid_state};
            out.push_back(std::move(f));
        }
        return out;
    }

    // Allocate the single reusable buffer BEFORE the Runtime is built so an
    // allocation failure cannot strand a started Runtime.
    std::vector<std::uint8_t> buffer;
    try {
        buffer.assign(buffer_size, 0);
    } catch (const std::bad_alloc&) {
        std::vector<FileHash> out;
        for (auto& in : inputs) {
            FileHash f;
            f.path = in.path;
            f.error = IoError{IoError::Code::no_space};
            out.push_back(std::move(f));
        }
        return out;
    }

    RuntimeBuilder builder;
    builder.backend(std::move(backend));
    builder.workers(workers);

    std::unique_ptr<ApplicationRuntime> rt;
    try {
        auto build_r = builder.build();
        if (!build_r.has_value()) {
            std::vector<FileHash> out;
            for (auto& in : inputs) {
                FileHash f;
                f.path = in.path;
                f.error = build_r.error();
                out.push_back(std::move(f));
            }
            return out;
        }
        rt = std::move(build_r.value());
        auto start_r = rt->start();
        if (!start_r.has_value()) {
            std::vector<FileHash> out;
            for (auto& in : inputs) {
                FileHash f;
                f.path = in.path;
                f.error = start_r.error();
                out.push_back(std::move(f));
            }
            return out;
        }
    } catch (...) {
        if (rt) (void)rt->shutdown();  // correct in every state
        std::vector<FileHash> out;
        for (auto& in : inputs) {
            FileHash f;
            f.path = in.path;
            f.error = IoError{IoError::Code::no_space};
            out.push_back(std::move(f));
        }
        return out;
    }

    std::mutex mtx;
    std::condition_variable done_cv;
    bool done = false;

    HashTask task{std::move(inputs), buffer_size, std::move(buffer), {},
                  mtx, done, done_cv};

    auto sub_r = rt->submit(std::ref(task));
    if (!sub_r.has_value()) {
        (void)rt->shutdown();
        std::vector<FileHash> out;
        for (auto& in : task.inputs) {
            FileHash f;
            f.path = in.path;
            f.error = sub_r.error();
            out.push_back(std::move(f));
        }
        return out;
    }

    // Wait for the task to publish (run-to-completion; the driver keeps
    // reaping I/O while we wait). Only then stop/drain/join.
    {
        std::unique_lock<std::mutex> wlk(mtx);
        done_cv.wait(wlk, [&] { return done; });
    }
    rt->request_stop();
    (void)rt->drain();
    (void)rt->join();

    return std::move(task.results);
}

}  // namespace

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

}  // namespace sluice_hash
