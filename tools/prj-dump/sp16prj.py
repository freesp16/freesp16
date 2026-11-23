"""Reference project format: an independent encoder/decoder written from the research page docs/project-format.md
(CC BY 4.0: "Container", "JUCE binary ValueTree encoding", "PatternData.Steps", "ArrangementData.Sections",
"SampleAdjunctData") and the spec page 04-file-formats/sp16-interop.md. Used by prj-synth.py (synthetic test projects:
the only fixtures this repository ever uses) and prj-dump.py (inspect a file locally). No device data is read or
embedded here. Standard library only.

Tree:  node = type NUL · compressed count · (name NUL · var)* · compressed count · node*
       compressed int = length byte (bit 7 = negative, ≤ 4) + little-endian bytes, minimal
       var = compressed N (1 + payload); N = 0 void; marker 1 int, 2/3 bool, 4 double, 5 string (UTF-8 + NUL),
             6 int64, 7 array (count + vars), 8 binary, 9 undefined
Container: b"vltrgzip" + zlib level 1. Steps / Sections blobs: zlib level 6.
Steps: 1024 records of 152 bytes in TRACK-major order (record index = track * 64 + step); a programmed record's first
two int32 are its own track (0-15) and step (0-63), an empty record holds -127 / -126 there. Confirmed against two
real 1.63 projects (every programmed record, 2,759 of 2,759); the research page's step-major reading is wrong.
"""
import base64
import struct
import zlib

MAGIC = b"vltrgzip"
STEPS, TRACKS, WORDS = 64, 16, 35
RECORD = 12 + 4 * WORDS
STEPS_BYTES = STEPS * TRACKS * RECORD  # 155,648
VALUE_FULL = 2097151                   # 2^21 - 1
K127 = 16513                           # VALUE_FULL / 127
FRAME_BASE = 1411200                   # 44,100 x 32
SECTIONS = 999


# ---------------------------------------------------------------------------------------------- values and nodes
class V:
    """A typed value: kind in void, int, bool, double, string, int64, array, binary, undefined."""
    __slots__ = ("kind", "value")

    def __init__(self, kind, value=None):
        self.kind, self.value = kind, value

    def __eq__(self, other):
        return isinstance(other, V) and self.kind == other.kind and self.value == other.value

    def __repr__(self):
        return f"V({self.kind}, {self.value!r})"


def I(v): return V("int", int(v))
def B(v): return V("bool", bool(v))
def D(v): return V("double", float(v))
def S(v): return V("string", str(v))
def L(v): return V("int64", int(v))
def A(items): return V("array", list(items))
def BIN(b): return V("binary", bytes(b))
VOID = V("void")


def from_json(x):
    """A description value: plain JSON (int, float, bool, str, list) or a typed {"int64": 5}, {"binary": "<b64>"} …"""
    if isinstance(x, V):
        return x
    if isinstance(x, dict) and len(x) == 1:
        (k, v), = x.items()
        if k == "int": return I(v)
        if k == "int64": return L(v)
        if k == "double": return D(v)
        if k == "bool": return B(v)
        if k == "string": return S(v)
        if k == "binary": return BIN(base64.b64decode(v))
        if k == "array": return A(from_json(e) for e in v)
        if k in ("void", "undefined"): return V(k)
    if isinstance(x, bool): return B(x)
    if isinstance(x, int): return I(x)
    if isinstance(x, float): return D(x)
    if isinstance(x, str): return S(x)
    if isinstance(x, list): return A(from_json(e) for e in x)
    if x is None: return VOID
    raise ValueError(f"cannot type {x!r}")


class Node:
    __slots__ = ("type", "props", "children")

    def __init__(self, type_, props=None, children=None):
        self.type = type_
        self.props = list(props or [])      # [(name, V)]
        self.children = list(children or [])

    def get(self, name):
        for n, v in self.props:
            if n == name:
                return v
        return None

    def set(self, name, value):
        value = from_json(value)
        for i, (n, _) in enumerate(self.props):
            if n == name:
                self.props[i] = (n, value)
                return
        self.props.append((name, value))

    def remove(self, name):
        self.props = [(n, v) for n, v in self.props if n != name]

    def child(self, type_):
        for c in self.children:
            if c.type == type_:
                return c
        return None


# ---------------------------------------------------------------------------------------------- encoding
def cint(v):
    mag = -v if v < 0 else v
    body = b""
    while mag:
        body += bytes([mag & 0xFF])
        mag >>= 8
    return bytes([len(body) | (0x80 if v < 0 else 0)]) + body


def enc_var(v):
    k = v.kind
    if k == "void": return cint(0)
    if k == "undefined": return cint(1) + b"\x09"
    if k == "int": return cint(5) + b"\x01" + struct.pack("<i", v.value)
    if k == "bool": return cint(1) + (b"\x02" if v.value else b"\x03")
    if k == "double": return cint(9) + b"\x04" + struct.pack("<d", v.value)
    if k == "int64": return cint(9) + b"\x06" + struct.pack("<q", v.value)
    if k == "string":
        p = v.value.encode("utf-8") + b"\x00"
        return cint(len(p) + 1) + b"\x05" + p
    if k == "binary": return cint(len(v.value) + 1) + b"\x08" + v.value
    if k == "array":
        p = cint(len(v.value)) + b"".join(enc_var(e) for e in v.value)
        return cint(len(p) + 1) + b"\x07" + p
    raise ValueError(k)


def enc_node(n):
    out = [n.type.encode("utf-8") + b"\x00", cint(len(n.props))]
    for name, v in n.props:
        out.append(name.encode("utf-8") + b"\x00")
        out.append(enc_var(v))
    out.append(cint(len(n.children)))
    out.extend(enc_node(c) for c in n.children)
    return b"".join(out)


def container(n):
    return MAGIC + zlib.compress(enc_node(n), 1)


# ---------------------------------------------------------------------------------------------- decoding
class Reader:
    def __init__(self, data, pos=0, end=None):
        self.d, self.p, self.end = data, pos, len(data) if end is None else end

    def take(self, n):
        if self.p + n > self.end:
            raise ValueError(f"truncated at byte {self.p}")
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def cint(self):
        h = self.take(1)[0]
        n = h & 0x7F
        if n > 4:
            raise ValueError(f"compressed int of {n} bytes at {self.p - 1}")
        v = int.from_bytes(self.take(n), "little")
        return -v if h & 0x80 else v

    def cstr(self):
        z = self.d.index(b"\x00", self.p, self.end)
        s = self.d[self.p:z].decode("utf-8", "replace")
        self.p = z + 1
        return s

    def var(self):
        n = self.cint()
        if n == 0:
            return VOID
        m = self.take(1)[0]
        body = n - 1
        if m == 1: return I(struct.unpack("<i", self.take(4))[0])
        if m in (2, 3): return B(m == 2)
        if m == 4: return D(struct.unpack("<d", self.take(8))[0])
        if m == 6: return L(struct.unpack("<q", self.take(8))[0])
        if m == 5: return S(self.take(body).rstrip(b"\x00").decode("utf-8", "replace"))
        if m == 8: return BIN(self.take(body))
        if m == 9: return V("undefined")
        if m == 7:
            sub = Reader(self.d, self.p, self.p + body)
            items = [sub.var() for _ in range(sub.cint())]
            self.p += body
            return A(items)
        raise ValueError(f"marker {m}")

    def node(self, depth=0):
        if depth > 64:
            raise ValueError("too deep")
        n = Node(self.cstr())
        for _ in range(self.cint()):
            name = self.cstr()
            n.props.append((name, self.var()))
        n.children = [self.node(depth + 1) for _ in range(self.cint())]
        return n


def decode_tree(data):
    r = Reader(data)
    n = r.node()
    if r.p != len(data):
        raise ValueError(f"{len(data) - r.p} trailing bytes")
    return n


def read_file(data):
    """(root node, compressed?) of a .prj / .rsm (container) or a .wav.dat (plain tree)."""
    if data[:8] == MAGIC:
        return decode_tree(zlib.decompress(data[8:])), True
    return decode_tree(data), False


# ---------------------------------------------------------------------------------------------- blobs
def none_index(slot):
    """The index byte of an empty word: the slot for 0-22; 5 + sub-slot k for the FX slots 23-34 (the empty-cell form
    a real project uses)."""
    return slot if slot < 23 else 5 + (slot - 23) % 6


def none_word(slot):
    return 0x3F800000 | none_index(slot)


def pack_word(value22, idx, vct=0):
    return ((vct & 3) << 30) | ((value22 & 0x3FFFFF) << 8) | (idx & 0xFF)


def unpack_word(w):
    v = (w >> 8) & 0x3FFFFF
    if v & 0x200000:
        v -= 0x400000
    return {"vct": w >> 30, "value": v, "idx": w & 0xFF, "empty": (w >> 30) == 0 and v == -32768}


def empty_record():
    return [-127, -126, 15] + [none_word(s) for s in range(WORDS)]


def record_index(step, track):
    """The file position of the record of (step, track), both 0-based: track-major."""
    return track * STEPS + step


def programmed_record(step, track, typ=15):
    """A programmed record without locks: its first two ints are its own track and step."""
    return [track, step, typ] + [none_word(s) for s in range(WORDS)]


def steps_blob(records):
    """records: dict (step, track) -> [first, second, type, 35 words] (0-based); others empty. Written track-major."""
    out = bytearray()
    for k in range(STEPS * TRACKS):
        t, s = divmod(k, STEPS)
        r = records.get((s, t)) or empty_record()
        out += struct.pack("<iii", *r[:3]) + struct.pack("<35I", *r[3:])
    assert len(out) == STEPS_BYTES
    return zlib.compress(bytes(out), 6)


def decode_steps(blob):
    raw = zlib.decompress(blob)
    if len(raw) != STEPS_BYTES:
        raise ValueError(f"step matrix of {len(raw)} bytes")
    cells = {}
    for k in range(STEPS * TRACKS):
        o = k * RECORD
        trig, v2, typ = struct.unpack_from("<iii", raw, o)
        words = struct.unpack_from("<35I", raw, o + 12)
        cells[(k % STEPS, k // STEPS)] = (trig, v2, typ, words)  # track-major: k = track * 64 + step
    return cells


def sections_blob(records=None):
    recs = [(65536, 0)] * SECTIONS
    for i, r in (records or {}).items():
        recs[i] = tuple(r)
    return zlib.compress(b"".join(struct.pack("<ii", a, b) for a, b in recs), 6)


def wav_dat(bar, bpm, sample_num):
    return enc_node(Node("SampleAdjunctData", [("bar", D(bar)), ("bpm", D(bpm)), ("sampleNum", I(sample_num))]))


# ---------------------------------------------------------------------------------------------- JSON view
def to_json(v):
    k = v.kind
    if k in ("void", "undefined"): return {k: None}
    if k == "binary": return {"binary": base64.b64encode(v.value).decode("ascii"), "bytes": len(v.value)}
    if k == "array": return {"array": [to_json(e) for e in v.value]}
    return {k: v.value}


def node_json(n):
    out = {"type": n.type, "props": {name: to_json(v) for name, v in n.props}}
    if n.children:
        out["children"] = [node_json(c) for c in n.children]
    return out
