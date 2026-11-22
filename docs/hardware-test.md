# The first full test on the reference hardware

A morning plan for one session with the reference hardware, using your own USB-stick launcher and its SSH access
(the launcher stops the stock app, runs this app as root from RAM, mounts the stick's `<name>-data/` over
`/root/data`, exports the stick-root `<name>.conf` as `<PREFIX>_<KEY>` variables and writes everything to
`<name>.log` on the stick). The steps are ordered by risk and value: each one only needs the ones before it. Every step
names the settings variant to use, what to look and listen for, the expected log lines, and what to send back.

`<name>` is the project's machine name and `<PREFIX>` its environment prefix (`python3 tools/identity.py --shell`);
the generated bundle and its helpers carry the real values. Timings are A9 cycles at 1 GHz (= ns); the budgets are
the project's performance budgets for the Cortex-A9.

## 0. Prepare (on the PC, before the session)

```sh
scripts/build-armel.sh                 # out/armel/<name>       (the app; glibc ≤ 2.13 checked)
scripts/build-armel-bench.sh           # out/armel/<name>_bench (the device benches)
scripts/make-hw-test-bundle.sh         # out/hw-test/           (kit, settings variants, scripts, helpers)
```

1. Copy the **content** of `out/hw-test/stick/` onto the stick's root, beside the launcher's image. It adds
   `<name>-data/<name>/Samples/TestKit/` (16 generated one-shots), `<name>-data/<name>/hw-test/` (settings variants,
   input scripts, `use.sh`, `report.sh`) and `<name>.conf.example`.
2. The stick-root `<name>.conf` (if you have one) keeps **network keys only** (`ssh=`, `ssh_ip=` …). App keys there
   become `<PREFIX>_<KEY>` variables, which win over the variants (the app reads every such variable now, not only a
   fixed list). The variants are copied to `<name>-data/<name>/<name>.conf` — the app's own settings file on the unit,
   re-read at every start, so switching a variant needs no re-insertion of the stick.
3. Plug the stick and the LAN cable, `host/sp16-link.sh up`, check `ssh root@169.254.16.16 ctl status`.
4. Deploy the new binary: `out/hw-test/pc/hwt.sh deploy` (= `ctl update < out/armel/<name>`; the launcher's own
   deploy script does the same).

`out/hw-test/pc/hwt.sh` wraps the SSH steps (it uses `SP16_IP`, `SP16_PORT`, `SP16_KEY` like the launcher's scripts):

| command | does |
|---|---|
| `pc/hwt.sh use <variant>` | copies `hw-test/conf/<variant>.conf` to the app's settings file and restarts the app (`use` alone lists them) |
| `pc/hwt.sh report <label>` | the last run's lines → `out/hw-test/reports/<label>-<time>.txt` (**send these back**) |
| `pc/hwt.sh log <label>` | the raw log (for `tools/*-check.py verify`) |
| `pc/hwt.sh follow` | the live log |
| `pc/hwt.sh bench [filter]` | stops the app, runs the device benches, prints the table, starts the app again |

On the unit itself the same is `sh <stick>/<name>-data/<name>/hw-test/use.sh <variant>` and `… /report.sh`.

What `report` sends back: the launcher's lines of that run (panel/strip holders, launch, exit code), `START`,
`PLATFORM`, `SETTINGS` (everything that differs from the defaults, and where it came from), the whole `DIAG` block,
`PROBE`/`RUN`, `AUDIO`, `LOAD`, `FRAME`, `MARK` (script phases), `PARAM`, `KIT`, `SAMPLING`, `RENDER`, `IMPORT`,
`SAVED`, `FATAL`, `CRASH` (with its backtrace), `DIAG STOP`, `STOP`, and one `INPUT summary` line per source (`report
<label> all` keeps every INPUT line).

## 1. Start-up and DIAG — variant `01-baseline`

`pc/hwt.sh use 01-baseline`, wait 20 s, `pc/hwt.sh report baseline`. Then once `00-defaults` (nothing set:
`render=auto`), `pc/hwt.sh report defaults`.

Expected (the block ends with `DIAG END`, before any project or kit is loaded):

| line | expected |
|---|---|
| `SETTINGS file=/root/data/<name>/<name>.conf (read) …` | `diag_interval=10 input_trace=1 render=gl-fb,soft` (plus the stick-root network keys, `ssh=…`, if that file has them) |
| `DIAG platform imx6 reason: imx6 selected (auto: detected …) \| i.MX6 board (/dev/subucom_spi0.1 present) …` | imx6 detected by the panel node |
| `DIAG kernel Linux … armv7l` | the stock kernel |
| `DIAG cpu online=4 … neon=yes freq_khz=… governor=…` | 4 cores, NEON; note the frequency and governor (benches assume 1 GHz) |
| `DIAG libc glibc 2.13 (… built against 2.13)` | 2.13 |
| `DIAG memory memtotal=… memfree=…` | note the numbers |
| `DIAG data dir=/root/data/<name> writable … mount=/root/data vfat` | the stick (bind mount); free space noted |
| `DIAG storage fat_mounts=…` | `/mnt/emmc` and the stick's mount |
| `DIAG storage internal=/mnt/emmc samples=… projects=N … (read-only library location $Ext:INTERNAL)` | the internal disk present |
| `DIAG sound card …` / `DIAG sound pcm …` | the two codecs (master and headphone PCM) |
| `DIAG midi rawmidi=…` | the MIDI ports by name (none opened) |
| `DIAG display backend=gl-fb size=800x480 … GL_RENDERER=…` | gl-fb working, the Vivante strings (`backend=soft` + `DIAG display probes failed:` = gl-fb failed, the variants fall back to soft) |
| `DIAG audio device=alsa-direct ok rate=44100 period=256 … ports MAIN=12/14 OUT3-4=8/10 OUT5-6=4/6 OUT7-8=0/2 PHONES=0/1 FILTERSEND=absent` | the codec and the port map; PHONES `absent` = the headphone PCM did not open (the `DIAG audio info` line says why) |
| `DIAG input panel ok …`, `DIAG input cdc-strip ok …` | both sources started (the EBUSY fix: no `FAILED … busy`) |
| `DIAG interval=10 s`, `DIAG END` | |

Every 10 s: `FRAME run …`, `AUDIO … xruns=0`, `LOAD … xruns=0 VmRSS=…`. On exit (`ctl stop`): `DIAG STOP reason=signal
… xruns=… load_p95_max=… peak_rss_kB=…` and `STOP`.

Listen: with no project yet, track 1's 440 Hz test tone at −12 dBFS on MAIN (OUT1-2) — only because `01-baseline`
sets `test_tone=1`; the app's default (`00-defaults`, and every normal start) is silent. Later runs resume the last
saved project (a self-test run saves one), so run this step first.

## 2. Display

Still `01-baseline`. Look: HOME drawn at once, no tearing, smooth meters. Expected: `FRAME run backend=gl-fb … total_ms
p95` well under 16.7 and no `PACE` line (a `PACE` line = the swap does not wait for vsync). Then `02-soft` (`pc/hwt.sh
use 02-soft`, `report soft`): the CPU rasteriser's `FRAME` numbers for comparison. If gl-fb fails, the `DIAG display
probes failed:` line says why and the variants (`render=gl-fb,soft`) go on with soft; `render=auto` (`00-defaults`)
tries gl-fb, gl-x11, juce-gl, soft. An explicit single backend that fails ends in `FATAL no renderer`.

## 3. Panel, pads, encoders, strip, lamps (HUMAN) — `04-trace`, then `09-lamps`

`pc/hwt.sh use 04-trace` (every panel and strip event and every lamp change is logged). Then by hand, in this order:
the 16 pads (soft and hard), the 16 step keys and BAR keys, the pad-mode keys, SCENE / PATTERN, PLAY / STOP / REC,
SHIFT, BACK, HOME, the six encoders (both directions, pushed), the selector (turn + push), the four analog-filter knobs
and the main volume, the strip (touch, slide, lift; STRIP MODE, HOLD), the LCD (tap a tab). `pc/hwt.sh report trace
all`. Expected: one `INPUT panel <control> <kind> <value>` / `INPUT cdc-strip …` line per action, pad velocities that
follow how hard you hit, encoder detents ±1 per click, no stuck lamps, `DIAG input panel ok` (the EBUSY fix:
launcher's `runner: holders (… before launch): none`).

`pc/hwt.sh use 09-lamps` (a script cycles the pad modes, SCENE / PATTERN / REC / HOLD / ACTIVE, sweeps the strip LEDs
and the pad lamps; 40 s). **HUMAN:** watch each `MARK` phase light the right lamp; note any lamp that stays dark or
lights the wrong colour. Send `report lamps all` (the `LAMP` lines show what the app asked for).

## 4. Audio: MAIN, OUT3-4 / 5-6 / 7-8, PHONES — `06-routing`

`pc/hwt.sh use 06-routing` (the kit and a two-track pattern play for 2 minutes; every 20 s the routing changes).
**HUMAN:** at each phase listen on the jack it names (move the monitor cable / the headphones):

| t | phase (`MARK` line) | expected |
|---|---|---|
| 6 s | MAIN: both tracks | MAIN: both; PHONES: both (cue MASTER) |
| 26 s | 8CH, track 1 → OUT3-4 | OUT3-4: track 1 (the higher tone on steps 1/5/9/13) only; MAIN: track 2 only |
| 46 s | track 1 → OUT5-6 | OUT5-6: track 1 only |
| 66 s | track 1 → OUT7-8 | OUT7-8: track 1 only |
| 86 s | track 1 → OUT3-4, HP CUE OUT3-4 | PHONES: track 1 only |
| 106 s | back to M + 6CH | MAIN: both |

Send `report routing`; say which jack carried what. A pair on the wrong jack is a data fix, not a rebuild: the setting
`audio_map=out34:8/10,out56:4/6,out78:0/2` (put the line into the variant file or the app's settings file). Expected
lines: `PARAM routing.… posted` ×10, `AUDIO … xruns=0`. `03-audio-off` isolates display and input problems from the
codec (`DIAG audio off`).

## 5. Kit, sequencer, scenes — `05-selftest` (unattended, ~65 s)

`pc/hwt.sh use 05-selftest`, wait until the app quits (~70 s; the launcher then brings the stock app back — `ctl start`
returns to this app), `pc/hwt.sh log selftest`, `pc/hwt.sh report selftest`. Check it on the PC:

```sh
python3 tools/hw-test-bundle.py verify --kind selftest --log out/hw-test/reports/log-selftest-<time>.txt
```

Phases (each a `MARK` line): pads 1–16 400 ms apart (a C major scale, four noise bursts, chords and a chirp —
listen for 16 distinct clean hits, no clicks), the pattern (track 1 steps 1/5/9/13, track 2 steps 3/7/11/15, 120 BPM),
a queued switch to scene 2 and back (`M2 SWITCH scene=2`), the routing round (track 1 to OUT3-4 / 5-6 / 7-8), the
analog filter model (sweep), a 2-bar RESAMPLE take onto track 16 (`SAMPLING TAKE ok`), a render (`RENDER JOB done …
frames=176400`) and `SAVED hwselftest ok`. Expected also: `KIT ready installed=16`, `xruns=0` everywhere, `LOAD p95`
noted (the A9 figure for this load).

## 6. Effects and the analog filter

In the self-test's `analog filter` phase (36–39 s) the low-pass closes with resonance: listen for a clearly filtered,
resonant pattern, then the open sound again; note `LOAD p95` in the 5-s lines of that window against the ones before.
The model's own cost is `afs_model` in step 10 (budget 45 k cycles; may exceed it on the A9).

## 7. MIDI DIN / USB — `01-baseline` with MIDI devices connected

The MIDI source is a control source like the panel: `DIAG input midi ok …` names the ports it opened (alsa-rawmidi:
the DIN port and, in USB MIDI gadget mode, the USB port; USB MIDI and USB storage are exclusive gadget modes), and
`DIAG midi rawmidi=midiC…D…` lists the device nodes. Every 10 s a `MIDI stats in=… sent=… clocks=… late=… dropped=…
jitter_p99=… tempo=…` line. **HUMAN:** connect a keyboard to MIDI IN (notes → `in=` grows, pads sound on MIDI tracks),
a receiver to MIDI OUT (the clock: `clocks=` grows while playing, `late=0`), and check THRU. Send `report midi`
with the port names, and say what played. `midi=0` in a variant turns MIDI off (to isolate it); the MIDI benches
(`bench_midi*`, step 10) carry the A9 numbers.

## 8. LINE IN sampling — `07-linein` (HUMAN: a signal into LINE IN)

Connect a source to LINE IN and keep it playing from 3 s on. `pc/hwt.sh use 07-linein`. The script records 2 bars
stereo onto track 15 and 3 s of LINE IN L onto track 14, then plays both. Expected: two `SAMPLING TAKE ok … ` lines
(frames, peak levels), the takes audible on pads 15 / 14, `AUDIO ok … input` if the capture side opened. Send `report
linein`; say whether the takes sound right (level, no dropouts, no offset).

## 9. Import of the unit's own project — `08-import`

The internal disk is a **read-only** library location: the app never writes there (no copy, move, rename, trash or
new folder into it; the stock tree's samples are browsable under INTERNAL). Before: on the unit `touch /tmp/before`.
`pc/hwt.sh use 08-import`: the script lists the internal disk's projects (`IMPORT unit project <n> <file> <bytes>`),
imports the first one (A→Z) with its samples in place, plays it for 10 s, renders a pass and saves it as `hwimport` in
this app's own project folder (on the stick). Expected: `IMPORT done ok project=… samples=N loaded=N missing=0`, the
`IMPORT` report lines, `RENDER JOB done`, `SAVED hwimport ok`. After: `find /mnt/emmc -newer /tmp/before` must print
nothing. Send `report import` and that `find` output. A different project: `cmd import-sp16 internal:<n>` (n-th, A→Z)
or `internal:<file.prj>` in a script line.

## 10. Device benches (performance budgets)

`pc/hwt.sh bench` (stops the app; ~5–10 min; one line per bench) — or by hand:

```sh
ssh root@169.254.16.16 ctl stop
ssh root@169.254.16.16 'cat > /tmp/<name>_bench && chmod 0755 /tmp/<name>_bench' < out/armel/<name>_bench
ssh root@169.254.16.16 /tmp/<name>_bench > bench-unit.txt 2>&1      # args: a Catch2 filter, e.g. '[mixer]'
ssh root@169.254.16.16 'rm /tmp/<name>_bench; ctl start'
python3 tools/bench-report.py bench-unit.txt             # --markdown for a table, --only-over, --csv FILE
```

Expected: `BENCH DIAG cpu … freq_khz=… governor=…` (the conditions), one `BENCH <name> budget=… measured=… OK|OVER`
line per bench, `BENCH DONE rc=… cases=… failed=…`. Send the raw output file. The table names what each number
answers: mixer (45 k), trigger (6 k), analog filter (45 k), stretch, library listing
(300 ms for 10,000 entries, measured on `/tmp`; `BENCH_TMPDIR=/root/data/x` puts the store benches on the stick),
render ≥ 1.8× real time (`bench_render_offline`, 806 k per block). Catch2 cases that FAIL only because a desktop gate is
exceeded (the UI benches, note `gate x10`) are expected; `tools/bench-report.py` compares those with the A9 figure.

## 11. Soak: 30-minute scripted live set — `10-liveset`

`pc/hwt.sh use 10-liveset` and leave it (the same generator as `scripts/smoke/95-liveset.sh`: every panel feature,
screens, scenes, recording, saves; seed 1604; quits after ~30 min). Glance at it now and then (screens change, lamps
follow). Afterwards `pc/hwt.sh log liveset` and on the PC:

```sh
python3 tools/liveset-check.py verify --log out/hw-test/reports/log-liveset-<time>.txt --manifest out/hw-test/liveset.json \
    --script out/hw-test/stick/<name>-data/<name>/hw-test/scripts/liveset.txt
```

Expected: exit code 0 of the app, no FATAL / CRASH, xruns 0 in every `AUDIO` / `LOAD` line, `LOAD p95` ≤ 725 000
cycles (50 % of the block), every scripted screen's line, every save ok, `VmRSS` flat (no growth), `DIAG STOP`. (No
WAV on the unit: the MAIN-level checks of the desktop run are skipped.) Send `report liveset` and the verify output.

## 12. Optional: the crash report — `12-crash`

Last, once: `pc/hwt.sh use 12-crash` crashes the app on purpose after 5 s. Expected in `report crash`: `CRASH SIGSEGV
(11) … pc=… lr=…`, `CRASH backtrace frames=…` with address lines, `CRASH map …` lines, `CRASH end`, and the launcher's
`runner: app exited rc=139`. The addresses resolve on the PC against the unstripped binary (`build/armel/bin/<name>`,
inside the toolchain container: `arm-linux-gnueabi-addr2line -C -f -e build/armel/bin/<name> <addr>`). Then `pc/hwt.sh
use 01-baseline` (or `ctl start`).

## Troubleshooting

| symptom (log lines) | likely cause | what to do |
|---|---|---|
| `PANEL FAILED panel: … Device or resource busy` or `CDC-STRIP FAILED …` | another process holds the device (stock app not stopped) | the launcher's `runner: holders` lines name it; `ctl restart`; send those lines |
| `AUDIO alsa-direct unavailable: alsa: open hw:ak4458audio,0: …`, `AUDIO FAILED no audio device started` | the codec PCM is busy or named differently | `DIAG sound card` / `sound pcm` lines give the names; `audio_device=hw:<card>,0` in the variant; `03-audio-off` to go on with the rest |
| `DIAG audio info … PHONES = unavailable: …` | the headphone PCM did not open | `phones_device=hw:<card>,0` (or `none`); send the line |
| `PROBE gl-fb unavailable: …`, `DIAG display probes failed: …` | the GL framebuffer path fails | `02-soft` (`render=soft`) for everything else; send the PROBE lines; `compare=1` measures every backend |
| `PACE gl-fb: swaps do not wait for vsync` | the driver ignores the swap interval | harmless (CPU pacing takes over); note it |
| `xruns=` > 0 in AUDIO / LOAD, `LOAD … overruns=` > 0 | period too short for the load, or a stall | `11-period512` (`audio_period=512`); send `LOAD` lines; `DIAG cpu … governor=` (ondemand = slower) |
| `KIT FAILED …` | the kit folder is missing | `Samples/TestKit` must be under `<name>-data/<name>/` on the stick |
| `IMPORT FAILED no unit project …` | no `.prj` in the internal disk's projects folder | `DIAG storage internal=` line; `internal_disk=<mount>` if mounted elsewhere |
| no new lines, `runner: app exited rc=…` | the app died before its log | `rc` ≥ 128 = signal (139 SEGV, 134 abort): the `CRASH` block precedes it; send `report <label>` |
| a setting has no effect | the stick-root conf overrides it, or a typo | the `SETTINGS` line shows every effective non-default key and its source counts |
