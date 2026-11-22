# Contributing

Bug reports, ideas and pull requests are welcome. For bigger changes, or to ask how something works, open an issue
or join the [Discord server](https://discord.gg/DedpwtE6EK) first.

- **Bugs:** [open an issue](https://github.com/freesp16/freesp16/issues/new) with the freesp16 version, whether it
  runs on the instrument or on Linux, and how to reproduce it. On the instrument, attach `bootmenu.log` from the stick: it
  holds the app's output too.
- **Pull requests:** small, focused changes with tests are easiest to review. Every change passes
  `ci/run-local.sh lint host`; changes that affect the hardware build also pass `ci/run-local.sh armel glibc`.
- **Audio thread:** new audio-path code adds a benchmark under `tests/bench/` and stays within its time budget. No
  allocation, locks or I/O on the audio thread: the tests and `tools/rt-lint.py` check this.
- **Modules:** a module only depends on the modules below it (`tools/check-deps.py`). New screens, effects and
  controllers register themselves instead of editing shared files.
- **Commit messages:** an imperative subject of at most 72 characters; the body says why.
- **Name:** the project's name, display name and id are written only in `cmake/ProjectIdentity.cmake`
  (`tools/check-identity.sh`).
- **No firmware:** never commit firmware, keys, update files, disk images, recordings of the instrument or anything
  derived from disassembly. Test data is generated (`tools/check-no-firmware.sh`).

By contributing you agree that your contribution is licensed under GPL-3.0-or-later, like the rest of the code.
