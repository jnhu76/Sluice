#!/usr/bin/env python3
"""Fresh exact-SHA dual-profile facade contraction evidence; no adoption implied."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import tarfile

parser = argparse.ArgumentParser()
parser.add_argument("--repo", required=True)
parser.add_argument("--before", required=True)
parser.add_argument("--after", required=True)
parser.add_argument("--output", required=True)
args = parser.parse_args()
repo = pathlib.Path(args.repo).resolve()
output = pathlib.Path(args.output).resolve()
output.mkdir(parents=True, exist_ok=False)
xmake = "/home/jnhu/.local/bin/xmake"
environment = {"PATH": "/usr/bin:/bin", "HOME": "/home/jnhu", "LANG": "C.UTF-8"}
headers = ["application_runtime", "await_op_helpers", "task_result", "group", "evented_wait_policy"]
sources = ["application_runtime", "await_op_helpers", "group"]
commands = []
summary = {}
def git(*argv):
    return subprocess.check_output(["git", "-C", str(repo), *argv])
def command(argv, label, directory, expected=0):
    result = subprocess.run([str(x) for x in argv], cwd=directory, env=environment,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (directory / (label + ".log")).write_text(result.stdout)
    commands.append({"argv": [str(x) for x in argv], "cwd": str(directory),
                     "returncode": result.returncode, "expected": expected})
    if expected == "failure":
        assert result.returncode != 0, label
    else:
        assert result.returncode == expected, (label, result.returncode, result.stdout[-3000:])
    return result.stdout
inventory = output / "inventory.lua"
inventory.write_text("""function main(output)
    import("core.project.config")
    config.load()
    import("core.project.project")
    import("core.base.json")
    local out = {}
    for _, name in ipairs({"sluice_core", "sluice_async"}) do
        local t = project.target(name)
        out[name] = {sources = t:sourcefiles(), objects = t:objectfiles(), target = t:targetfile()}
    end
    io.writefile(output, json.encode(out))
end
""")
toolchain = output / "toolchain"
toolchain.mkdir()
for executable in ("/usr/bin/g++", "/usr/bin/clang++", "/usr/bin/ar", xmake):
    command([executable, "--version"], pathlib.Path(executable).name, toolchain)
command(["uname", "-a"], "kernel", toolchain)
sha_before = git("rev-parse", args.before).decode().strip()
sha_after = git("rev-parse", args.after).decode().strip()
for stage, sha in (("before", sha_before), ("after", sha_after)):
    snapshot = output / (stage + "-source.tar")
    snapshot.write_bytes(git("archive", sha))
    for ring in ("n", "y"):
        root = output / (stage + "-" + ring)
        root.mkdir()
        with tarfile.open(snapshot) as archive:
            archive.extractall(root, filter="data")
        prefix = output / (stage + "-installed-" + ring)
        evidence = output / (stage + "-evidence-" + ring)
        evidence.mkdir()
        command([xmake, "f", "-P", root, "-y", "-m", "debug", "--liburing=" + ring,
                 "--ccache=n", "-o", root / "build"], "config", evidence)
        command([xmake, "build", "-P", root, "-v", "-j", "4", "sluice_async"], "build", evidence)
        for library in ("sluice_async", "sluice_core"):
            command([xmake, "install", "-P", root, "-o", prefix, library], "install-" + library, evidence)
        command([xmake, "lua", "-P", root, inventory, evidence / "resolved.json"], "resolved", evidence)
        resolved = json.loads((evidence / "resolved.json").read_text())
        library_objects = sum(len(x["objects"]) for x in resolved.values())
        assert all(pathlib.Path(p).is_file() for x in resolved.values() for p in x["objects"])
        installed = sorted(str(p.relative_to(prefix / "include")) for p in (prefix / "include").rglob("*.hpp"))
        (evidence / "installed-headers.txt").write_text("\n".join(installed) + "\n")
        for library in ("core", "async"):
            archive = prefix / "lib" / ("libsluice_" + library + ".a")
            command(["ar", "t", archive], "members-" + library, evidence)
            command(["nm", "-C", "--defined-only", archive], "defined-" + library, evidence)
            command(["nm", "-C", "--undefined-only", archive], "undefined-" + library, evidence)
        async_members = (evidence / "members-async.log").read_text().splitlines()
        if stage == "after":
            for name in sources:
                assert all(not p.endswith("/" + name + ".cpp") for x in resolved.values() for p in x["sources"])
                assert all(name + ".cpp.o" not in p for x in resolved.values() for p in x["objects"])
                assert name + ".cpp.o" not in async_members
            for name in headers:
                assert not (root / ("include/sluice/async/" + name + ".hpp")).exists()
                assert not (prefix / ("include/sluice/async/" + name + ".hpp")).exists()
            definitions = (evidence / "defined-async.log").read_text()
            undefined = (evidence / "undefined-async.log").read_text()
            removed = r"sluice::async::(ApplicationRuntime|RuntimeBuilder|RuntimeTaskContext|Group)::|sluice::async::await_(take|drain|read_once|read_fill|write_exact)\("
            assert re.search(removed, definitions) is None
            assert re.search(removed, undefined) is None
        clean = output / (stage + "-consumers-" + ring)
        clean.mkdir()
        flags = ["/usr/bin/g++", "-std=c++20", "-g", "-I" + str(prefix / "include")]
        if ring == "y":
            flags.append("-DSLUICE_HAS_LIBURING")
        libraries = [str(prefix / "lib/libsluice_async.a"), str(prefix / "lib/libsluice_core.a"), "-pthread"]
        if ring == "y":
            libraries.append("-luring")
        closures = {}
        for name in ("request", "request_scope", "async_io_context", "scheduler", "stackful_io_host", "completion", "fiber", "fiber_ctx", "future", "wait_policy"):
            probe = clean / (name + "-header.cpp")
            probe.write_text("#include <sluice/async/" + name + ".hpp>\nint main() {}\n")
            dep = command(flags + ["-MM", probe], "closure-" + name, evidence)
            closures[name] = sorted(set(re.findall(r"sluice/[\w/.-]+\.hpp", dep)))
            command(flags + ["-fsyntax-only", probe], "header-" + name, evidence)
        (evidence / "closures.json").write_text(json.dumps(closures, indent=2) + "\n")
        for name in headers:
            probe = clean / (name + "-negative.cpp")
            probe.write_text("#include <sluice/async/" + name + ".hpp>\nint main() {}\n")
            text = command(flags + ["-fsyntax-only", probe], "removed-header-" + name, evidence,
                           0 if stage == "before" else "failure")
            if stage == "after":
                assert "No such file or directory" in text
        for name in ("w01_direct", "w02_pipeline", "w03_external_loop", "w04_stackful_candidate",
                     "contract_request_lifetime", "contract_admission", "contract_cancel_effect",
                     "contract_observer", "contract_progress", "contract_shutdown",
                     "contract_durability", "contract_backend_availability"):
            probe = clean / (name + ".cpp")
            probe.write_bytes((root / ("tests/package/consumers/" + name + ".cpp")).read_bytes())
            binary = clean / name
            link = [str(prefix / "lib/libsluice_core.a")] if name == "w01_direct" else libraries
            command(flags + [probe, *link, "-Wl,-Map=" + str(evidence / (name + ".map")), "-o", binary],
                    "link-" + name, evidence)
            if name == "w03_external_loop":
                command([binary, "threadpool"], "run-w03-threadpool", evidence)
                command([binary, "uring"], "run-w03-uring", evidence, 0 if ring == "y" else 2)
            elif name == "w04_stackful_candidate":
                command([binary], "run-w04-candidate", evidence, 0 if ring == "y" else 2)
            else:
                command([binary], "run-" + name, evidence)
        retained = clean / "retained-compat.cpp"
        retained.write_text("""#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/future.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/stackful_io_host.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>
#include <array>
int main() {
    using namespace sluice::async;
    Future<void> future;
    future.complete_with(sluice::Result<void>{});
    if (!future.await()) return 1;
    AsyncIoContext io(std::make_unique<ThreadPoolBackend>());
    {
        Scheduler scheduler(io);
        std::array<std::byte, 65536> stack{};
        Fiber fiber([](Fiber&) {});
        if (!scheduler.init_fiber(fiber, stack.data(), stack.size())) return 2;
        scheduler.spawn(fiber);
        scheduler.run(1);
        if (fiber.state() != FiberState::done) return 3;
    }
    auto host = StackfulIoHost::create(io, {});
    if (!host) return 4;
    Completion<std::size_t> completion;
    return completion.idle() ? 0 : 5;
}
""")
        command(flags + [retained, *libraries, "-o", clean / "retained-compat"], "link-retained-compat", evidence)
        command([clean / "retained-compat"], "run-retained-compat", evidence)
        command(["nm", "-C", clean / "retained-compat"], "symbols-retained-compat", evidence)
        old = clean / "old-headers" / "sluice" / "async"
        old.mkdir(parents=True)
        for name in headers:
            (old / (name + ".hpp")).write_bytes(git("show", sha_before + ":include/sluice/async/" + name + ".hpp"))
        old_flags = flags + ["-I" + str(clean / "old-headers")]
        for name, body in {
            "runtime": "#include <sluice/async/application_runtime.hpp>\nauto volatile p = &sluice::async::RuntimeBuilder::build;\nint main() { return p == nullptr; }\n",
            "helper": "#include <sluice/async/await_op_helpers.hpp>\nauto* volatile p = &sluice::async::await_read_fill;\nint main() { return p == nullptr; }\n",
            "group-threaded": "#include <sluice/async/group.hpp>\nint main() { sluice::async::Group g; g.async([](sluice::async::CancelToken&) {}); g.await(); }\n",
        }.items():
            probe = clean / (name + "-old-link.cpp")
            probe.write_text(body)
            text = command(old_flags + [probe, *libraries, "-o", clean / (name + "-old-link")],
                           "old-link-" + name, evidence, 0 if stage == "before" else "failure")
            if stage == "after":
                assert "undefined reference" in text
            else:
                command([clean / (name + "-old-link")], "old-run-" + name, evidence)
        files = {str(p.relative_to(prefix)): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in prefix.rglob("*") if p.is_file()}
        package = output / (stage + "-package-" + ring + ".tar.gz")
        with tarfile.open(package, "w:gz") as archive:
            archive.add(prefix, arcname="prefix")
        summary[stage + "-" + ring] = {
            "source_sha": sha, "source_archive_sha256": hashlib.sha256(snapshot.read_bytes()).hexdigest(),
            "library_objects": library_objects, "async_archive_members": len(async_members),
            "installed_headers": len(installed), "files": files,
            "package_path": str(package), "package_sha256": hashlib.sha256(package.read_bytes()).hexdigest(),
            "real_ring": "PASS" if ring == "y" else "UNAVAILABLE"}
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
for ring in ("n", "y"):
    before_closure = json.loads((output / ("before-evidence-" + ring) / "closures.json").read_text())
    after_closure = json.loads((output / ("after-evidence-" + ring) / "closures.json").read_text())
    assert before_closure == after_closure, "retained public dependency closure changed"
    assert [summary["before-" + ring][k] for k in ("library_objects", "async_archive_members", "installed_headers")] == [49, 36, 78]
    assert [summary["after-" + ring][k] for k in ("library_objects", "async_archive_members", "installed_headers")] == [46, 33, 73]
print("CANDIDATE_FRESH_PACKAGE_PASS", sha_before, sha_after)
