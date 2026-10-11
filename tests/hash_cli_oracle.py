#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('binary', type=Path)
parser.add_argument('--early-bounds', action='store_true')
args = parser.parse_args()
binary = args.binary.resolve()
records = []
with tempfile.TemporaryDirectory(prefix='sluice-hash-oracle-') as directory:
    root = Path(directory)
    paths = [root / 'empty', root / 'abc', root / 'chunks']
    payloads = [b'', b'abc', bytes(range(256)) * 129]
    for path, payload in zip(paths, payloads):
        path.write_bytes(payload)
    expected = ''.join(f'{hashlib.sha256(data).hexdigest()}  {path}\n'
                       for path, data in zip(paths, payloads))
    for workers in (1, 2, 64):
        command = [str(binary), '--buffer-size', '4096', '--workers', str(workers),
                   *map(str, paths)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        assert (result.returncode, result.stdout, result.stderr) == (0, expected, ''), result
        records.append({'command': command, 'exit': result.returncode,
                        'stdout': result.stdout, 'stderr': result.stderr})
    missing = root / 'missing'
    result = subprocess.run([str(binary), str(paths[1]), str(missing), str(paths[2])],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 2
    assert result.stdout == expected.splitlines(keepends=True)[1] + expected.splitlines(keepends=True)[2]
    assert str(missing) in result.stderr
    records.append({'case': 'error-isolation-input-order', 'exit': result.returncode,
                    'stdout': result.stdout, 'stderr': result.stderr})
    for option, invalid in [('--workers', '0'), ('--workers', '65'),
                            ('--workers', '-1'), ('--buffer-size', '0'),
                            ('--buffer-size', '184467440737095516160')]:
        result = subprocess.run([str(binary), option, invalid, str(missing)],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 1 and result.stdout == ''
        assert 'usage:' in result.stderr and 'No such file' not in result.stderr
        records.append({'case': [option, invalid], 'exit': result.returncode,
                        'stdout': result.stdout, 'stderr': result.stderr})
    if args.early_bounds:
        fifo = root / 'must-not-open'
        import os
        os.mkfifo(fifo)
        for bound in ('1', '4095', '67108865'):
            command = [str(binary), '--buffer-size', bound, str(fifo)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=3)
            assert result.returncode == 1 and result.stdout == ''
            assert 'usage:' in result.stderr and 'not a regular file' not in result.stderr
            records.append({'case': 'invalid-bound-before-open', 'command': command,
                            'exit': result.returncode, 'stderr': result.stderr})
print(json.dumps({'HASH_CLI_ORACLE': 'PASS', 'records': records}, indent=2))
