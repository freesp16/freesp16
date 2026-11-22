#!/usr/bin/env python3
"""Generate the pad/kit check's demo kit: 16 distinct short
one-shots, synthesised here — never device data. Each file is 16-bit stereo 44.1 kHz, 0.10 s long (the smoke's
hits, 500 ms apart, do not overlap even with the emulator's input jitter), peak -6 dBFS, with a 2 ms raised-cosine
fade-in and a 15 ms fade-out (so the samples themselves never click):

  01-08  sine tones, one octave of a C major scale from C4 (261.6 Hz) up to C5, plus a 5 % second harmonic
  09-12  decaying noise bursts (seeded LCG, different seeds and decay rates)
  13-16  two-tone chords and a downward chirp (kick-like) for timbre variety

  tools/gen-test-kit.py OUT_DIR [--json KIT.json]
  tools/gen-test-kit.py --data-dir DATA_DIR [--json KIT.json] [--input-script FILE [--first-ms 2000]
                        [--spacing-ms 250] [--hold-ms 50] [--velocity 110] [--tail-ms 1500]]

--data-dir writes the kit into DATA_DIR/Samples/TestKit, the folder the app's dev_kit=Samples/TestKit setting loads
(<PREFIX>_DATA_DIR = DATA_DIR, <PREFIX>_DEV_KIT = Samples/TestKit; relative dev_kit paths are under the data dir).
--input-script writes the pad script of the host smoke's scripted input (<PREFIX>_INPUT_SCRIPT): pads 1-16 once each,
in order, `<ms> press padN <velocity>` then `<ms + hold> release padN`, `spacing` ms apart from `first` ms, and
`<ms> quit` `tail` ms after the last hit. It prints the analysis start (--from-s) to pass to tools/analyze-wav.py.

The JSON manifest lists each file's name, kind, fundamental (Hz or null), peak and length; tools/analyze-wav.py
--kit KIT.json uses it to check the 16 onsets on MAIN. Standard library only."""
import argparse
import json
import math
import os
import struct
import sys
import wave

RATE = 44100
SECONDS = 0.10
PEAK_DBFS = -6.0
FADE_IN_S = 0.002
FADE_OUT_S = 0.015

C_MAJOR = [261.63, 293.66, 329.63, 349.23, 392.00, 440.00, 493.88, 523.25]


def envelope(i: int, n: int) -> float:
    fi, fo = int(FADE_IN_S * RATE), int(FADE_OUT_S * RATE)
    g = 1.0
    if i < fi:
        g *= 0.5 - 0.5 * math.cos(math.pi * i / fi)
    if i >= n - fo:
        g *= 0.5 - 0.5 * math.cos(math.pi * (n - 1 - i) / fo)
    return g


def lcg(seed: int):
    x = seed & 0xFFFFFFFF
    while True:
        x = (1664525 * x + 1013904223) & 0xFFFFFFFF
        yield (x / 2147483648.0) - 1.0


def render(kind: str, arg) -> list:
    n = int(SECONDS * RATE)
    out = []
    if kind == "tone":
        f = arg
        out = [math.sin(2 * math.pi * f * i / RATE) + 0.05 * math.sin(4 * math.pi * f * i / RATE) for i in range(n)]
    elif kind == "noise":
        seed, decay = arg
        g = lcg(seed)
        out = [next(g) * math.exp(-i / (decay * RATE)) for i in range(n)]
    elif kind == "chord":
        f1, f2 = arg
        out = [math.sin(2 * math.pi * f1 * i / RATE) + math.sin(2 * math.pi * f2 * i / RATE) for i in range(n)]
    elif kind == "chirp":
        f0, f1 = arg
        phase = 0.0
        for i in range(n):
            f = f1 + (f0 - f1) * math.exp(-i / (0.03 * RATE))
            phase += 2 * math.pi * f / RATE
            out.append(math.sin(phase) * math.exp(-i / (0.08 * RATE)))
    peak = max(abs(x) for x in out) or 1.0
    gain = 10 ** (PEAK_DBFS / 20) / peak
    return [x * gain * envelope(i, n) for i, x in enumerate(out)]


def kit() -> list:
    items = []
    for k, f in enumerate(C_MAJOR):
        items.append((f"{k + 1:02d}-tone-{int(round(f))}hz.wav", "tone", f, f))
    for k, (seed, decay) in enumerate([(11, 0.02), (23, 0.04), (37, 0.07), (51, 0.12)]):
        items.append((f"{k + 9:02d}-noise-{int(decay * 1000)}ms.wav", "noise", (seed, decay), None))
    items.append(("13-chord-330-415hz.wav", "chord", (329.63, 415.30), 329.63))
    items.append(("14-chord-587-880hz.wav", "chord", (587.33, 880.00), 587.33))
    items.append(("15-chirp-180-50hz.wav", "chirp", (180.0, 50.0), None))
    items.append(("16-chirp-900-300hz.wav", "chirp", (900.0, 300.0), None))
    return items


def write(path: str, samples: list) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for x in samples:
            v = max(-32768, min(32767, int(round(x * 32767))))
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))


KIT_SUBDIR = os.path.join("Samples", "TestKit")  # = the app's dev_kit setting, relative to the data directory


def write_input_script(path: str, first_ms: int, spacing_ms: int, hold_ms: int, velocity: int, tail_ms: int,
                       pads: int) -> float:
    """The host smoke's pad script; returns the analysis start in seconds (500 ms before the first hit)."""
    lines = []  # no comment line: the script format is only `<ms> press|release <control> [velocity]` / `<ms> quit`
    for k in range(pads):
        t = first_ms + k * spacing_ms
        lines.append(f"{t} press pad{k + 1} {velocity}")
        lines.append(f"{t + hold_ms} release pad{k + 1}")
    lines.append(f"{first_ms + (pads - 1) * spacing_ms + tail_ms} quit")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return max(0.0, first_ms / 1000.0 - 0.5)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir", nargs="?", default=None, help="kit folder (or use --data-dir)")
    ap.add_argument("--data-dir", default=None, help="the app's data directory: the kit goes to DATA_DIR/" + KIT_SUBDIR)
    ap.add_argument("--json", default=None, help="manifest path (default OUT_DIR/kit.json)")
    ap.add_argument("--input-script", default=None, help="also write the host smoke's pad script here")
    ap.add_argument("--first-ms", type=int, default=2000, help="first hit, ms after the app's input clock starts")
    ap.add_argument("--spacing-ms", type=int, default=250, help="hit spacing (default 250 ms; samples are 100 ms)")
    ap.add_argument("--hold-ms", type=int, default=50)
    ap.add_argument("--velocity", type=int, default=110)
    ap.add_argument("--tail-ms", type=int, default=1500, help="quit this long after the last hit")
    a = ap.parse_args()
    if (a.out_dir is None) == (a.data_dir is None):
        ap.error("give either OUT_DIR or --data-dir")
    out_dir = a.out_dir if a.out_dir is not None else os.path.join(a.data_dir, KIT_SUBDIR)
    if a.spacing_ms < 1000 * SECONDS + 20:
        ap.error(f"--spacing-ms {a.spacing_ms}: the {1000 * SECONDS:.0f} ms samples would overlap")
    os.makedirs(out_dir, exist_ok=True)
    manifest = []
    for name, kind, arg, hz in kit():
        s = render(kind, arg)
        write(os.path.join(out_dir, name), s)
        manifest.append({"name": name, "kind": kind, "hz": hz, "seconds": SECONDS, "peak_dbfs": PEAK_DBFS})
    path = a.json or os.path.join(out_dir, "kit.json")
    with open(path, "w") as f:
        json.dump({"rate": RATE, "samples": manifest}, f, indent=1)
    print(f"wrote {len(manifest)} samples to {out_dir} (manifest {path})")
    if a.data_dir is not None:
        print(f"dev_kit={KIT_SUBDIR}")
    if a.input_script:
        from_s = write_input_script(a.input_script, a.first_ms, a.spacing_ms, a.hold_ms, a.velocity, a.tail_ms,
                                    len(manifest))
        print(f"input script {a.input_script}: first hit {a.first_ms} ms, {a.spacing_ms} ms apart; "
              f"analyze-wav --from-s {from_s:g} --spacing-ms {a.spacing_ms}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
