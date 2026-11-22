#!/usr/bin/env python3
"""Check a WAV recorded by the emulator (--audio file). Two modes:

Tone: per-channel RMS / peak in dBFS and dominant frequency (zero crossings); exit 1 when the expected tone is
missing.
  tools/analyze-wav.py FILE.wav [--channels 0,2] [--expect-hz 440] [--from-s 2] [--seconds 3]

Kit (the pad/kit check): the 16 pads were hit once each, in
order, with the generated kit (tools/gen-test-kit.py) on tracks 1-16. Checks on MAIN (the named channels):
  - 16 onsets after the last long sound (the fallback test tone sounds until the kit is installed), each matching its
    kit sample: normalised cross-correlation of the recorded segment with the kit file >= --min-corr (default 0.9),
    and that sample correlating best of all 16 at that position (16 distinct hits, one per pad, in pad order);
  - with --spacing-ms: consecutive onsets that far apart, within --spacing-tol-ms (default 40: UI frame + period);
  - no sample above --max-peak-dbfs (default -0.3 dBFS);
  - click check: no sample-to-sample step above --click-dbfs (default -40 dBFS) outside the sounding segments
    (onset .. onset + sample length + 5 ms), from the end of the long sound on.
  tools/analyze-wav.py FILE.wav --channels 0,2 --kit KIT.json [--kit-dir DIR] [--from-s S] [--seconds N]
                       [--spacing-ms MS [--spacing-tol-ms 40]]
--from-s starts the window (onsets, peak and clicks all count from there). Exit 0 = OK, 1 = a check failed.

The device's master stream is 16 channels with MAIN L/R on channels 12 and 14; the emulator's recorded WAV keeps
channels 12..15 only, so MAIN is WAV channels 0 and 2 (sp16-emulator docs/audio-jack-map.md). The Linux host's `file`
audio device writes MAIN as WAV channels 0 and 1 (--channels 0,1).
Standard library only (wave, struct, math, json)."""
import argparse
import json
import math
import os
import struct
import sys
import wave


def db(v: float) -> float:
    return 20 * math.log10(v) if v > 0 else float("-inf")


def read(path: str, from_s, seconds):
    with wave.open(path, "rb") as w:
        ch, width, rate, total = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        n = total if seconds is None else min(total, int(seconds * rate))
        start = min(int(from_s * rate), total) if from_s is not None else max(0, total - n)
        n = min(n, total - start)
        w.setpos(start)
        raw = w.readframes(n)
    if width != 2:
        raise SystemExit(f"{path}: {width * 8}-bit samples not supported")
    frames = len(raw) // (2 * ch)
    data = struct.unpack("<%dh" % (frames * ch), raw[: frames * ch * 2])
    return ch, rate, total, start, frames, data


def tone_mode(a) -> int:
    ch, rate, total, start, frames, data = read(a.wav, a.from_s, a.seconds)
    print(f"{a.wav}: {ch} ch, {rate} Hz, {total / rate:.1f} s; window {start / rate:.1f}-{(start + frames) / rate:.1f} s")
    ok = True
    for c in [int(x) for x in a.channels.split(",")]:
        if c >= ch:
            print(f"  ch{c}: absent")
            ok = False
            continue
        s = data[c::ch]
        rms = math.sqrt(sum(x * x for x in s) / max(1, len(s))) / 32768.0
        peak = max((abs(x) for x in s), default=0) / 32768.0
        crossings = sum(1 for i in range(1, len(s)) if (s[i - 1] < 0) != (s[i] < 0))
        hz = crossings / 2 / (len(s) / rate) if s else 0.0
        print(f"  ch{c}: rms {db(rms):6.1f} dBFS  peak {db(peak):6.1f} dBFS  ~{hz:7.1f} Hz")
        if a.expect_hz and (rms < 1e-4 or abs(hz - a.expect_hz) > a.expect_hz * 0.05):
            ok = False
    if a.expect_hz:
        print("  tone check:", "OK" if ok else "FAILED", f"(expected ~{a.expect_hz:g} Hz)")
    return 0 if ok else 1


def read_mono(path: str):
    """A kit file as mono floats (mean of its channels) and its rate."""
    with wave.open(path, "rb") as w:
        ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    d = struct.unpack("<%dh" % (n * ch), raw[: n * ch * 2])
    return [sum(d[i * ch:(i + 1) * ch]) / (ch * 32768.0) for i in range(n)], rate


def ncc(x, y) -> float:
    """Normalised cross-correlation at lag 0 (equal lengths)."""
    sxy = sum(p * q for p, q in zip(x, y))
    sxx = sum(p * p for p in x)
    syy = sum(q * q for q in y)
    return sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else 0.0


def best_ncc(seg, ref, max_lag: int, rate: int):
    """The best correlation of `ref` within `seg` over lags 0..max_lag: the lag from the first 30 ms (every lag, so
    noise bursts align too), then the whole sample at that lag."""
    n = len(ref)
    head = ref[: max(1, int(0.030 * rate))]
    at, best_head = 0, -2.0
    for lag in range(0, max_lag + 1):
        part = seg[lag:lag + len(head)]
        if len(part) < len(head):
            break
        c = ncc(part, head)
        if c > best_head:
            best_head, at = c, lag
    part = seg[at:at + n]
    return (ncc(part, ref) if len(part) == n else 0.0), at


def kit_mode(a) -> int:
    ch, rate, total, start, frames, data = read(a.wav, a.from_s, a.seconds)
    chans = [int(x) for x in a.channels.split(",")]
    if any(c >= ch for c in chans):
        print(f"{a.wav}: channels {a.channels} not all present ({ch} ch)")
        return 1
    mains = [data[c::ch] for c in chans]
    # MAIN as the per-frame mean of the named channels (the kit is mono, centred), peak over each channel.
    main = [sum(m[i] for m in mains) / (len(mains) * 32768.0) for i in range(frames)]
    peak = max(max((abs(x) for x in m), default=0) for m in mains) / 32768.0
    print(f"{a.wav}: {ch} ch, {rate} Hz, {total / rate:.1f} s; window {start / rate:.1f}-{(start + frames) / rate:.1f} s")

    with open(a.kit) as f:
        kit = json.load(f)
    refs = []
    for s in kit["samples"]:
        mono, r = read_mono(os.path.join(a.kit_dir, s["name"]))
        if r != rate:
            print(f"  kit {s['name']}: {r} Hz but the recording is {rate} Hz (no resampling here)")
            return 1
        refs.append((s, mono))

    # Envelope in 2 ms windows; a segment = a run of windows above the gate, joined over gaps < 20 ms.
    win = max(1, int(0.002 * rate))
    gate = 10 ** (a.gate_dbfs / 20)
    env = [max((abs(x) for x in main[i:i + win]), default=0.0) for i in range(0, frames, win)]
    segs = []
    i = 0
    join = int(0.020 * rate / win) + 1
    while i < len(env):
        if env[i] < gate:
            i += 1
            continue
        j, quiet = i, 0
        while j < len(env) and quiet < join:
            quiet = quiet + 1 if env[j] < gate else 0
            j += 1
        segs.append((i * win, (j - quiet) * win))
        i = j
    long_end = 0
    for s0, s1 in segs:
        if s1 - s0 > a.long_s * rate:
            long_end = max(long_end, s1)
    hits = [(s0, s1) for s0, s1 in segs if s0 >= long_end and s1 - s0 <= a.long_s * rate]
    if long_end:
        print(f"  a long sound (the fallback test tone) ends at {(start + long_end) / rate:.2f} s")
    print(f"  onsets after it: {len(hits)} (expected {len(refs)})")

    ok = len(hits) == len(refs)
    matched = 0
    lead = int(0.004 * rate)  # the gate opens a little after the true start
    starts = []
    for k, (s0, s1) in enumerate(hits):
        hit_peak = max(max((abs(x) for x in m[s0:s1]), default=0) for m in mains) / 32768.0
        line = f"  onset {k + 1:2d} at {(start + s0) / rate:8.3f} s, {1000 * (s1 - s0) / rate:4.0f} ms, max {db(hit_peak):6.2f} dBFS"
        true0 = s0
        if k < len(refs):
            spec, ref = refs[k]
            seg = main[max(0, s0 - lead):s0 + len(ref) + 2 * lead]
            c, lag = best_ncc(seg, ref, 3 * lead, rate)
            true0 = max(0, s0 - lead) + lag
            # Distinct: at that position no other kit sample matches better (a wrong pad or a repeated sample fails).
            best_k, best_c = k, c
            for j, (_, other) in enumerate(refs):
                if j != k:
                    part = main[true0:true0 + len(other)]
                    cj = ncc(part, other) if len(part) == len(other) else 0.0
                    if cj > best_c:
                        best_k, best_c = j, cj
            if c < a.min_corr:  # which sample was it? each one at its own best lag (only on this failure path)
                for j, (_, other) in enumerate(refs):
                    cj, _ = best_ncc(main[max(0, s0 - lead):s0 + len(other) + 2 * lead], other, 3 * lead, rate)
                    if cj > best_c:
                        best_k, best_c = j, cj
            good = c >= a.min_corr and best_k == k
            matched += 1 if good else 0
            verdict = "OK" if good else ("MISMATCH" if best_k == k else f"MISMATCH (best {refs[best_k][0]['name']})")
            line += f"  {spec['name']:<24} corr {c:5.3f} {verdict}"
            ok = ok and good
        starts.append(true0)
        print(line)
    if len(starts) > 1:
        gaps = [(b - p) * 1000 / rate for p, b in zip(starts, starts[1:])]
        print(f"  onset spacing ms: min {min(gaps):.0f} max {max(gaps):.0f}")
        if a.spacing_ms is not None:
            spacing_ok = all(abs(g - a.spacing_ms) <= a.spacing_tol_ms for g in gaps)
            print(f"  spacing {a.spacing_ms:g} ms +- {a.spacing_tol_ms:g}: {'OK' if spacing_ok else 'FAILED'}")
            ok = ok and spacing_ok

    # Click check outside the sounding segments, from the end of the long sound (+50 ms) on.
    sounding = bytearray(frames)
    for k, s0 in enumerate(starts):
        n = len(refs[k][1]) if k < len(refs) else hits[k][1] - hits[k][0]
        lo, hi = max(0, s0 - lead), min(frames, s0 + n + int(0.005 * rate))
        sounding[lo:hi] = b"\x01" * (hi - lo)
    first = max(1, min(frames, long_end + int(0.05 * rate)))
    click, click_at = 0.0, 0
    for m in mains:
        for x in range(first, frames):
            if not sounding[x]:
                step = abs(m[x] - m[x - 1])
                if step > click:
                    click, click_at = step, x
    click /= 32768.0
    peak_ok = db(peak) <= a.max_peak_dbfs
    click_ok = db(click) <= a.click_dbfs
    print(f"  peak {db(peak):6.2f} dBFS (limit {a.max_peak_dbfs:g}): {'OK' if peak_ok else 'FAILED'}")
    print(f"  click: largest step outside the samples {db(click):6.1f} dBFS at {(start + click_at) / rate:.3f} s "
          f"(limit {a.click_dbfs:g}): {'OK' if click_ok else 'FAILED'}")
    ok = ok and peak_ok and click_ok
    print(f"  kit check: {'OK' if ok else 'FAILED'} ({matched}/{len(refs)} samples matched)")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--channels", default="0,2")
    ap.add_argument("--expect-hz", type=float, default=None)
    ap.add_argument("--from-s", type=float, default=None, help="start of the window (tone default: last --seconds)")
    ap.add_argument("--seconds", type=float, default=None, help="window length (tone default 3 s; kit: to the end)")
    ap.add_argument("--kit", default=None, help="kit manifest (tools/gen-test-kit.py --json): kit mode")
    ap.add_argument("--kit-dir", default=None, help="folder of the kit's WAVs (default: the manifest's folder)")
    ap.add_argument("--min-corr", type=float, default=0.9)
    ap.add_argument("--max-peak-dbfs", type=float, default=-0.3)
    ap.add_argument("--click-dbfs", type=float, default=-40.0)
    ap.add_argument("--gate-dbfs", type=float, default=-50.0, help="onset gate on MAIN")
    ap.add_argument("--long-s", type=float, default=1.0, help="a sound longer than this is not a pad hit")
    ap.add_argument("--spacing-ms", type=float, default=None, help="kit: expected spacing of consecutive onsets")
    ap.add_argument("--spacing-tol-ms", type=float, default=40.0)
    a = ap.parse_args()
    if a.kit:
        a.kit_dir = a.kit_dir or os.path.dirname(os.path.abspath(a.kit))
        if a.from_s is None:
            a.from_s = 0.0
        return kit_mode(a)
    if a.seconds is None:
        a.seconds = 3.0
    return tone_mode(a)


if __name__ == "__main__":
    sys.exit(main())
