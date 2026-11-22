"""Golden-test generator library (spec 05-quality/testing-strategy.md "DSP goldens from the research models").

  stimuli   seeded, platform-independent input signals (sweeps, noise, impulses, ...) — never recordings
  f32       float32 rounding and the .f32 file format (raw little-endian float32, channel-major)
  research  locating the research models and importing them behind a guard (no verify/, no firmware-table loaders)
  applaw    the app's own tables plugged into the models (core/util/GainLaw.h)
  models/   one adapter per golden subject: drives a research model (or an own reference) over a case
"""
