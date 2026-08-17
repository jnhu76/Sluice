// sluice-copy CLI argument parsing (small internal module).
//
// Extracted from main.cpp so the strict integer parsing and resource-limit
// rules can be unit-tested directly (the copy_task.cpp tests already follow
// the "app source compiled into the test target" pattern). Not an installed
// public header; the app target and its tests are the only consumers.
#pragma once

#include "copy_task.hpp"  // SyncPolicy, kMaxWorkers

#include <sluice/error.hpp>

#include <cstddef>
#include <string>

namespace sluice_copy::cli {

struct CliArgs {
    std::string src;
    std::string dst;
    std::size_t buffer_size = 1 << 20;  // 1 MiB default
    std::size_t pipeline_depth = 1;     // Version A default; >1 enables Version B
    unsigned workers = 1;
    SyncPolicy sync = SyncPolicy::none;
    bool atomic = true;                 // Version C default: temp file + rename
    bool help = false;
};

// Print usage to stderr. Returns the usage-error exit code (1).
int usage(const char* prog);

// Strict unsigned decimal parser shared by --buffer-size and --pipeline-depth.
//
// Accepts ONLY ASCII digits. Rejects: empty strings, leading '-' or '+',
// trailing junk ("123abc", "1MiB"), values that do not fit std::size_t
// (checked before any conversion — no silent truncation), and zero. errno is
// deliberately not consulted: this is a pure digit scanner, not strtoull, so
// strtoull's sign/suffix semantics cannot leak in.
bool parse_size(const char* s, std::size_t& out);

// Worker-count parser. Applies the size_t -> unsigned narrowing check BEFORE
// the conversion and then the app-level kMaxWorkers cap, so a legal-but-
// extreme count cannot trigger resource exhaustion downstream.
bool parse_workers(const char* s, unsigned& out);

bool parse_sync(const char* s, SyncPolicy& out);

// Parse argv into `args`. Returns 0 on success (args filled), or a non-zero
// exit code on a usage error (message already printed).
int parse_args(int argc, char** argv, CliArgs& args);

// Stable name for an IoError code (diagnostics only).
const char* code_name(sluice::IoError::Code c);

}  // namespace sluice_copy::cli
