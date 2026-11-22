"""float32 helpers and the golden sample-file format.

A golden sample file (`<case>.in.f32`, `<case>.out.f32`) is raw IEEE-754 binary32, little-endian, channel-major: all
frames of channel 0, then all frames of channel 1, ... The channel names and the frame count are in the case's .json.
"""
import array
import hashlib
import struct
import sys

_PACK = struct.Struct("<f")


def F(x):
    """Round a Python float (double) to the nearest float32 (round to nearest even), as a C cast does."""
    return _PACK.unpack(_PACK.pack(x))[0]


def bits(x):
    """The binary32 bit pattern of x (already float32-representable) as an unsigned int."""
    return struct.unpack("<I", _PACK.pack(x))[0]


def to_bytes(channels):
    """channels: list of equal-length lists of float32-representable values -> bytes (channel-major, little-endian)."""
    a = array.array("f")
    for c in channels:
        a.extend(c)
    if sys.byteorder != "little":
        a.byteswap()
    return a.tobytes()


def from_bytes(data, n_channels):
    a = array.array("f")
    a.frombytes(data)
    if sys.byteorder != "little":
        a.byteswap()
    if n_channels == 0:
        return []
    n = len(a) // n_channels
    return [list(a[i * n:(i + 1) * n]) for i in range(n_channels)]


def sha256(data):
    return hashlib.sha256(data).hexdigest()
