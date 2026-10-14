#!/usr/bin/env python3
"""F5 (#402) B-01 v6 dual-profile package capture; no adoption implied.

Same clean-room shape as the B-01 v5 (F2) capture: exact-SHA Git archive
sources, their own .xmake/build/prefix, compiler caching disabled, consumers
compiled against the installed prefix only. The F5 contraction retires the
legacy scheduler/select/sync runtime (23 installed headers, 17 compiled
sources), de-installs the library-private detail/context_identity.hpp and
removes the never-installed pre-v1 experimental uring tree.

Gates asserted here:
  - removed headers/sources/objects/archive members are absent after, with
    positive before-side controls (the same predicates must fail before);
  - every symbol defined by a removed archive member is gone and unreferenced;
  - detail/context_identity.hpp stays in the source tree but leaves the prefix;
  - the experimental tree leaves the source tree;
  - every retained public header's dependency closure loses only removed
    headers and gains none;
  - retained consumers (W-01/W-02/W-03, contract families, the W-04 candidate
    host) link and run from the fresh prefix in both ring profiles;
  - consumers written against the removed entry points still compile against
    the old headers but fail to link against the new library.
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

# F5 tree-removed installed headers, relative to include/.
removed_headers = [
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
]
# Library-private substrate: stays in the tree, leaves the installed prefix.
deinstalled_headers = ["sluice/async/detail/context_identity.hpp"]
# F5 retired compiled sources, relative to the repository root.
removed_sources = [
    "src/async/wait_policy.cpp",
    "src/async/scheduler.cpp",
    "src/async/scheduler_condition.cpp",
    "src/async/scheduler_event.cpp",
    "src/async/scheduler_fe2_test_seam.cpp",
    "src/async/scheduler_mutex.cpp",
    "src/async/scheduler_park_wake.cpp",
    "src/async/scheduler_queue.cpp",
    "src/async/scheduler_rwlock.cpp",
    "src/async/scheduler_semaphore.cpp",
    "src/async/scheduler_timer.cpp",
    "src/async/select.cpp",
    "src/async/select_event.cpp",
    "src/async/select_timer.cpp",
    "src/async/queue_port.cpp",
    "src/async/mutex_test_seam.cpp",
    "src/async/queue_test_seam.cpp",
]
# Never-installed pre-v1 experimental surfaces removed from the tree.
removed_experimental = [
    "include/sluice/experimental/uring_io_context.hpp",
    "include/sluice/experimental/uring_write_batch.hpp",
    "src/experimental/uring_io_context.cpp",
    "src/experimental/uring_write_batch.cpp",
]
# Retained public headers whose closure the contraction must not widen.
retained_closures = [
    "request",
    "request_scope",
    "async_io_context",
    "stackful_io_host",
    "fiber",
    "fiber_ctx",
    "threadpool_backend",
    "uring_backend",
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
removed_member_symbols = {}
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
        async_archive = prefix / "lib" / "libsluice_async.a"
        async_members = (evidence / "members-async.log").read_text().splitlines()
        removed_member_names = {pathlib.Path(name).name + ".o" for name in removed_sources}
        member_dir = output / (stage + "-members-" + ring)
        member_dir.mkdir()
        for member in async_members:
            extracted = member_dir / member
            extracted.write_bytes(subprocess.check_output(
                ["ar", "p", str(async_archive), member]))
            listing = subprocess.check_output(
                ["nm", "-C", "--defined-only", str(extracted)], text=True)
            extracted.unlink()
            names = sorted(line.split(" T ", 1)[1] for line in listing.splitlines()
                           if " T " in line)
            (member_dir / (member + ".symbols")).write_text("\n".join(names) + "\n")
            if stage == "before" and member in removed_member_names:
                removed_member_symbols[member] = set(names)
        removed_defs = (r"sluice::async::Scheduler::|sluice::async::AsyncCondition::|"
                        r"sluice::async::Semaphore::|sluice::async::AsyncQueue::|"
                        r"sluice::async::detail::QueuePort::|sluice::async::detail::QueueItem|"
                        r"sluice::async::default_wait_policy|sluice::async::WaitPolicy|"
                        r"sluice::async::detail::queue_lease_fail_fast|"
                        r"sluice::async::detail::select_invariant_fail_fast|"
                        r"sluice::async::detail::scheduler_\w+_fail_fast|"
                        r"sluice::async::detail::evented_admission|"
                        r"sluice::async::detail::async_mutex_\w+_fail_fast|"
                        r"sluice::async::detail::async_rwlock_\w+_fail_fast|"
                        r"sluice::async::detail::wait_queue_lifetime_fail_fast|"
                        r"sluice::async::detail::async_condition_lifetime_fail_fast")
        definitions = (evidence / "defined-async.log").read_text()
        undefined = (evidence / "undefined-async.log").read_text()
        if stage == "before":
            # Positive controls: the after-side predicates must be observably
            # false here, so a typo in a path or pattern cannot make the
            # after-side assertions vacuous.
            for name in removed_sources:
                assert any(name in x["sources"] for x in resolved.values()), (name, resolved)
                assert any(pathlib.Path(name).name + ".o" in p for x in resolved.values()
                           for p in x["objects"]), name
            for name in removed_headers + deinstalled_headers:
                assert (prefix / "include" / name).exists(), name
                assert name in installed, name
            for name in removed_experimental:
                assert (root / name).exists(), name
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
            for name in deinstalled_headers:
                assert (root / "include" / name).exists(), name
                assert not (prefix / "include" / name).exists(), name
                assert name not in installed, name
            for name in removed_experimental:
                assert not (root / name).exists(), name
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
        retained = clean / "retained-closure.cpp"
        retained.write_text("""#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/stackful_io_host.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>
#include <memory>
int main() {
    using namespace sluice::async;
    AsyncIoContext io(std::make_unique<ThreadPoolBackend>());
    auto host = StackfulIoHost::create(io, {});
    if (!host) return 1;
    RequestId id;
    return id.valid() ? 2 : 0;
}
""")
        command(flags + [retained, *libraries, "-o", clean / "retained-closure"],
                "link-retained-closure", evidence)
        command([clean / "retained-closure"], "run-retained-closure", evidence)
        command(["nm", "-C", clean / "retained-closure"], "symbols-retained-closure", evidence)
        old = clean / "old-headers"
        # The removed surfaces are only self-consistent against the whole old
        # header set (scheduler.hpp pulls the full legacy closure), so the
        # probes compile against the complete before-tree include/ - placed
        # first on the search path so every nested include resolves inside the
        # old set - and link against the library under test: at before the link
        # succeeds (positive control; constructor-real probes also run), at
        # after the link step itself is the assertion.
        old.mkdir()
        shutil.copytree(output / "before-n" / "include", old, dirs_exist_ok=True)
        old_flags = list(flags)
        old_flags.insert(old_flags.index("-I" + str(prefix / "include")),
                         "-I" + str(old))
        # Probes that construct real objects run at before; null-scheduler
        # probes (marked link_only) reference the removed symbols without
        # dereferencing, so their positive control is the successful link.
        old_entry_points = {
            "scheduler-lifecycle": ("""
                #include <sluice/async/async_io_context.hpp>
                #include <sluice/async/fiber.hpp>
                #include <sluice/async/scheduler.hpp>
                #include <sluice/async/threadpool_backend.hpp>
                #include <array>
                #include <memory>
                int main() {
                    using namespace sluice::async;
                    AsyncIoContext io(std::make_unique<ThreadPoolBackend>());
                    Scheduler scheduler(io);
                    std::array<std::byte, 65536> stack{};
                    Fiber fiber([](Fiber&) {});
                    if (!scheduler.init_fiber(fiber, stack.data(), stack.size())) return 2;
                    scheduler.spawn(fiber);
                    scheduler.run(1);
                    return fiber.state() != FiberState::done;
                }
            """, True),
            "semaphore-acquire": ("""
                #include <sluice/async/async_io_context.hpp>
                #include <sluice/async/scheduler.hpp>
                #include <sluice/async/semaphore.hpp>
                #include <sluice/async/threadpool_backend.hpp>
                #include <memory>
                int main() {
                    using namespace sluice::async;
                    AsyncIoContext io(std::make_unique<ThreadPoolBackend>());
                    Scheduler scheduler(io);
                    Semaphore semaphore(scheduler, 1, 1);
                    if (!semaphore.try_acquire()) return 2;
                    return semaphore.release() ? 0 : 3;
                }
            """, True),
            "condition-notify": ("""
                #include <sluice/async/async_io_context.hpp>
                #include <sluice/async/condition.hpp>
                #include <sluice/async/scheduler.hpp>
                #include <sluice/async/threadpool_backend.hpp>
                #include <memory>
                int main() {
                    using namespace sluice::async;
                    AsyncIoContext io(std::make_unique<ThreadPoolBackend>());
                    Scheduler scheduler(io);
                    AsyncMutex mutex(scheduler);
                    AsyncCondition condition(mutex);
                    condition.notify_one();
                    condition.notify_all();
                    return 0;
                }
            """, True),
            "queue-push": ("""
                #include <sluice/async/async_io_context.hpp>
                #include <sluice/async/async_queue.hpp>
                #include <sluice/async/scheduler.hpp>
                #include <sluice/async/threadpool_backend.hpp>
                #include <memory>
                int main() {
                    using namespace sluice::async;
                    AsyncIoContext io(std::make_unique<ThreadPoolBackend>());
                    Scheduler scheduler(io);
                    AsyncQueue<int> queue(scheduler, 4);
                    auto result = queue.try_push(7);
                    return result.status() == QueuePushStatus::committed ? 0 : 2;
                }
            """, False),
            "future-await": ("""
                #include <sluice/async/future.hpp>
                int main() {
                    using namespace sluice::async;
                    Future<void> future;
                    future.complete_with(sluice::Result<void>{});
                    return future.await() ? 0 : 1;
                }
            """, True),
            "wait-policy": ("""
                #include <sluice/async/wait_policy.hpp>
                using OldPolicy = sluice::async::WaitPolicy& (*)() noexcept;
                int main() { OldPolicy p = &sluice::async::default_wait_policy;
                    return p == nullptr; }
            """, True),
            "async-mutex-lock": ("""
                #include <sluice/async/async_mutex.hpp>
                int main() {
                    sluice::async::Scheduler& scheduler =
                        *reinterpret_cast<sluice::async::Scheduler*>(0);
                    sluice::async::AsyncMutex mutex(scheduler);
                    sluice::async::WaitNode node;
                    mutex.lock(node);
                    mutex.unlock();
                    return 0;
                }
            """, False),
            "rwlock-write": ("""
                #include <sluice/async/async_rwlock.hpp>
                int main() {
                    sluice::async::Scheduler& scheduler =
                        *reinterpret_cast<sluice::async::Scheduler*>(0);
                    sluice::async::AsyncRwLock lock(scheduler);
                    sluice::async::WaitNode node;
                    lock.write_lock(node);
                    lock.unlock_write();
                    return 0;
                }
            """, False),
            "event-set": ("""
                #include <sluice/async/event.hpp>
                int main() {
                    sluice::async::Scheduler& scheduler =
                        *reinterpret_cast<sluice::async::Scheduler*>(0);
                    sluice::async::Event event(scheduler);
                    event.set();
                    event.reset();
                    return 0;
                }
            """, False),
            "select-await": ("""
                #include <sluice/async/event.hpp>
                #include <sluice/async/select.hpp>
                int main() {
                    using namespace sluice::async;
                    Scheduler& scheduler = *reinterpret_cast<Scheduler*>(0);
                    Event event(scheduler);
                    SelectResult result = select(scheduler, EventSelectCase{event});
                    return result.index() == 0 ? 0 : 2;
                }
            """, False),
        }
        import textwrap
        for name, (body, run_at_before) in old_entry_points.items():
            probe = clean / (name + "-old-entry.cpp")
            probe.write_text(textwrap.dedent(body))
            text = command(old_flags + [probe, *libraries, "-o", clean / (name + "-old-entry")],
                           "old-entry-" + name, evidence,
                           0 if stage == "before" else "failure")
            if stage == "after":
                assert "undefined reference" in text, name
            elif run_at_before:
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

removed_basenames = {pathlib.Path(h).name for h in removed_headers + deinstalled_headers}
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
    # 14a27a83 (the F2 exit master) installs 2 core + 30 async objects, 30
    # async archive members and 49 headers in every ring profile; the F2
    # closeout receipt records the same 49/30 split. F5 retires seventeen
    # compiled sources (one object/archive member each), twenty-three
    # tree-removed installed headers and one de-installed private header.
    assert counts_before == [32, 30, 49], counts_before
    assert counts_after == [32 - len(removed_sources), 30 - len(removed_sources),
                            49 - len(removed_headers) - len(deinstalled_headers)], counts_after
    for other in ("n", "y"):
        if other == ring:
            continue
        assert [summary["before-" + other][k] for k in
                ("library_objects", "async_archive_members", "installed_headers")] == counts_before
        assert [summary["after-" + other][k] for k in
                ("library_objects", "async_archive_members", "installed_headers")] == counts_after

# Every defined symbol of every removed archive member must be absent from the
# after archives in both ring profiles, and no retained member may have picked
# one up as an undefined reference.
after_defined = ""
after_undefined = ""
for ring in ("n", "y"):
    evidence = output / ("after-evidence-" + ring)
    after_defined += (evidence / "defined-async.log").read_text()
    after_undefined += (evidence / "undefined-async.log").read_text()
for member, symbols in removed_member_symbols.items():
    for symbol in symbols:
        assert symbol not in after_defined, (member, symbol, "definition survived")
        assert symbol not in after_undefined, (member, symbol, "reference survived")
assert removed_member_symbols, "control: no before-side member symbols were collected"

print("F5_B01_V6_CANDIDATE_FRESH_PACKAGE_PASS", sha_before, sha_after)
