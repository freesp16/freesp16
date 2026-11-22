#!/usr/bin/env python3
"""The pad/kit check's tools: tools/gen-test-kit.py writes the 16-sample kit into a data directory (Samples/TestKit,
the app's dev_kit folder) and the host smoke's pad script; tools/analyze-wav.py --kit checks a recording of MAIN (WAV
channels 0,1 here, as the Linux `file` audio device writes it) from a start time. A synthetic recording — the fallback
test tone first, then the 16 kit samples at the script's times — must pass; a wrong pad, a clipped hit, a click, a
missing hit and a late hit must each fail."""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import wave

RATE = 44100


def read_mono(path):
    with wave.open(str(path), "rb") as w:
        ch, n = w.getnchannels(), w.getnframes()
        d = struct.unpack("<%dh" % (n * ch), w.readframes(n))
    return [sum(d[i * ch:(i + 1) * ch]) / (ch * 32768.0) for i in range(n)]


def write_recording(path, samples, hits, seconds, gain=0.6, extra=None):
    """4-channel WAV: MAIN on 0/1 (the kit centred: equal gain), silence on 2/3. hits = [(time_s, sample_index)]."""
    n = int(seconds * RATE)
    main = [0.0] * n
    for i in range(int(0.6 * RATE)):  # the fallback 440 Hz test tone until the kit is installed (−12 dBFS)
        main[i] = 0.25 * __import__("math").sin(2 * 3.141592653589793 * 440 * i / RATE)
    for t, k in hits:
        s0 = int(t * RATE)
        for i, x in enumerate(samples[k]):
            if s0 + i < n:
                main[s0 + i] += gain * x
    for at, v in (extra or []):
        main[int(at * RATE)] += v
    frames = bytearray()
    for x in main:
        v = max(-32768, min(32767, int(round(x * 32767))))
        frames += struct.pack("<hhhh", v, v, 0, 0)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(4)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))


def main() -> int:
    tools = pathlib.Path(os.environ.get("APP_SOURCE_DIR", pathlib.Path(__file__).resolve().parents[3])) / "tools"
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        data = tmp / "data"
        script = tmp / "input.txt"
        manifest = tmp / "kit.json"
        gen = subprocess.run([sys.executable, str(tools / "gen-test-kit.py"), "--data-dir", str(data), "--json",
                              str(manifest), "--input-script", str(script)], capture_output=True, text=True)
        print(gen.stdout, gen.stderr)
        kit = data / "Samples" / "TestKit"
        wavs = sorted(kit.glob("*.wav"))
        if gen.returncode != 0 or len(wavs) != 16 or "dev_kit=Samples/TestKit" not in gen.stdout:
            print("FAILED: gen-test-kit --data-dir")
            return 1
        lines = script.read_text().split("\n")
        presses = [l.split() for l in lines if " press " in l]
        if [p[2] for p in presses] != [f"pad{k}" for k in range(1, 17)] or not lines[-2].endswith(" quit") \
                or any(" release " not in l for l in lines[1:-2:2]):
            print("FAILED: input script", lines[:4], lines[-3:])
            return 1
        times = [int(p[0]) / 1000.0 for p in presses]  # 2.0, 2.25, … (the host clock = the recording's clock here)
        samples = [read_mono(w) for w in wavs]

        def analyze(name, hits, expect_ok, **kw):
            wav = tmp / f"{name}.wav"
            write_recording(wav, samples, hits, times[-1] + 1.5, **kw)
            r = subprocess.run([sys.executable, str(tools / "analyze-wav.py"), str(wav), "--channels", "0,1", "--kit",
                                str(manifest), "--kit-dir", str(kit), "--from-s", "1.5", "--spacing-ms", "250"],
                               capture_output=True, text=True)
            ok = r.returncode == 0
            print(f"--- {name}: exit {r.returncode} (expected {'0' if expect_ok else '1'})")
            print(r.stdout[-1800:], r.stderr[-800:])
            if ok != expect_ok:
                failures.append(name)

        good = [(t, k) for k, t in enumerate(times)]
        analyze("clean", good, True)
        analyze("wrong-pad", [(t, 5 if k == 4 else k) for t, k in good], False)  # pad 5 played sample 6
        analyze("missing-hit", good[:-1], False)
        analyze("late-hit", [(t + (0.1 if k == 8 else 0.0), k) for t, k in good], False)
        analyze("clipped", good, False, gain=2.0)  # the kit peaks at −6 dBFS: ×2 reaches full scale (> −0.3 dBFS)
        # A click 12 ms after pad 4's sample ends: inside its segment (gaps < 20 ms join) so still 16 onsets, but
        # outside the sample (+5 ms): only the click check catches it. A click in silence is an extra onset instead.
        analyze("click", good, False, extra=[(times[3] + 0.100 + 0.012, 0.05)])
        analyze("click-in-silence", good, False, extra=[(times[-1] + 0.6, 0.05)])
    if failures:
        print("FAILED:", ", ".join(failures))
        return 1
    print("kit tools OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
