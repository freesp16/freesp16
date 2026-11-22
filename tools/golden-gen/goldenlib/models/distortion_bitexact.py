"""Local-only, bit-exact Distortion goldens (parity + tables 1e-6). The same cases as models/distortion.py, but
distortion.py's expf must run with the glibc 2.13 `__exp_atable` / `__exp_deltatable` tables read from the user's own
device libm (research tools/fx/README.md "Inputs you must supply"; LGPL-2.1-or-later glibc_expf.py). Those are
firmware-derived inputs: golden-gen never loads them (research.py stubs glibc_expf.load_tables), so gen.py reports this
adapter as needing user-supplied inputs and skips it, the recipe is local_only (never committed outputs), and its ctests
skip (77) wherever build/golden-local has no files — always in CI. Producing them is a deliberate local opt-in.
"""
from . import distortion

NAME = "distortion_bitexact"
SUBJECT = "fx.effect"
RESEARCH_MODELS = distortion.RESEARCH_MODELS
OWNER_INPUTS = ["glibc 2.13 expf tables __exp_atable / __exp_deltatable from your own libm.so.6"]
input_channels = distortion.input_channels
output_channels = distortion.output_channels
validate = distortion.validate
run = distortion.run
