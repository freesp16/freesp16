#!/usr/bin/env python3
"""Self-test of tools/golden-gen (ctest "golden: gen self-test"): deterministic stimuli, the file format, the app gain
table, and the research guard (no verify/, no firmware-table loaders, no file reads). Research-dependent tests are
skipped when the research models are absent; exit 77 only if nothing could run."""
import math
import os
import pathlib
import sys
import unittest

ROOT = pathlib.Path(os.environ.get("APP_SOURCE_DIR") or pathlib.Path(__file__).resolve().parents[3])
sys.path.insert(0, str(ROOT / "tools" / "golden-gen"))
sys.dont_write_bytecode = True

from goldenlib import applaw, f32, research, stimuli  # noqa: E402

try:
    RESEARCH = research.find_research(ROOT)
except research.ResearchMissing:
    RESEARCH = None


class Stimuli(unittest.TestCase):
    def test_sin2pi_accuracy(self):
        for i in range(2000):
            p = i * 0.000731 - 0.3
            self.assertAlmostEqual(stimuli.sin2pi(p), math.sin(2 * math.pi * p), delta=1e-14)

    def test_exp2_accuracy(self):
        for i in range(-200, 200):
            y = i * 0.0731
            self.assertLess(abs(stimuli.exp2(y) / 2.0 ** y - 1.0), 1e-14)

    def test_xorshift_reference(self):
        # xorshift32 (13, 17, 5) from seed 1: 270369, 67634689, 2647435461 (Marsaglia 2003)
        g = stimuli._xorshift32(1)
        self.assertEqual([next(g) for _ in range(3)], [270369, 67634689, 2647435461])

    def test_values_are_float32_and_deterministic(self):
        spec = {"type": "sum", "parts": [{"type": "sweep", "f0": 20.0, "octaves": 10.0, "amplitude": 0.5},
                                         {"type": "noise", "seed": 9, "amplitude": 0.25}]}
        a = stimuli.generate(spec, 4096); b = stimuli.generate(spec, 4096)
        self.assertEqual(a, b)
        self.assertTrue(all(f32.F(v) == v for v in a))

    def test_impulse_step_window(self):
        x = stimuli.generate({"type": "impulse", "at": [0, 5], "amplitude": 0.5, "start": 1}, 8)
        self.assertEqual(x, [0.0, 0, 0, 0, 0, 0.5, 0, 0])
        self.assertEqual(stimuli.generate({"type": "step", "at": 2, "level": 1.0}, 4), [0, 0, 1.0, 1.0])

    def test_file_format_round_trip(self):
        ch = [[1.0, -0.5, 0.25], [0.0, 2.0, -3.0]]
        data = f32.to_bytes(ch)
        self.assertEqual(len(data), 24)
        self.assertEqual(data[:4], b"\x00\x00\x80\x3f")       # 1.0f little-endian, channel-major
        self.assertEqual(f32.from_bytes(data, 2), ch)


class GainTable(unittest.TestCase):
    def test_spec_points(self):
        t = applaw.gain_table()
        self.assertEqual(t[0], 0.0)
        self.assertEqual(t[100], 1.0)
        self.assertEqual(t[127], f32.F(2.0 ** (math.log(1.27) / math.log(1.27))))
        self.assertAlmostEqual(20 * math.log10(t[1]), -54.0, places=5)
        self.assertAlmostEqual(20 * math.log10(t[9]), -41.0, places=5)
        self.assertAlmostEqual(20 * math.log10(t[5]), -47.5, places=5)   # interpolated, the ladder says -45


@unittest.skipIf(RESEARCH is None, "research models not found")
class Guard(unittest.TestCase):
    def test_forbidden_loaders_are_stubbed(self):
        with research.Research(RESEARCH) as r:
            dev = r.load("device_out")
            with self.assertRaises(research.ForbiddenInput):
                dev.load_table()
            ex = r.load("glibc_expf")
            with self.assertRaises(research.ForbiddenInput):
                ex.load_tables()

    def test_verify_lief_and_open_refused(self):
        with research.Research(RESEARCH):
            with self.assertRaises(ImportError):
                __import__("verify")
            with self.assertRaises(research.ForbiddenInput):
                __import__("lief")
            with self.assertRaises(research.ForbiddenInput):
                open(str(ROOT / "LICENSE"))
        open(str(ROOT / "LICENSE")).close()                    # restored afterwards

    def test_served_files_carry_licence_and_hash(self):
        with research.Research(RESEARCH) as r:
            r.load("track_mix")
            served = r.served()
        self.assertEqual([s["file"] for s in served], ["tools/fx/track_mix.py"])
        self.assertEqual(served[0]["licence"], "MIT")
        self.assertEqual(len(served[0]["sha256"]), 64)
        self.assertNotIn("track_mix", sys.modules)

    def test_mixer_golden_uses_app_table(self):
        with research.Research(RESEARCH) as r:
            tm = r.load("track_mix")
            ladder = tm.VOLUME_TABLE[5]
        self.assertNotEqual(ladder, applaw.gain_table()[5])  # the reference ladder (-45 dB) is not the app's law


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=1).result
    if not result.wasSuccessful():
        sys.exit(1)
    sys.exit(77 if result.testsRun == len(result.skipped) else 0)
