#!/usr/bin/env python3
"""Supplement actual retained Completion submit/result/reset against captured prefixes."""
import json
import pathlib
import subprocess
import sys
base = pathlib.Path(sys.argv[1]).resolve()
rows = []
body = """#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>
int main() {
    using namespace sluice::async;
    auto file = sluice::File::open("/dev/null", {});
    if (!file) return 1;
    AsyncIoContext io(std::make_unique<ThreadPoolBackend>());
    Completion<std::size_t> completion;
    if (!io.submit_read(ReadOp{NativeFileRef{file.value()}, nullptr, 0, 0}, completion)) return 2;
    if (!completion.ready()) return 3;
    auto result = completion.result();
    completion.reset();
    return result && result.value() == 0 && io.outstanding() == 0 ? 0 : 4;
}
"""
for stage in ("before", "after"):
    for ring in ("n", "y"):
        prefix = base / (stage + "-installed-" + ring)
        clean = base / (stage + "-consumers-" + ring)
        evidence = base / (stage + "-evidence-" + ring)
        source = clean / "completion-submit.cpp"
        source.write_text(body)
        binary = clean / "completion-submit"
        argv = ["/usr/bin/g++", "-std=c++20", "-I" + str(prefix / "include")]
        if ring == "y":
            argv.append("-DSLUICE_HAS_LIBURING")
        argv += [str(source), str(prefix / "lib/libsluice_async.a"),
                 str(prefix / "lib/libsluice_core.a"), "-pthread"]
        if ring == "y":
            argv.append("-luring")
        argv += ["-o", str(binary)]
        for label, command in (("link-completion-submit", argv), ("run-completion-submit", [str(binary)])):
            result = subprocess.run(command, cwd=clean, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (evidence / (label + ".log")).write_text(result.stdout)
            rows.append({"stage": stage, "profile": ring, "argv": command, "returncode": result.returncode})
            assert result.returncode == 0, result.stdout
(base / "completion-submit-commands.json").write_text(json.dumps(rows, indent=2) + "\n")
print("RETAINED_COMPLETION_SUBMIT_RESULT_RESET_PASS")
