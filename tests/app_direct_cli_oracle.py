import hashlib, os, pathlib, subprocess, sys, tempfile
app, binary = sys.argv[1:3]
if len(sys.argv) > 3:
    os.environ['LD_PRELOAD'] = str(pathlib.Path(sys.argv[3]).resolve())
    os.environ['SLUICE_DIRECT_READ_LIMIT'] = '17'

binary = str(pathlib.Path(binary).resolve())
usage = 1 if app == "hash" else 2
with tempfile.TemporaryDirectory() as d:
    root = pathlib.Path(d)
    (root / "a").write_bytes(b"hit one\n" + b"x" * 9000 + b"\nhit tail")
    (root / "b").write_bytes(b"hit two\n")
    (root / "empty").write_bytes(b"")
    os.mkfifo(root / "fifo")
    prefix = [] if app == "hash" else ["hit"]
    for flag in [["--workers", v] for v in ["1", "2", "64", "0", "65", "garbage"]] + [["--workers"], ["--workers=1"]]:
        result = subprocess.run([binary, *prefix, "fifo", *flag], cwd=d, capture_output=True, timeout=3)
        assert result.returncode == usage and not result.stdout and b"--workers has been removed; omit it to use direct execution" in result.stderr and b"usage:" in result.stderr, (flag, result.returncode, result.stdout, result.stderr)
    result = subprocess.run([binary, "--help"], cwd=d, capture_output=True, timeout=3)
    assert result.returncode == 0 and b"--workers" not in result.stderr
    for size in [4096, 4097, 8192]:
        result = subprocess.run([binary, "--buffer-size", str(size), *prefix, "a", "missing", "b", "empty"], cwd=d, capture_output=True, timeout=3)
        assert result.returncode == 2 and b"missing" in result.stderr
        if app == "hash":
            expected = b"".join(hashlib.sha256((root / f).read_bytes()).hexdigest().encode() + b"  " + f.encode() + b"\n" for f in ["a", "b", "empty"])
        else:
            expected = b"a:hit one\na:hit tail\nb:hit two\n"
        assert result.stdout == expected, (size, result.stdout, expected)
    if app == "grep":
        for flags, files, expected in [([], ["b"], b"hit two\n"), (["-n"], ["b"], b"1:hit two\n"), (["-n"], ["a", "b"], b"a:1:hit one\na:3:hit tail\nb:1:hit two\n")]:
            result = subprocess.run([binary, *flags, "hit", *files], cwd=d, capture_output=True, timeout=3)
            assert result.returncode == 0 and result.stdout == expected
        result = subprocess.run([binary, "absent", "a"], cwd=d, capture_output=True, timeout=3)
        assert result.returncode == 1 and not result.stdout
    if len(sys.argv) > 3:
        os.environ['SLUICE_DIRECT_FAIL_INODE'] = str((root / 'a').stat().st_ino)
        result = subprocess.run([binary, *prefix, 'a', 'b'], cwd=d, capture_output=True, timeout=3)
        assert result.returncode == 2 and b'a:' in result.stderr
        if app == 'hash':
            expected = hashlib.sha256((root / 'b').read_bytes()).hexdigest().encode() + b'  b\n'
        else:
            expected = b'a:hit one\nb:hit two\n'
        assert result.stdout == expected, (result.stdout, expected)
        del os.environ['SLUICE_DIRECT_FAIL_INODE']
    for size in [4095, 67108865]:
        result = subprocess.run([binary, "--buffer-size", str(size), *prefix, "fifo"], cwd=d, capture_output=True, timeout=3)
        assert result.returncode == usage and not result.stdout and b"usage:" in result.stderr
print(app, "CLI_ORACLE_PASS")
