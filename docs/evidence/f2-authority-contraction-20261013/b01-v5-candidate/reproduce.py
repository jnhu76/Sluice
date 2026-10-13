#!/usr/bin/env python3
"""F2 (#402) B-01 v5 dual-profile package capture; no adoption implied.

Same clean-room shape as the B-01 v4 (F3) candidate: exact-SHA Git archive
sources, their own .xmake/build/prefix, compiler caching disabled, container
compiled against the installed prefix only. The F2 contraction removes seven
installed headers and three compiled sources, so this capture records the
before/after delta instead of reproducing the unchanged F1/F3 baseline.

Gates asserted here:
  - removed headers are absent from the source tree and the installed prefix;
  - removed sources/objects/archive members are absent after;
  - no removed definition or unresolved reference survives in the archives;
  - every retained public header's dependency closure loses only removed
    headers and gains none (no new dependency introduced by the contraction);
  - every retained consumer (W-01/W-02/W-03, the contract consumers, the
    retained Scheduler/Fiber/host/Future set) links and runs from the prefix;
  - a consumer written against the removed entry points still compiles against
    the old headers but fails to link against the new library.
"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
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

# F2 removed installed headers, relative to include/.
removed_headers = [
    "sluice/async/completion.hpp",
    "sluice/async/request_handle.hpp",
    "sluice/async/batch.hpp",
    "sluice/async/op_helpers.hpp",
    "sluice/async/detail/request_arena.hpp",
    "sluice/async/detail/request_slot.hpp",
    "sluice/async/detail/submit_transaction.hpp",
]
# F2 retired compiled sources, relative to the repository root.
removed_sources = [
    "src/async/request_handle.cpp",
    "src/async/batch.cpp",
    "src/async/op_helpers.cpp",
]
# Retained public headers whose closure the contraction must not widen.
retained_closures = [
    "request",
    "request_scope",
    "async_io_context",
    "scheduler",
    "stackful_io_host",
    "fiber",
    "fiber_ctx",
    "future",
    "wait_policy",
]

commands = []
summary = {}


def git(*argv):
    return subprocess.check_output(["git", "-C", str(repo), *argv])


def command(argv, label, directory, expected=0):
    workdir = pathlib.Path(argv[argv.index("-P") + 1]) if "-P" in argv else directory
    result = subprocess.run([str(x) for x in argv], cwd=workdir, env=environment,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (directory / (label + ".log")).write_text(result.stdout)
    commands.append({"argv": [str(x) for x in argv], "cwd": str(workdir),
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
            command([xmake, "install", "-P", root, "-o", prefix, library],
                    "install-" + library, evidence)
        command([xmake, "lua", "-P", root, inventory, evidence / "resolved.json"], "resolved",
                evidence)
        resolved = json.loads((evidence / "resolved.json").read_text())
        library_objects = sum(len(x["objects"]) for x in resolved.values())
        assert all((root / p).is_file() for x in resolved.values() for p in x["objects"])
        installed = sorted(str(p.relative_to(prefix / "include"))
                           for p in (prefix / "include").rglob("*.hpp"))
        (evidence / "installed-headers.txt").write_text("\n".join(installed) + "\n")
        for library in ("core", "async"):
            archive = prefix / "lib" / ("libsluice_" + library + ".a")
            command(["ar", "t", archive], "members-" + library, evidence)
            command(["nm", "-C", "--defined-only", archive], "defined-" + library, evidence)
            command(["nm", "-C", "--undefined-only", archive], "undefined-" + library, evidence)
        async_members = (evidence / "members-async.log").read_text().splitlines()
        removed_defs = (r"sluice::async::Batch::|sluice::async::read_all\(|"
                        r"sluice::async::write_all\(|sluice::async::sync_data_all\(|"
                        r"sluice::async::sync_all_all\(|"
                        r"sluice::async::AsyncIoContext::submit_read_request\(|"
                        r"sluice::async::AsyncIoContext::request_state\(|"
                        r"sluice::async::AsyncIoContext::identity_of\(|"
                        r"sluice::async::detail::RequestArena::|"
                        r"sluice::async::detail::completion_binding_reset_fail_fast\(")
        definitions = (evidence / "defined-async.log").read_text()
        undefined = (evidence / "undefined-async.log").read_text()
        if stage == "before":
            # Positive controls: the same predicates that must hold after the
            # contraction must be observably false before it, so a typo in a
            # path or pattern cannot make the after-side assertions vacuous.
            for name in removed_sources:
                assert any(name in x["sources"] for x in resolved.values()), (name, resolved)
                assert any(pathlib.Path(name).name + ".o" in p for x in resolved.values()
                           for p in x["objects"]), name
            assert pathlib.Path(removed_sources[0]).name + ".o" in async_members, async_members
            for name in removed_headers:
                assert (prefix / "include" / name).exists(), name
                assert name in installed, name
            assert re.search(removed_defs, definitions), \
                "control: the removed-definition pattern matches nothing in the before archive"
        else:
            for name in removed_sources:
                assert all(name not in x["sources"] for x in resolved.values()), name
                assert all(not p.endswith("/" + pathlib.Path(name).name) for x in resolved.values()
                           for p in x["sources"]), name
                assert all(pathlib.Path(name).name + ".o" not in p for x in resolved.values()
                           for p in x["objects"]), name
                assert pathlib.Path(name).name + ".o" not in async_members, name
            for name in removed_headers:
                assert not (root / "include" / name).exists(), name
                assert not (prefix / "include" / name).exists(), name
                assert name not in installed, name
            assert re.search(removed_defs, definitions) is None, "removed definition survived"
            assert re.search(removed_defs, undefined) is None, "removed reference survived"
        clean = output / (stage + "-consumers-" + ring)
        clean.mkdir()
        flags = ["/usr/bin/g++", "-std=c++20", "-g", "-I" + str(prefix / "include")]
        if ring == "y":
            flags.append("-DSLUICE_HAS_LIBURING")
        libraries = [str(prefix / "lib/libsluice_async.a"),
                     str(prefix / "lib/libsluice_core.a"), "-pthread"]
        if ring == "y":
            libraries.append("-luring")
        closures = {}
        for name in retained_closures:
            probe = clean / (name + "-header.cpp")
            probe.write_text("#include <sluice/async/" + name + ".hpp>\nint main() {}\n")
            dep = command(flags + ["-MM", probe], "closure-" + name, evidence)
            closures[name] = sorted(set(re.findall(r"sluice/[\w/.-]+\.hpp", dep)))
            command(flags + ["-fsyntax-only", probe], "header-" + name, evidence)
        (evidence / "closures.json").write_text(json.dumps(closures, indent=2) + "\n")
        for name in removed_headers:
            probe = clean / (pathlib.Path(name).name + "-negative.cpp")
            probe.write_text("#include <" + name + ">\nint main() {}\n")
            text = command(flags + ["-fsyntax-only", probe],
                           "removed-header-" + pathlib.Path(name).stem, evidence,
                           0 if stage == "before" else "failure")
            if stage == "after":
                assert "No such file or directory" in text, name
        for name in ("w01_direct", "w02_pipeline", "w03_external_loop", "w04_stackful_candidate",
                     "contract_request_lifetime", "contract_admission", "contract_cancel_effect",
                     "contract_observer", "contract_progress", "contract_shutdown",
                     "contract_durability", "contract_backend_availability"):
            probe = clean / (name + ".cpp")
            probe.write_bytes((root / ("tests/package/consumers/" + name + ".cpp")).read_bytes())
            binary = clean / name
            link = [str(prefix / "lib/libsluice_core.a")] if name == "w01_direct" else libraries
            command(flags + [probe, *link, "-Wl,-Map=" + str(evidence / (name + ".map")),
                             "-o", binary], "link-" + name, evidence)
            if name == "w03_external_loop":
                command([binary, "threadpool"], "run-w03-threadpool", evidence)
                command([binary, "uring"], "run-w03-uring", evidence, 0 if ring == "y" else 2)
            elif name == "w04_stackful_candidate":
                command([binary], "run-w04-candidate", evidence, 0 if ring == "y" else 2)
            else:
                command([binary], "run-" + name, evidence)
        retained = clean / "retained-compat.cpp"
        retained.write_text("""#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/future.hpp>
#include <sluice/async/request.hpp>
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
    RequestId id;
    return id.valid() ? 5 : 0;
}
""")
        command(flags + [retained, *libraries, "-o", clean / "retained-compat"],
                "link-retained-compat", evidence)
        command([clean / "retained-compat"], "run-retained-compat", evidence)
        command(["nm", "-C", clean / "retained-compat"], "symbols-retained-compat", evidence)
        old = clean / "old-headers"
        # The removed headers are only self-consistent against the whole old
        # header set (they include detail/request_arena.hpp and the fail-fast
        # declarations F2 retired), so the probe compiles against the complete
        # before-tree include/ - placed first on the search path so every
        # nested include resolves inside the old set - and links against the
        # new library: the link step is the assertion.
        old.mkdir()
        shutil.copytree(root.parent / "before-n" / "include", old, dirs_exist_ok=True)
        old_flags = list(flags)
        old_flags.insert(old_flags.index("-I" + str(prefix / "include")),
                         "-I" + str(old))
        old_entry_points = {
            "completion-submit":
                "#include <sluice/async/async_io_context.hpp>\n"
                "#include <sluice/async/completion.hpp>\n"
                "using OldSubmit = sluice::Result<void> (sluice::async::AsyncIoContext::*)("
                "sluice::async::ReadOp, sluice::async::Completion<std::size_t>&);\n"
                "int main() { OldSubmit p = &sluice::async::AsyncIoContext::submit_read; "
                "return p == nullptr; }\n",
            "completion-submit-request":
                "#include <sluice/async/async_io_context.hpp>\n"
                "#include <sluice/async/completion.hpp>\n"
                "#include <sluice/async/request_handle.hpp>\n"
                "using OldRequest = sluice::Result<sluice::async::RequestHandle> "
                "(sluice::async::AsyncIoContext::*)("
                "sluice::async::ReadOp, sluice::async::Completion<std::size_t>&);\n"
                "int main() { OldRequest p = "
                "&sluice::async::AsyncIoContext::submit_read_request; "
                "return p == nullptr; }\n",
            "identity-state":
                "#include <sluice/async/async_io_context.hpp>\n"
                "#include <sluice/async/request_handle.hpp>\n"
                "using OldState = sluice::Result<sluice::async::RequestHandleState> "
                "(sluice::async::AsyncIoContext::*)(const sluice::async::RequestHandle&) const;\n"
                "int main() { OldState p = &sluice::async::AsyncIoContext::request_state; "
                "return p == nullptr; }\n",
            "batch-await":
                "#include <sluice/async/batch.hpp>\n"
                "using OldBatch = sluice::Result<std::size_t> "
                "(sluice::async::Batch::*)(sluice::async::AsyncIoContext&);\n"
                "int main() { OldBatch p = &sluice::async::Batch::await_one; "
                "return p == nullptr; }\n",
            "op-helpers":
                "#include <sluice/async/op_helpers.hpp>\n"
                "using OldReadAll = sluice::Result<std::size_t> (*)(sluice::async::AsyncIoContext&, "
                "const sluice::async::NativeFileRef&, std::span<std::byte>, std::uint64_t);\n"
                "int main() { OldReadAll p = &sluice::async::read_all; "
                "return p == nullptr; }\n",
        }
        for name, body in old_entry_points.items():
            probe = clean / (name + "-old-entry.cpp")
            probe.write_text(body)
            text = command(old_flags + [probe, *libraries, "-o", clean / (name + "-old-entry")],
                           "old-entry-" + name, evidence, 0 if stage == "before" else "failure")
            if stage == "after":
                assert "undefined reference" in text, name
            else:
                command([clean / (name + "-old-entry")], "old-entry-run-" + name, evidence)
        files = {str(p.relative_to(prefix)): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in prefix.rglob("*") if p.is_file()}
        package = output / (stage + "-package-" + ring + ".tar.gz")
        with tarfile.open(package, "w:gz") as archive:
            archive.add(prefix, arcname="prefix")
        summary[stage + "-" + ring] = {
            "source_sha": sha,
            "source_archive_sha256": hashlib.sha256(snapshot.read_bytes()).hexdigest(),
            "library_objects": library_objects,
            "async_archive_members": len(async_members),
            "installed_headers": len(installed),
            "installed_header_list": installed,
            "files": files,
            "package_path": str(package),
            "package_sha256": hashlib.sha256(package.read_bytes()).hexdigest(),
            "real_ring": "PASS" if ring == "y" else "UNAVAILABLE",
        }
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

removed_header_names = set(removed_headers)
removed_basenames = {pathlib.Path(h).name for h in removed_headers}
for ring in ("n", "y"):
    before_closure = json.loads((output / ("before-evidence-" + ring) / "closures.json").read_text())
    after_closure = json.loads((output / ("after-evidence-" + ring) / "closures.json").read_text())
    assert before_closure.keys() == after_closure.keys()
    for name in retained_closures:
        before_set = set(before_closure[name])
        after_set = set(after_closure[name])
        appeared = after_set - before_set
        assert not appeared, ("contraction introduced a dependency", name, sorted(appeared))
        vanished = before_set - after_set
        assert all(pathlib.Path(v).name in removed_basenames for v in vanished), \
            ("closure lost a retained header", name, sorted(vanished))
    counts_before = [summary["before-" + ring][k] for k in
                     ("library_objects", "async_archive_members", "installed_headers")]
    counts_after = [summary["after-" + ring][k] for k in
                    ("library_objects", "async_archive_members", "installed_headers")]
    # 0ed16588 (the F3+F4 closed master) installs 2 core + 33 async objects and
    # 56 headers in every ring profile; the F4 post-merge master capture records
    # the same 2/33 archive split and 4x56 installed headers, so these counts are
    # not this harness's own invention. F2 retires three compiled sources (one
    # object and one archive member each) and seven installed headers.
    assert counts_before == [35, 33, 56], counts_before
    assert counts_after == [35 - len(removed_sources), 33 - len(removed_sources),
                            56 - len(removed_headers)], counts_after
    for other in ("n", "y"):
        if other == ring:
            continue
        assert [summary["before-" + other][k] for k in
                ("library_objects", "async_archive_members", "installed_headers")] == counts_before
        assert [summary["after-" + other][k] for k in
                ("library_objects", "async_archive_members", "installed_headers")] == counts_after
print("F2_B01_V5_CANDIDATE_FRESH_PACKAGE_PASS", sha_before, sha_after)
