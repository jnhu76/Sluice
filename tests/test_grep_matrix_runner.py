import os
import pathlib
import subprocess
import tempfile
import unittest


class MatrixRunnerTest(unittest.TestCase):
    def test_two_runs_at_same_sha_preserve_workspaces_and_receipts(self):
        runner = pathlib.Path(__file__).resolve().parents[1] / "docs/review/f3-grep-evidence-20261011/run-matrix.sh"
        with tempfile.TemporaryDirectory(prefix="sluice-grep-runner-") as directory:
            root = pathlib.Path(directory)
            repo = root / "repo"
            repo.mkdir()
            evidence = root / "evidence"
            evidence.mkdir()
            unknown = root / "unknown"
            tools = root / "tools"
            tools.mkdir()
            xmake = tools / "xmake"
            xmake.write_text("#!/bin/sh\nexit 0\n")
            xmake.chmod(0o755)
            (repo / "tests").mkdir()
            (repo / "tests/grep_cli_oracle.py").write_text("print('runner fixture')\n")
            def git(*args):
                return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True)
            git("init", "-q")
            git("add", "tests")
            git("-c", "user.name=Runner Test", "-c", "user.email=runner@example.invalid", "commit", "-qm", "runner fixture")
            sha = git("rev-parse", "HEAD").stdout.strip()
            git("worktree", "add", "--detach", str(unknown), sha)
            (unknown / "sentinel").write_text("preserve this workspace")
            adapted = root / "run-matrix.sh"
            adapted.write_text(runner.read_text().replace("/home/jnhu/Source/Sluice-f3-grep-direct", str(repo))
                               .replace("/home/jnhu/Source/f3-grep-evidence", str(evidence))
                               .replace("/home/jnhu/Source", str(root)))
            env = {**os.environ, "PATH": str(tools) + os.pathsep + os.environ["PATH"]}
            first = subprocess.run(["bash", str(adapted)], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(first.returncode, 0, first.stderr)
            worktrees_before = git("worktree", "list", "--porcelain").stdout
            second = subprocess.run(["bash", str(adapted)], env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(second.returncode, 0, second.stderr)
            paths = [line[9:] for line in git("worktree", "list", "--porcelain").stdout.splitlines() if line.startswith("worktree ")]
            self.assertEqual(len(paths), 10)
            self.assertEqual(len(set(paths)), 10)
            for line in worktrees_before.splitlines():
                if line.startswith("worktree "):
                    self.assertIn(line[9:], paths)
            logs = list(evidence.rglob("*.log"))
            self.assertEqual(len(logs), 8)
            for log in logs:
                self.assertIn(sha, log.read_text())
            self.assertEqual((unknown / "sentinel").read_text(), "preserve this workspace")


if __name__ == "__main__":
    unittest.main()
