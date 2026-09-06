#!/usr/bin/env python3
"""FDG-0 Phase A — deterministic hermetic self-tests for the Build Truth
layer (issue #298). Covers the A1–A6 / A9–A10 adversarial requirements at
the logic level using synthetic Xmake responses + a fixture compile_commands
database. No xmake, scip-clang, or C++ build is required.

    A1  exact target membership (Xmake resolved sources -> TU set)
    A2  internal-testing exclusion (SLUICE_ASYNC_INTERNAL_TESTING never
        leaks into the selected production world)
    A3  dependency closure (sluice_async -> sluice_core; each TU indexed
        exactly once)
    A4  Xmake-owned TU with no compile_commands entry -> UNKNOWN_BUILD_WORLD
        (fail closed)
    A5  compile_commands entries outside the selected world -> excluded
    A6  a legitimate new production TU (Xmake membership) -> discovered
    A8  path prefix is not authority: a src/ file Xmake does not own is
        excluded even when a compile_commands entry exists
    A9  config identity drift -> different build identity
    A10 stale Build Manifest under a different configuration -> not fresh
    A11 duplicate/ambiguous compile commands for one TU -> fail closed
    provenance: BUILD is recorded in the manifest

Corrective-1 (PR #302 review) regressions:

    CR6 a TU must not borrow a sibling/dependency target's compdb entry:
        owning target missing while another closure target owns the same
        path -> UNKNOWN_BUILD_WORLD
    CR7 unrelated compdb cardinality (diagnostics) does not change the
        selected build identity
    CR8 compdb ownership derives ONLY from the `-o` object path, never from
        arbitrary command text containing `/.objs/...`
"""
from __future__ import annotations

import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
FORMAL_DIR = SCRIPT_DIR.parent / "formal"
sys.path.insert(0, str(FORMAL_DIR))
sys.path.insert(0, str(SCRIPT_DIR))

import build_truth as bt  # noqa: E402

HEAD = "abcdef0123456789abcdef0123456789abcdef01"
XMAKE_VER = "v3.0.9+HEAD.2b184e1"
COMPILER = "/usr/bin/g++"

DEPGRAPH = {
    "root_targets": ["sluice_async"],
    "targets": [
        {"name": "sluice_async", "deps": ["sluice_core"]},
        {"name": "sluice_core", "deps": []},
        {"name": "sluice_async_internal_testing", "deps": ["sluice_core"]},
        {"name": "sluice_experimental_uring", "deps": ["sluice_core"]},
        {"name": "uring_x_test", "deps": ["sluice_core"]},
    ],
}


def target_meta(name, deps, defines=(), globs=()):
    return {
        "name": name,
        "kind": "static",
        "deps": [{"name": d, "source": {"file": "./xmake/f.lua", "line": 1}} for d in deps],
        "defines": [{"value": d, "source": {"file": "./xmake/f.lua", "line": 1}} for d in defines],
        "includedirs": [{"value": "/repo/include", "source": {"file": "./xmake/f.lua", "line": 1}}],
        "files": [{"path": g, "source": {"file": "./xmake/f.lua", "line": 1}} for g in globs],
        "compilers": [{"program": COMPILER, "flags": "-std=c++20"}],
        "targetfile": f"build/linux/x86_64/release/lib{name}.a",
    }


METADATA = {
    "sluice_async": target_meta(
        "sluice_async", ["sluice_core"],
        defines=["SLUICE_HAS_LIBURING"], globs=["src/async/*.cpp"],
    ),
    "sluice_core": target_meta("sluice_core", [], globs=["src/*.cpp"]),
    "sluice_async_internal_testing": target_meta(
        "sluice_async_internal_testing", ["sluice_core"],
        defines=["SLUICE_ASYNC_INTERNAL_TESTING", "SLUICE_HAS_LIBURING"],
        globs=["src/async/*.cpp"],
    ),
    "sluice_experimental_uring": target_meta(
        "sluice_experimental_uring", ["sluice_core"], globs=["src/experimental/*.cpp"]
    ),
}


def obj_path(target: str, src: str) -> str:
    return f"build/.objs/{target}/linux/x86_64/release/{src}.o"


def cc_entry(src: str, target: str, defines=(), includes=()) -> dict:
    args = [COMPILER, "-std=c++20", "-Wall"]
    args += list(defines)
    args += ["-I/repo/include"] + [f"-isystem {i}" for i in includes]
    args += ["-c", src, "-o", obj_path(target, src)]
    return {"file": src, "directory": "/repo", "arguments": args}


def default_sources() -> dict:
    return {
        "sluice_async": ["src/async/a.cpp", "src/async/b.cpp"],
        "sluice_core": ["src/reader.cpp"],
        "sluice_async_internal_testing": ["src/async/a.cpp", "src/async/b.cpp"],
        "sluice_experimental_uring": ["src/experimental/uring_io_context.cpp"],
    }


def default_compdb():
    return [
        cc_entry("src/async/a.cpp", "sluice_async", defines=["-DSLUICE_HAS_LIBURING"]),
        cc_entry("src/async/b.cpp", "sluice_async", defines=["-DSLUICE_HAS_LIBURING"]),
        cc_entry("src/reader.cpp", "sluice_core"),
        # Duplicate variants owned by NON-world targets (test seam / test).
        cc_entry("src/async/a.cpp", "sluice_async_internal_testing",
                 defines=["-DSLUICE_ASYNC_INTERNAL_TESTING", "-DSLUICE_HAS_LIBURING"]),
        cc_entry("src/async/a.cpp", "uring_x_test",
                 defines=["-DSLUICE_ASYNC_INTERNAL_TESTING", "-DSLUICE_HAS_LIBURING"]),
        # Outside the selected world entirely.
        cc_entry("tests/some_test.cpp", "some_test"),
        cc_entry("src/experimental/uring_io_context.cpp", "sluice_experimental_uring"),
        cc_entry("bench/bench_common.cpp", "sluice_bench_common"),
    ]


class FakeXmake:
    """Emulates the four machine-readable Xmake interfaces the Build Truth
    layer depends on (version, config options, depgraph, per-target show,
    resolved source batches)."""

    def __init__(self, options=None, sources=None):
        self.options = dict(options or {})
        self.sources = dict(sources or default_sources())

    def __call__(self, args, cwd=None):
        assert args[0] == "xmake", args
        if args[1] == "--version":
            return "xmake v3.0.9+HEAD.2b184e1, A cross-platform build utility based on Lua\n"
        if args[1] == "lua":
            script = args[3]
            match = re.search(r'project\.target\("([^"]+)"\)', script)
            if match:
                name = match.group(1)
                if name not in self.sources:
                    raise bt.BuildTruthError(f"xmake lua target {name} missing (fake)")
                return "".join(f"{f}\n" for f in self.sources[name])
            # config-options probe
            lines = []
            for key in bt.CONFIG_OPTIONS:
                value = self.options.get(key, "nil")
                if value is None:
                    value = "nil"
                lines.append(f"{key}={value}")
            return "\n".join(lines) + "\n"
        if args[1] == "show":
            if "--info=depgraph" in args:
                return json.dumps(DEPGRAPH)
            if "-t" in args:
                name = args[args.index("-t") + 1]
                if name not in METADATA:
                    raise bt.BuildTruthError(f"xmake show -t {name} failed: no such target")
                return json.dumps(METADATA[name])
        raise AssertionError(f"unexpected xmake invocation: {args}")


def make_manifest(options=None, sources=None, compdb=None):
    fake = FakeXmake(options=options, sources=sources)
    bt.run_capture = fake
    bt.current_head = lambda: HEAD
    compdb = default_compdb() if compdb is None else compdb
    return bt.build_manifest(
        ["sluice_async"], compdb=compdb, head_sha=HEAD, xmake_ver=XMAKE_VER
    )


class BuildTruthTests(unittest.TestCase):
    def setUp(self):
        # Restore the real functions after every test.
        self._real_run = bt.run_capture
        self._real_head = bt.current_head

    def tearDown(self):
        bt.run_capture = self._real_run
        bt.current_head = self._real_head

    # --- A1: exact target membership ----------------------------------------

    def test_a1_membership_comes_from_resolved_sources(self):
        manifest = make_manifest()
        world = manifest["worlds"][0]
        self.assertEqual(
            set(world["tus"]),
            {"src/async/a.cpp", "src/async/b.cpp", "src/reader.cpp"},
        )
        self.assertEqual(set(world["targets"]), {"sluice_async", "sluice_core"})

    def test_a1_selected_entries_match_membership(self):
        manifest = make_manifest()
        world = manifest["worlds"][0]
        self.assertEqual(world["compdb"]["selected_entries"], 3)
        self.assertEqual(world["compdb"]["total_entries"], 8)

    # --- A2: internal-testing exclusion -------------------------------------

    def test_a2_internal_testing_macro_never_leaks(self):
        manifest = make_manifest()
        tu = manifest["worlds"][0]["tus"]["src/async/a.cpp"]
        self.assertEqual(tu["owning_targets"], ["sluice_async"])
        self.assertNotIn("-DSLUICE_ASYNC_INTERNAL_TESTING", tu["defines"])
        self.assertIn("-DSLUICE_HAS_LIBURING", tu["defines"])

    # --- A3: dependency closure, each TU indexed once -----------------------

    def test_a3_closure_and_single_index(self):
        manifest = make_manifest()
        world = manifest["worlds"][0]
        self.assertEqual(world["dependency_closure"], ["sluice_async", "sluice_core"])
        self.assertEqual(len(world["tus"]), 3)  # no duplicate TU entries

    # --- A4: missing compile command for an owned TU ------------------------

    def test_a4_missing_compdb_entry_fails_closed(self):
        compdb = default_compdb()
        compdb = [e for e in compdb if e["file"] != "src/async/a.cpp"
                  or bt.compdb_owner(e) != "sluice_async"]
        with self.assertRaises(bt.BuildTruthError) as ctx:
            make_manifest(compdb=compdb)
        self.assertIn("UNKNOWN_BUILD_WORLD", str(ctx.exception))
        self.assertIn("src/async/a.cpp", str(ctx.exception))

    # --- A5: entries outside the selected world are excluded ----------------

    def test_a5_extra_entries_excluded(self):
        manifest = make_manifest()
        world = manifest["worlds"][0]
        for path in ("tests/some_test.cpp", "bench/bench_common.cpp",
                     "src/experimental/uring_io_context.cpp"):
            self.assertNotIn(path, world["tus"], path)

    # --- A6: a new production TU is discovered via membership ---------------

    def test_a6_new_tu_discovered(self):
        sources = default_sources()
        sources["sluice_async"] = sources["sluice_async"] + ["src/async/new.cpp"]
        compdb = default_compdb() + [
            cc_entry("src/async/new.cpp", "sluice_async", defines=["-DSLUICE_HAS_LIBURING"])
        ]
        manifest = make_manifest(sources=sources, compdb=compdb)
        self.assertIn("src/async/new.cpp", manifest["worlds"][0]["tus"])
        self.assertEqual(
            manifest["worlds"][0]["tus"]["src/async/new.cpp"]["owning_targets"],
            ["sluice_async"],
        )

    # --- A8: path prefix is not authority -----------------------------------

    def test_a8_compdb_entry_without_membership_is_excluded(self):
        # A compile_commands entry exists for src/async/orphan.cpp owned by a
        # world target, but Xmake membership does NOT resolve that file.
        compdb = default_compdb() + [
            cc_entry("src/async/orphan.cpp", "sluice_async", defines=["-DSLUICE_HAS_LIBURING"])
        ]
        manifest = make_manifest(compdb=compdb)
        self.assertNotIn("src/async/orphan.cpp", manifest["worlds"][0]["tus"])
        self.assertEqual(manifest["worlds"][0]["compdb"]["selected_entries"], 3)

    # --- A9: config identity drift ------------------------------------------

    def test_a9_config_drift_changes_build_identity(self):
        liburing = make_manifest(options={"with-liburing": "true"})
        stub = make_manifest(options={"with-liburing": "false"})
        self.assertNotEqual(liburing["build_id"], stub["build_id"])
        self.assertEqual(liburing["worlds"][0]["id"], "sluice_async-liburing")
        self.assertEqual(stub["worlds"][0]["id"], "sluice_async-default")

    # --- A10: stale manifest under another configuration --------------------

    def test_a10_stale_manifest_not_fresh(self):
        manifest = make_manifest(options={"with-liburing": "true"})
        # Query the manifest under a DIFFERENT configuration.
        fake = FakeXmake(options={"with-liburing": "false"})
        bt.run_capture = fake
        bt.current_head = lambda: HEAD
        result = bt.verify_current_world(manifest, compdb=default_compdb())
        self.assertFalse(result["fresh"])
        self.assertEqual(result["reason"], "BUILD_WORLD_CHANGED")
        self.assertNotEqual(
            result["current_build_id"], result["manifest_build_id"]
        )

    def test_a10_fresh_manifest_verifies(self):
        manifest = make_manifest(options={"with-liburing": "true"})
        fake = FakeXmake(options={"with-liburing": "true"})
        bt.run_capture = fake
        bt.current_head = lambda: HEAD
        result = bt.verify_current_world(manifest, compdb=default_compdb())
        self.assertTrue(result["fresh"])

    # --- A11: duplicate/ambiguous compile commands fail closed --------------

    def test_a11_ambiguous_entries_fail_closed(self):
        compdb = default_compdb()
        compdb.append(
            cc_entry("src/async/b.cpp", "sluice_async",
                     defines=["-DSLUICE_HAS_LIBURING", "-DEXTRA_FLAG"])
        )
        with self.assertRaises(bt.BuildTruthError) as ctx:
            make_manifest(compdb=compdb)
        self.assertIn("incompatible compile_commands candidates", str(ctx.exception))

    def test_a11_identical_duplicates_dedupe(self):
        compdb = default_compdb()
        dupe = cc_entry("src/async/b.cpp", "sluice_async", defines=["-DSLUICE_HAS_LIBURING"])
        compdb.append(dupe)
        manifest = make_manifest(compdb=compdb)
        self.assertIn("src/async/b.cpp", manifest["worlds"][0]["tus"])

    # --- provenance / identity ----------------------------------------------

    def test_build_provenance_recorded(self):
        manifest = make_manifest()
        self.assertIn("BUILD", manifest["provenance"])
        self.assertIn("build_id", manifest)
        self.assertEqual(len(manifest["build_id"]), 64)

    # --- corrective-1: exact per-TU owner join (CR6 / C4) --------------------

    def test_c4_cr6_tu_cannot_borrow_sibling_target_entry(self):
        # src/async/a.cpp is owned ONLY by sluice_async. Its sluice_async
        # compdb entry is removed while a sluice_core-owned entry for the
        # SAME path remains. sluice_core is inside the dependency closure,
        # but borrowing its compile command would analyze the TU under the
        # wrong target's compiler interpretation -> fail closed.
        compdb = [
            e
            for e in default_compdb()
            if not (e["file"] == "src/async/a.cpp"
                    and bt.compdb_owner(e) == "sluice_async")
        ]
        compdb.append(
            cc_entry("src/async/a.cpp", "sluice_core", defines=["-DSLUICE_HAS_LIBURING"])
        )
        with self.assertRaises(bt.BuildTruthError) as ctx:
            make_manifest(compdb=compdb)
        message = str(ctx.exception)
        self.assertIn("UNKNOWN_BUILD_WORLD", message)
        self.assertIn("src/async/a.cpp", message)
        self.assertIn("sluice_async", message)

    def test_c4_owner_outside_both_tu_and_closure_never_selected(self):
        # Defense in depth: an entry owned by a NON-closure target is still
        # excluded even when the TU's owning target entry is missing (the
        # failure is UNKNOWN_BUILD_WORLD, not a silent borrow).
        compdb = [
            e
            for e in default_compdb()
            if not (e["file"] == "src/async/a.cpp"
                    and bt.compdb_owner(e) == "sluice_async")
        ]
        compdb.append(
            cc_entry("src/async/a.cpp", "sluice_experimental_uring",
                     defines=["-DSLUICE_HAS_LIBURING"])
        )
        with self.assertRaises(bt.BuildTruthError) as ctx:
            make_manifest(compdb=compdb)
        self.assertIn("UNKNOWN_BUILD_WORLD", str(ctx.exception))

    # --- corrective-1: build identity scope (CR7 / C5) -----------------------

    def test_c5_cr7_unrelated_compdb_entry_does_not_change_build_id(self):
        # An unrelated test entry appearing in compile_commands.json changes
        # the compdb cardinality diagnostics but must NOT change the
        # production build identity (selected world is unchanged).
        base = make_manifest()
        with_extra = make_manifest(
            compdb=default_compdb()
            + [cc_entry("tests/unrelated_test.cpp", "some_other_test")]
        )
        self.assertNotEqual(
            base["worlds"][0]["compdb"]["total_entries"],
            with_extra["worlds"][0]["compdb"]["total_entries"],
        )
        self.assertEqual(
            base["worlds"][0]["compdb"]["selected_entries"],
            with_extra["worlds"][0]["compdb"]["selected_entries"],
        )
        self.assertEqual(base["build_id"], with_extra["build_id"])

    # --- corrective-1: -o-derived ownership (CR8 / C6) ------------------------

    def test_c6_cr8_owner_derives_from_o_argument_only(self):
        # An include path that happens to contain /.objs/<fake>/ must NOT be
        # treated as the owner; ownership comes from the -o object path.
        entry = {
            "file": "src/async/a.cpp",
            "directory": "/repo",
            "arguments": [
                COMPILER,
                "-I/tmp/.objs/fake/include",
                "-DSLUICE_HAS_LIBURING",
                "-c", "src/async/a.cpp",
                "-o", obj_path("sluice_async", "src/async/a.cpp"),
            ],
        }
        self.assertEqual(bt.compdb_owner(entry), "sluice_async")

    def test_c6_cr8_command_string_is_shell_tokenized_before_o_lookup(self):
        entry = {
            "file": "src/async/a.cpp",
            "directory": "/repo",
            "command": (
                f"{COMPILER} -I/tmp/.objs/fake/include -c src/async/a.cpp "
                f"-o {obj_path('sluice_core', 'src/async/a.cpp')}"
            ),
        }
        self.assertEqual(bt.compdb_owner(entry), "sluice_core")

    def test_c6_cr8_owner_unknown_without_determinable_o(self):
        base = {"file": "x.cpp", "directory": "/r"}
        # No -o at all.
        self.assertIsNone(
            bt.compdb_owner({**base, "arguments": [COMPILER, "-c", "x.cpp"]})
        )
        # Trailing -o with no value.
        self.assertIsNone(
            bt.compdb_owner({**base, "arguments": [COMPILER, "-c", "x.cpp", "-o"]})
        )
        # Two incompatible -o outputs: ambiguous -> unknown.
        self.assertIsNone(
            bt.compdb_owner({
                **base,
                "arguments": [COMPILER, "-c", "x.cpp",
                              "-o", obj_path("t1", "x.cpp"),
                              "-o", obj_path("t2", "x.cpp")],
            })
        )
        # -o present but the object path is outside the known xmake .objs
        # layout -> ownership not derivable.
        self.assertIsNone(
            bt.compdb_owner({**base, "arguments": [COMPILER, "-c", "x.cpp",
                                                   "-o", "/tmp/somewhere/x.o"]})
        )

    def test_c6_cr8_flags_like_wl_o_are_not_output_flags(self):
        # `-Wl,-o,...`-style tokens or values containing -o must not be
        # mistaken for the output flag.
        entry = {
            "file": "src/async/a.cpp",
            "directory": "/repo",
            "arguments": [
                COMPILER, "-Wl,-o,build/.objs/other/x.o", "-c", "src/async/a.cpp",
                "-o", obj_path("sluice_async", "src/async/a.cpp"),
            ],
        }
        self.assertEqual(bt.compdb_owner(entry), "sluice_async")

    def test_compdb_owner_parsing(self):
        self.assertEqual(
            bt.compdb_owner(cc_entry("src/async/a.cpp", "sluice_async")),
            "sluice_async",
        )
        self.assertIsNone(bt.compdb_owner({"file": "x.cpp", "directory": "/r",
                                           "arguments": ["g++", "-c", "x.cpp"]}))

    def test_entry_identity_is_deterministic(self):
        e1 = cc_entry("src/async/a.cpp", "sluice_async", defines=["-DSLUICE_HAS_LIBURING"])
        e2 = cc_entry("src/async/a.cpp", "sluice_async", defines=["-DSLUICE_HAS_LIBURING"])
        self.assertEqual(bt.entry_identity(e1), bt.entry_identity(e2))
        e3 = cc_entry("src/async/a.cpp", "sluice_async", defines=["-DOTHER"])
        self.assertNotEqual(bt.entry_identity(e1), bt.entry_identity(e3))

    def test_missing_selected_target_fails(self):
        fake = FakeXmake()
        fake.sources = {}
        bt.run_capture = fake
        bt.current_head = lambda: HEAD
        # Remove sluice_async from the depgraph -> dependency_closure raises.
        def bad_depgraph(args, cwd=None):
            if "--info=depgraph" in args:
                return json.dumps({"root_targets": [], "targets": [
                    {"name": "sluice_core", "deps": []}]})
            return fake(args, cwd=cwd)

        bt.run_capture = bad_depgraph
        with self.assertRaises(bt.BuildTruthError) as ctx:
            bt.build_manifest(["sluice_async"], compdb=default_compdb(),
                              head_sha=HEAD, xmake_ver=XMAKE_VER)
        self.assertIn("not present in the Xmake depgraph", str(ctx.exception))


class BuildManifestLoadTests(unittest.TestCase):
    def test_load_manifest_roundtrip(self):
        manifest = make_manifest()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "build-manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            loaded = bt.load_manifest(path)
            self.assertEqual(loaded["build_id"], manifest["build_id"])
            # build_id is deterministic across serialize/deserialize.
            self.assertEqual(bt.compute_build_identity(loaded), manifest["build_id"])

    def test_load_missing_manifest_fails(self):
        with self.assertRaises(bt.BuildTruthError):
            bt.load_manifest(Path("/nonexistent/build-manifest.json"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
