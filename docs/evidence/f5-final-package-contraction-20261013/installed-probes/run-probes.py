#!/usr/bin/env python3
"""F5 installed-header compile / link / ODR probes.

Runs against an install prefix produced by the B-01 v6 clean-room capture:

  * every installed header is compiled standalone (`-fsyntax-only`), so a
    header that only worked through a particular include order breaks here;
  * two translation units each include the whole retained public closure,
    instantiate the canonical request/result surface and take the address of
    two header-inline functions (`sluice::detail::uring_chunk_length`,
    `sluice::make_unexpected_void`); the probe links them together and compares
    those addresses and the `RequestId` type name at run time. Equal addresses
    show the inline entities from the two translation units resolved to one
    symbol. Weak-symbol folding means this probe cannot detect two divergent
    definitions that the linker merges; it detects visibility, linkage and
    identity failures;
  * the removed and de-installed headers are asserted absent from the prefix,
    and the same presence check is recorded against the before prefix as the
    negative control that makes the absence meaningful.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess

COMPILER = "/usr/bin/g++"
LIBS = ["lib/libsluice_async.a", "lib/libsluice_core.a"]
CLOSURE = [
    "sluice/async/async_io_context.hpp",
    "sluice/async/fiber.hpp",
    "sluice/async/fiber_ctx.hpp",
    "sluice/async/request.hpp",
    "sluice/async/request_scope.hpp",
    "sluice/async/stackful_io_host.hpp",
    "sluice/async/threadpool_backend.hpp",
    "sluice/async/uring_backend.hpp",
    "sluice/blocking/file.hpp",
    "sluice/detail/uring_submit.hpp",
    "sluice/file_resource.hpp",
]
REMOVED = [
    "sluice/async/condition.hpp",
    "sluice/async/semaphore.hpp",
    "sluice/async/async_queue.hpp",
    "sluice/async/future.hpp",
    "sluice/async/wait_policy.hpp",
    "sluice/async/select.hpp",
    "sluice/async/select_fwd.hpp",
    "sluice/async/event.hpp",
    "sluice/async/wait_queue.hpp",
    "sluice/async/timer_registration.hpp",
    "sluice/async/mutex.hpp",
    "sluice/async/lock_guard.hpp",
    "sluice/async/scheduler.hpp",
    "sluice/async/async_mutex.hpp",
    "sluice/async/async_rwlock.hpp",
    "sluice/async/wait_node.hpp",
    "sluice/async/thread_annotations.hpp",
    "sluice/async/detail/queue_item.hpp",
    "sluice/async/detail/queue_port.hpp",
    "sluice/async/detail/select_registration.hpp",
    "sluice/async/detail/select_port.hpp",
    "sluice/async/detail/mutex_test_seam.hpp",
    "sluice/async/detail/queue_test_seam.hpp",
    "sluice/async/detail/context_identity.hpp",
]


def odr_tu(suffix):
    includes = "\n".join("#include <%s>" % header for header in CLOSURE)
    return """%s

#include <string>
#include <typeinfo>

namespace {

sluice::async::Request<std::size_t> request_instantiation() { return {}; }

sluice::Result<std::size_t> result_instantiation() {
    return sluice::Result<std::size_t>(std::size_t{0});
}

} // namespace

extern "C" const char* odr_description_%s() {
    static const std::string text =
        std::string("RequestId=") + typeid(sluice::async::RequestId).name() +
        ",ready=" + std::to_string(request_instantiation().valid()) +
        ",result=" + std::to_string(result_instantiation().has_value());
    return text.c_str();
}

extern "C" const void* odr_chunk_entry_%s() {
    return reinterpret_cast<const void*>(&sluice::detail::uring_chunk_length);
}

extern "C" const void* odr_unexpected_entry_%s() {
    return reinterpret_cast<const void*>(&sluice::make_unexpected_void);
}
""" % (includes, suffix, suffix, suffix)


ODR_MAIN = """#include <cstdio>
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
    std::printf("a=%s\\nb=%s\\nchunk equal=%d\\nunexpected equal=%d\\n", odr_description_a(),
                odr_description_b(), int(chunk_equal), int(unexpected_equal));
    return (described_alike && chunk_equal && unexpected_equal) ? 0 : 1;
}
"""

parser = argparse.ArgumentParser()
parser.add_argument("--prefix", required=True)
parser.add_argument("--before-prefix")
parser.add_argument("--liburing", action="store_true")
parser.add_argument("--output", required=True)
parser.add_argument("--source-sha", default=None)
args = parser.parse_args()

output = pathlib.Path(args.output).resolve()
output.mkdir(parents=True, exist_ok=True)
prefix = pathlib.Path(args.prefix).resolve()
include = prefix / "include"
profile_flags = ["-DSLUICE_HAS_LIBURING"] if args.liburing else []
profile_libs = ["-luring"] if args.liburing else []


def run(argv, label):
    log = output / (label + ".log")
    completed = subprocess.run([str(x) for x in argv], capture_output=True, text=True,
                               check=False)
    log.write_text(completed.stdout + completed.stderr)
    return {"argv": [str(x) for x in argv], "exit_code": completed.returncode,
            "log": log.name, "log_sha256": hashlib.sha256(log.read_bytes()).hexdigest()}


headers = sorted(str(p.relative_to(prefix)) for p in include.rglob("*.hpp"))
standalone = []
for header in headers:
    label = "standalone-" + header.replace("/", "_")
    source = output / (label + ".cpp")
    source.write_text("#include <%s>\nint main() { return 0; }\n" % header[len("include/"):])
    record = run([COMPILER, "-std=c++20", *profile_flags, "-fsyntax-only",
                  "-I" + str(include), str(source)], label)
    standalone.append({"header": header, **record})
failed_standalone = [r for r in standalone if r["exit_code"] != 0]

sources = {"odr-a": odr_tu("a"), "odr-b": odr_tu("b"), "odr-main": ODR_MAIN}
objects = []
for name, text in sources.items():
    source = output / (name + ".cpp")
    source.write_text(text)
    object_file = output / (name + ".o")
    objects.append(object_file)
    record = run([COMPILER, "-std=c++20", *profile_flags, "-c", "-I" + str(include),
                  str(source), "-o", str(object_file)], "compile-" + name)
    if record["exit_code"] != 0:
        raise AssertionError(("odr probe failed to compile", name, record))
link = run([COMPILER, *(str(o) for o in objects), *(str(prefix / lib) for lib in LIBS),
            "-lpthread", *profile_libs, "-o", str(output / "odr-probe")], "link-odr")
if link["exit_code"] != 0:
    raise AssertionError(("odr probe failed to link", link))
odr_run = run([str(output / "odr-probe")], "run-odr")

present_removed = [h for h in REMOVED if (include / h).exists()]
control = None
if args.before_prefix:
    before = pathlib.Path(args.before_prefix).resolve()
    control = {"prefix": str(before),
               "installed_headers": len(list((before / "include").rglob("*.hpp"))),
               "present_removed_headers": [h for h in REMOVED
                                           if (before / "include" / h).exists()]}

report = {"prefix": str(prefix), "installed_headers": len(headers),
          "liburing": bool(args.liburing), "source_sha": args.source_sha,
          "standalone_failures": failed_standalone, "odr_link": link, "odr_run": odr_run,
          "present_removed_headers": present_removed, "control_prefix": control,
          "standalone": standalone}
(output / "probes.json").write_text(json.dumps(report, indent=2) + "\n")
assert not failed_standalone, failed_standalone
assert odr_run["exit_code"] == 0, odr_run
assert not present_removed, present_removed
print(json.dumps({"prefix": str(prefix), "headers": len(headers),
                  "standalone_failures": 0, "odr": "PASS", "removed_present": 0,
                  "source_sha": args.source_sha, "control": control}))
