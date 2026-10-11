#!/usr/bin/env python3
import pathlib
import resource
import signal
import subprocess
import sys
import tempfile

binary = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="sluice-f3-copy-cli-") as tmp:
    root = pathlib.Path(tmp)
    for index, (length, depth, workers, sync, atomic) in enumerate([
        (0, 1, 1, "none", True),
        (177, 4, 3, "none", True),
        (177, 3, 1, "data", True),
        (177, 4, 2, "all", True),
        (177, 1, 1, "all", False),
        (177, 3, 3, "data", False),
    ]):
        source = root / f"source-{index}"
        destination = root / f"destination-{index}"
        payload = bytes((i * 31 + 7) % 256 for i in range(length))
        source.write_bytes(payload)
        destination.write_bytes(b"previous destination")
        args = [str(binary), "--buffer-size", "16", "--pipeline-depth", str(depth),
                "--workers", str(workers), "--sync", sync]
        if not atomic:
            args.append("--no-atomic")
        args.extend([str(source), str(destination)])
        result = subprocess.run(args, capture_output=True, text=True, timeout=30)
        print(f"RUN: {' '.join(args)}")
        print(result.stdout.strip())
        assert result.returncode == 0, result.stderr
        assert destination.read_bytes() == payload
        assert not list(root.glob(".sluice-copy.tmp.*"))
        print(f"CLI PASS length={length} depth={depth} workers={workers} sync={sync} atomic={atomic}")


def limit_output():
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (64, 64))


with tempfile.TemporaryDirectory(prefix="sluice-f3-copy-cli-fault-") as tmp:
    root = pathlib.Path(tmp)
    payload = bytes((i * 31 + 7) % 256 for i in range(177))
    source = root / "source"
    source.write_bytes(payload)
    for atomic, depth in [(True, 3), (False, 4)]:
        destination = root / "destination"
        marker = b"previous destination"
        destination.write_bytes(marker)
        args = [str(binary), "--buffer-size", "16", "--pipeline-depth", str(depth),
                "--workers", "2", "--sync", "none"]
        if not atomic:
            args.append("--no-atomic")
        args.extend([str(source), str(destination)])
        result = subprocess.run(args, capture_output=True, text=True, timeout=30,
                                preexec_fn=limit_output)
        print(f"FAULT RUN RLIMIT_FSIZE=64 SIGXFSZ=ignored: {' '.join(args)}")
        print(result.stderr.strip())
        assert result.returncode != 0
        retained = destination.read_bytes()
        if atomic:
            assert retained == marker
        else:
            assert 0 < len(retained) <= 64
            assert retained == payload[:len(retained)]
        assert not list(root.glob(".sluice-copy.tmp.*"))
        print(f"CLI FAILURE PASS atomic={atomic} retained={len(retained)} exit={result.returncode}")
