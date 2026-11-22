"""Golden model adapters. Each module defines:

  SUBJECT                    the C++ subject id the harness runs against it (tests/golden/harness/Subject.h)
  input_channels(case)       names of the input channels, in file order
  output_channels(case)      names of the output channels, in file order
  run(case, inputs, research, block) -> (outputs, tables)
                             inputs/outputs: lists of channels (lists of float32 values); tables: the app tables
                             plugged into the model, {name: {"source": ..., "f32_bits": [...]}}
  RESEARCH_MODELS            the research tools/fx modules it drives (empty for an own reference)
  OWNER_INPUTS               firmware-derived inputs the model needs ([] = CI-generatable; see cases.json local_only)
"""
from . import fx_eq2, fx_filter, fx_shell, gain_test, track_strip
from . import compressor, ducker
from . import distortion, distortion_bitexact, lofi, oscillator
from . import fx_delay, fx_delaybus, fx_reverb, fx_reverb_parity

ADAPTERS = {m.NAME: m for m in (track_strip, gain_test, fx_shell, fx_filter, fx_eq2)}
ADAPTERS.update({m.NAME: m for m in (compressor, ducker)})
ADAPTERS.update({m.NAME: m for m in (lofi, distortion, distortion_bitexact, oscillator)})
from . import chorus, flanger, phaser
ADAPTERS.update({m.NAME: m for m in (chorus, flanger, phaser)})
ADAPTERS.update({m.NAME: m for m in (fx_delay, fx_delaybus, fx_reverb, fx_reverb_parity)})
from . import routing_device, routing_headphones, routing_router
ADAPTERS.update({m.NAME: m for m in (routing_router, routing_headphones, routing_device)})
