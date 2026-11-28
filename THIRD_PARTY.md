# Third-party components

The repository's own code is GPL-3.0-or-later (`LICENSE`). JUCE is AGPLv3, so a distributed binary follows the AGPLv3
for the JUCE part. Only GPLv3-compatible dependencies may be added; add a row here in the same change.

| Component | Version / pin | Licence | Where | Used for |
|---|---|---|---|---|
| JUCE | 9.0.6, commit `654bdbc7a9db0dae90849b44abe91f339e9f0768` (git submodule) | AGPLv3 or commercial JUCE licence | `third_party/JUCE` | audio devices, MIDI, files, ValueTree, fonts, juce-gl backend, host preview |
| JUCE's bundled code (zlib, libpng, libjpeg, libwebp, FLAC, Ogg Vorbis, HarfBuzz, …) | as in JUCE 9.0.6 | see `third_party/JUCE/modules/*/` licence files (zlib, libpng, IJG, BSD, MIT) | inside JUCE | image/audio codecs, text shaping |
| CLAP C headers | 1.2.9 | MIT (`third_party/clap/LICENSE`) | `third_party/clap/include` | instrument and effect plug-in ABI |
| Catch2 | v3.7.1 (CMake FetchContent, host tests only, not shipped) | BSL-1.0 | build tree | unit tests |
| DejaVu Sans (symbol subset) | 2.37 (SHA-256 `abdc775b21b1bc470d50c97e790d276f2054b7504e56e5bd3e64f48d68582322`; only the subset is in the repository) | Bitstream Vera + public-domain changes (`resources/fonts/DEJAVU-LICENSE.txt`) | `resources/fonts/DejaVuSans-Symbols.subset.ttf` | atlas fallback for the 11 UI symbols Inter lacks (✕ ⟲ ▸ ▾ ⌂ …) |
| Inter | 4.1, `Inter-4.1.zip` from github.com/rsms/inter/releases/tag/v4.1, SHA-256 `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e` | SIL OFL 1.1, no Reserved Font Name (`resources/fonts/INTER-LICENSE.txt`) | `resources/fonts/Inter-{Light,Regular,Medium,SemiBold}.subset.ttf`, bytes in `ui/render/fonts/FontData.cpp` | UI font in the token weights 300/400/500/600; subsets (Latin-1, Latin Extended-A, UI symbols; Light/Medium with tnum+ss01 frozen) made by `resources/fonts/subset.py` |
| JetBrains Mono | 2.304, `JetBrainsMono-2.304.zip` from github.com/JetBrains/JetBrainsMono/releases/tag/v2.304, SHA-256 `6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf` | SIL OFL 1.1, no Reserved Font Name (`resources/fonts/JETBRAINSMONO-LICENSE.txt`) | `resources/fonts/JetBrainsMono-Regular.subset.ttf` | debug readouts (02-tokens.md font.family.mono) |
| fontTools | 4.55.0 (pip, developer tool only, not shipped) | MIT | — | `resources/fonts/subset.py` (font subsetting, run by hand) |
| Khronos EGL/GLES headers | Debian bookworm `libegl-dev`/`libgles-dev` (toolchain image only) | MIT / Apache-2.0 (Khronos) | toolchain sysroot | compiling JUCE's GLES backend and gldispatch; no Khronos code is linked |
| X11 extension headers (XInput2, Xcomposite) | Debian bookworm (toolchain image only) | MIT/X11 | toolchain sysroot | compiling JUCE's X11 code; libraries are loaded at run time |
| Linux desktop system libraries (libasound, libX11; Mesa / libglvnd `libEGL.so.1` + `libGLESv2.so.2`; Xvfb for the smoke test) | the desktop's packages (Ubuntu: `libasound2-dev`, `libx11-dev`, `libegl-dev`, `libgles-dev`, `xvfb`) | LGPL-2.1 (alsa-lib) / MIT (X11, Mesa, libglvnd) | the developer's system | the `linux` platform: libasound and libX11 are linked; EGL/GLES are loaded at run time through gldispatch (no GL library is linked); not redistributed |
| Debian wheezy armel sysroot (eglibc 2.13, libasound, libX11, fontconfig, …) | archive.debian.org, built in `toolchain/Dockerfile` | LGPL / MIT / various (Debian) | toolchain image | cross-linking against the device's libraries; not redistributed |
| GCC 11.4 + binutils 2.40 | built in `toolchain/Dockerfile` | GPLv3 + GCC Runtime Library Exception | toolchain image | cross compiler; libstdc++/libgcc statically linked under the runtime exception |
| QEMU user mode (`qemu-arm`) | Debian bookworm `qemu-user` (toolchain image only) | GPLv2 | toolchain image | runs the armel NEON kernel tests (`scripts/neon-tests.sh`); a build tool, never linked or shipped |
| FreeType | 2.13.3, built statically into the sysroot | FreeType License (BSD-style) or GPLv2 | toolchain image → linked | font rasterisation (JUCE) |
| fdlibm `logf` / `log10f` / `powf` (float versions, as in glibc ≤ 2.27) | the public algorithm (Sun Microsystems 1993, float conversion by Ian Lance Taylor, Cygnus Support), ported in C++ from the research's MIT transliteration `tools/fx/fdlibm_powf.py` at `8901c97` | fdlibm notice (permissive: use, copy, modify, distribute with the notice kept), kept in the file | `src/engine/fx/dsp/filter_fdlibm.h` → linked | exact glide step counts of the Filter and 2-band EQ, and the EQ's parity-maths frequency law (FX-054) |
| Design tokens and mockups | Sixteen spec, `07-ui-design/tokens.json`, `mockups/*.svg` (pinned copies listed with their SHA-256 in `resources/design/SPEC_VERSION`, checked by `tools/check-spec-sync.sh`) | GPL-3.0-or-later, like this repository (the spec is the project's own design document) | `resources/design/` | theme generation, snapshot test |

Planned: Ableton Link (GPLv2+).

Time-stretch: no third-party library. The MASTER TEMPO backends are the project's own code
(`src/engine/voice/stretch/`: `pvoc` phase vocoder with its own FFT, `wsola` time-domain, `none` resample only).
Signalsmith Stretch (MIT) and Rubber Band (GPLv2+, R2 engine) stay candidates should the device bench and a
listening test reject the own backends; either would then be vendored at a pinned release and listed above.

Research inputs used only to generate test data, never shipped: the research repository's `docs/` (CC BY 4.0) and
`tools/fx/` Python models (MIT; `glibc_expf.py` LGPL-2.1-or-later; fdlibm notices). No firmware.
`tools/golden-gen/` drives them from a separate checkout at the spec's pinned commit `8901c97` (not
vendored, not linked; Python standard library only, no pip packages). Each generated case under `tests/golden/` lists
the model files it ran with their licence and SHA-256; so far only `tools/fx/track_mix.py` (MIT). Committed goldens are
generated signals run through those models; nothing firmware-derived is loaded (the table loaders are stubbed out).
