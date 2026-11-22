"""The app's own tables, plugged into the research models in place of the reference ones.

gain_table(): core::GainLaw (core/util/GainLaw.h; spec 01-architecture/audio-engine.md "Gain law"). The research
`track_mix.VOLUME_TABLE` uses the reference's coarse ladder for indices 1-9; the spec interpolates linearly in dB,
so mixer goldens use this table. The formula mirrors GainLaw::gainExact() operation for operation and is rounded
to float32 like GainLaw::gain(); every case .json stores the table's bit patterns and the C++ harness checks them
against GainLaw::gain() before it runs (a changed GainLaw.h fails with "regenerate the goldens").
"""
import math

from .f32 import F

MAX_INDEX = 127


def gain_exact(index):
    if index <= 0:
        return 0.0
    if index > MAX_INDEX:
        index = MAX_INDEX
    if index < 10:
        db = -54.0 + (index - 1) * (13.0 / 8.0)
        return 10.0 ** (db / 20.0)
    x = index / 100.0
    if index <= 100:
        return x * x
    return 2.0 ** (math.log(x) / math.log(1.27))


def gain_table():
    return [F(gain_exact(i)) for i in range(MAX_INDEX + 1)]
