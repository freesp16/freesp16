"""Seeded, generated input signals for goldens (never recordings).

Every signal is computed with IEEE-754 double +, -, *, / only (own sine and 2^x series, no libm), then rounded once to
float32, so a recipe gives bit-identical samples on every platform and Python version: `gen.py --check` compares
regenerated files bit for bit on macOS and in Linux CI.

A stimulus spec is a JSON object; `type` selects the generator:
  {"type": "silence"}
  {"type": "dc", "level": 0.5}
  {"type": "impulse", "at": [0, 100], "amplitude": 1.0}             unit impulses at the given frames
  {"type": "step", "at": 100, "level": 0.5}                          0 before `at`, `level` from `at` on
  {"type": "noise", "seed": 1, "amplitude": 0.5}                     uniform white noise, xorshift32 (seed != 0)
  {"type": "sine", "freq": 1000.0, "amplitude": 0.5, "phase": 0.0}   phase in cycles
  {"type": "sweep", "f0": 20.0, "octaves": 10.0, "amplitude": 0.5}   exponential sweep over the whole signal
  {"type": "sweep", "f0": 20.0, "f1": 20000.0, "mode": "linear", "amplitude": 0.5}
  {"type": "sum", "parts": [spec, spec, ...]}                        sum of the parts (each rounded, then summed in double)
Optional on every spec: "gain" (multiplies the result), "start"/"length" (frames; zero outside).
"""
import math

from .f32 import F

_PI = 3.141592653589793
_HALF_PI = 1.5707963267948966
_LN2 = 0.6931471805599453
_SIN_COEF = [(-1.0 if (k // 2) % 2 else 1.0) / math.factorial(k) for k in range(1, 24, 2)]  # x, -x^3/3!, ...
_EXP_COEF = [1.0 / math.factorial(k) for k in range(0, 26)]


def sin2pi(phase):
    """sin(2*pi*phase) with |error| < 1e-15, using only +, -, *, / (deterministic everywhere)."""
    p = phase - math.floor(phase)          # [0, 1)
    if p >= 0.5:
        p -= 1.0                           # [-0.5, 0.5)
    x = 2.0 * _PI * p                      # [-pi, pi)
    if x > _HALF_PI:
        x = _PI - x
    elif x < -_HALF_PI:
        x = -_PI - x
    x2 = x * x
    acc = 0.0
    for c in reversed(_SIN_COEF):
        acc = acc * x2 + c
    return acc * x


def exp2(y):
    """2**y with a relative error < 1e-15, using only +, -, *, / and the exact ldexp."""
    n = math.floor(y)
    f = (y - n) * _LN2                     # [0, ln 2)
    acc = 0.0
    for c in reversed(_EXP_COEF):
        acc = acc * f + c
    return math.ldexp(acc, int(n))


def _xorshift32(seed):
    x = seed & 0xFFFFFFFF
    if x == 0:
        raise ValueError("noise seed must be non-zero")
    while True:
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        yield x


def _raw(spec, frames, fs):
    t = spec["type"]
    if t == "silence":
        return [0.0] * frames
    if t == "dc":
        return [float(spec["level"])] * frames
    if t == "impulse":
        out = [0.0] * frames
        for at in spec.get("at", [0]):
            if 0 <= at < frames:
                out[at] = float(spec.get("amplitude", 1.0))
        return out
    if t == "step":
        at = int(spec["at"])
        return [0.0 if i < at else float(spec["level"]) for i in range(frames)]
    if t == "noise":
        a = float(spec.get("amplitude", 1.0))
        g = _xorshift32(int(spec["seed"]))
        return [a * ((next(g) / 4294967296.0) * 2.0 - 1.0) for _ in range(frames)]
    if t == "sine":
        a = float(spec.get("amplitude", 1.0)); f = float(spec["freq"]); ph = float(spec.get("phase", 0.0))
        return [a * sin2pi(ph + f * i / fs) for i in range(frames)]
    if t == "sweep":
        a = float(spec.get("amplitude", 1.0)); f0 = float(spec["f0"])
        mode = spec.get("mode", "log")
        out = []; ph = float(spec.get("phase", 0.0))
        for i in range(frames):
            out.append(a * sin2pi(ph))
            if mode == "log":
                f = f0 * exp2(float(spec["octaves"]) * i / frames)
            elif mode == "linear":
                f = f0 + (float(spec["f1"]) - f0) * i / frames
            else:
                raise ValueError("sweep mode must be log or linear: %r" % mode)
            ph += f / fs
            ph -= math.floor(ph)
        return out
    if t == "sum":
        parts = [generate(p, frames, fs) for p in spec["parts"]]
        return [sum(v) for v in zip(*parts)]
    raise ValueError("unknown stimulus type %r" % t)


def generate(spec, frames, fs=44100.0):
    """The stimulus as a list of `frames` float32-representable values."""
    x = _raw(spec, frames, fs)
    g = float(spec.get("gain", 1.0))
    start = int(spec.get("start", 0)); length = spec.get("length")
    end = frames if length is None else min(frames, start + int(length))
    out = []
    for i, v in enumerate(x):
        v = F(v * g) if start <= i < end else 0.0
        if v != 0.0 and abs(v) < 1.1754943508222875e-38:
            raise ValueError("stimulus produced a denormal at frame %d (goldens use normal input)" % i)
        out.append(v)
    return out
