#!/usr/bin/env python3
"""statfs fstype helper for COPY-X0 scripts (no external deps).

Returns the canonical fstype name plus the numeric magic ('tmpfs:0x1021994',
'ext4:0xef53') so validators can compare labels against physical substrate
exactly (C0 Corrective-2 substrate-authority lesson).
"""

import subprocess

MAGICS = {
    "0x1021994": "tmpfs",
    "0xef53": "ext4",
}


def fstype(path: str) -> str:
    out = subprocess.run(["stat", "-f", "-c", "%t", path],
                         capture_output=True, text=True, check=True).stdout.strip().lower()
    hexmagic = out if out.startswith("0x") else f"0x{out}"
    name = MAGICS.get(hexmagic, "unknown")
    return f"{name}:{hexmagic}"


def canonical_name(path: str) -> str:
    return fstype(path).split(":")[0]
