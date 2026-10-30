"""
Message sets: many small inputs in one file, for measuring per-call cost.

Timing a single 1 KB compress call is mostly timer noise, so small-message
tiers are stored as a message set and every driver times a loop over all of
its messages, one independent one-shot call per message. The format is a plain
sequence of records:

    u32 little-endian length, then that many bytes   (repeated)

No header or trailer, so every driver can read it in a few lines. A dataset
built from message sets carries kind="msgset" and the runner sends its jobs in
`msgset` mode.
"""

from __future__ import annotations

import struct
from pathlib import Path

SUFFIX = ".msgset"


def write(path: Path, messages: list[bytes]) -> None:
    tmp = path.with_suffix(path.suffix + ".part")
    with open(tmp, "wb") as f:
        for m in messages:
            f.write(struct.pack("<I", len(m)))
            f.write(m)
    tmp.replace(path)


def read(path: Path) -> list[bytes]:
    data = path.read_bytes()
    out, pos = [], 0
    while pos < len(data):
        (n,) = struct.unpack_from("<I", data, pos)
        pos += 4
        out.append(data[pos:pos + n])
        pos += n
    if pos != len(data):
        raise ValueError(f"{path}: truncated message set")
    return out


def blocks(data: bytes, block: int, budget: int) -> list[bytes]:
    """Up to `budget` bytes of `block`-sized blocks, evenly spaced across
    `data` so every region of the file is represented, and aligned to the block
    size (as if the file had been cut into blocks and some of them kept)."""
    total = len(data) // block
    want = min(total, max(1, budget // block))
    if want == 0:
        return []
    stride = total / want
    return [data[int(i * stride) * block:(int(i * stride) + 1) * block] for i in range(want)]
