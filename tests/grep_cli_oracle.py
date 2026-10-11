import os
import pathlib
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())
trace_dir = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
if trace_dir:
    trace_dir.mkdir(parents=True, exist_ok=True)
failures = []
checks = 0

def check(args, code, output=None, diagnostic=False):
    global checks
    checks += 1
    try:
        result = subprocess.run([binary, *args], capture_output=True, timeout=3)
        assert result.returncode == code, (result.returncode, code, result.stderr)
        if output is not None:
            assert result.stdout == output, (result.stdout, output)
        if diagnostic:
            assert result.stderr
    except (AssertionError, subprocess.TimeoutExpired) as error:
        failures.append((args, str(error)))

with tempfile.TemporaryDirectory(prefix="sluice-grep-cli-") as directory:
    os.chdir(directory)
    pathlib.Path("a").write_bytes(b"plain\nhit one\n")
    pathlib.Path("b").write_bytes(b"hit two")
    for workers in (1, 2, 64):
        options = ["--workers", str(workers)]
        check([*options, "hit", "a"], 0, b"hit one\n")
        check([*options, "-n", "hit", "a"], 0, b"2:hit one\n")
        check([*options, "hit", "a", "b"], 0, b"a:hit one\nb:hit two\n")
        check([*options, "-n", "hit", "a", "b"], 0, b"a:2:hit one\nb:1:hit two\n")
    pathlib.Path("empty").write_bytes(b"")
    check(["", "empty"], 1, b"")
    check(["missing", "a", "b"], 1, b"")
    check(["hit", "absent", "a", "b"], 2, b"a:hit one\nb:hit two\n", True)
    check(["hit", directory, "a"], 2, b"a:hit one\n", True)
    pathlib.Path("bytes").write_bytes(b"\nhit\r\n\x00hit\xff\nlast hit")
    check(["-n", "hit", "bytes"], 0, b"2:hit\r\n3:\x00hit\xff\n4:last hit\n")
    check(["", "bytes"], 0, b"\nhit\r\n\x00hit\xff\nlast hit\n")
    crossing = b"x" * 4094 + b"hit\n" + b"z" * 8200 + b"hit\nhit tail"
    pathlib.Path("crossing").write_bytes(crossing)
    check(["--buffer-size", "4096", "--max-line-bytes", "5000", "-n", "hit", "crossing"],
          0, b"1:" + b"x" * 4094 + b"hit\n3:hit tail\n", True)
    os.mkfifo("fifo")
    invalid = {"--buffer-size": ("0", "4095", "67108865"),
               "--max-line-bytes": ("0", "67108865"),
               "--workers": ("0", "65", "4294967296")}
    for option, bounds in invalid.items():
        for value in (*bounds, "-1", "+1", "1x", "", "18446744073709551616"):
            check([option, value, "hit", "fifo", "a"], 2, b"", True)
    for buffer_size, cap in ((4096, 1), (67108864, 67108864)):
        check(["--buffer-size", str(buffer_size), "--max-line-bytes", str(cap), "", "empty"], 1, b"")
    check(["hit"], 2, b"", True)
    check(["hit\n", "a"], 2, b"", True)
    check(["--help"], 0)
    if trace_dir:
        for workers in (1, 2, 64):
            trace = trace_dir / f"workers-{workers}.trace"
            result = subprocess.run(["strace", "-f", "-e", "trace=clone,clone3",
                "-o", str(trace), binary, "--workers", str(workers), "hit", "a"],
                capture_output=True, timeout=10)
            calls = [line for line in trace.read_text().splitlines() if "clone" in line]
            checks += 1
            if result.returncode != 0 or (workers == 1 and calls) or (workers > 1 and not calls):
                failures.append((["thread trace", str(workers)], str(calls)))
            print(f"THREAD_TRACE workers={workers} clone_calls={len(calls)}")
        trace = trace_dir / "invalid-fifo.trace"
        result = subprocess.run(["strace", "-f", "-e", "trace=openat",
            "-o", str(trace), binary, "--buffer-size", "4095", "hit", "fifo", "a"],
            capture_output=True, timeout=3)
        checks += 1
        if result.returncode != 2 or '"fifo"' in trace.read_text():
            failures.append((["FIFO open trace"], "opened FIFO or wrong exit"))

for args, error in failures:
    print("FAIL", args, error)
print(f"CLI_ORACLE checks={checks} failures={len(failures)}")
sys.exit(bool(failures))
