// The Ducker's SYNC tables (spec 02-features/effects/ducker.md FX-1210): Sixteen's own, by
// formula — never the reference's firmware tables — in sixteenth notes, rounded to 0.001 sixteenth:
//   T_A[k] = 4·(k/127)²            k = 0…127: 0 … 4 sixteenths, finest near zero
//   T_R[k] = 0.25·128^(k/127)      0.25 … 32 sixteenths, logarithmic
// The same formula as core::fx::duckerSixteenths (the SYNC conversion and the value text, core/params/catalogue/fx.cpp);
// tests/unit/fx/test_fx_ducker.cpp checks the two agree for every k. Built once before main() (namespace-scope object
// in Ducker.cpp), read-only on the audio thread.
#pragma once

#include <cmath>

namespace engine::fx::dsp::ducker
{
struct SyncTables
{
    static constexpr int kSize = 128;
    float attack[kSize]{}, release[kSize]{};

    SyncTables() noexcept
    {
        for (int k = 0; k < kSize; ++k)
        {
            const double x = k / 127.0;
            attack[k] = static_cast<float>(std::round(4.0 * x * x * 1000.0) / 1000.0);
            release[k] = static_cast<float>(std::round(0.25 * std::pow(128.0, x) * 1000.0) / 1000.0);
        }
    }
};
} // namespace engine::fx::dsp::ducker
