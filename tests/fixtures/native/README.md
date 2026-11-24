# Native-format fixtures

Every bundle here was written by this app's own writer. None of them holds device data.

| Path | What it is |
| --- | --- |
| `v<N>/*.sixteen` | Projects saved in schema `N`. A fixture is never rewritten once its schema is no longer the current one: it is what an older app wrote. |
| `expected/v<N>/*.sixteen` | The bundle of `v<N>/` with the same name, opened by the current app (so migrated) and saved in the current schema. |
| `v<N>-defaults.json` | The default value of every member that schema `N` leaves out of a file. |

`tests/unit/store/test_migrations.cpp` opens every `v<N>` bundle and checks that it equals its `expected` bundle. For bundles of the current schema it also checks that saving them again gives the same bytes.

## Regenerating

```sh
# <name>: the project name (python3 tools/identity.py)
APP_REGENERATE_FIXTURES=1 build/host/tests/<name>_tests_store \
  "migration: every fixture version migrates to the current one,store: the current schema's header defaults are pinned by a fixture"
```

This command:

- rewrites `v<current>/` (`default`, `beat`, `random-42`) from their generators;
- rewrites every `expected/` bundle;
- rewrites `v<current>-defaults.json`.

Do it only after you add a migration, then review the diff. When you bump the schema, keep the previous `v<N>/` directory as it is: it becomes an "old version" fixture.

## SP-16 imports made by importer 1 (`v2/sp16-legacy*.sixteen`)

Before importer version 2, the SP-16 importer read the step matrix step-major and kept envelope times as k/127. These two bundles are what it saved from `tests/unit/interop/synthetic/legacy.json`.

How they were made:

1. Generate the `.prj` with `tools/prj-dump/prj-synth.py`.
2. Import it with the importer of commit `721a8dd` (the parent of the fix).
3. Save it with pinned `SaveOptions`: timestamp `2026-11-21T00:00:00Z`, app `fixture-writer`, version `legacy-importer`.
4. Rewrite the sample paths to `/fixture/PIONEER DJ SAMPLER/Samples/Kit/<name>`.

`sp16-legacy-edited` is the same import plus edits made in the app afterwards, in importer 1's coordinates (scene 1, pattern 1):

- a step added at model (step 2, track 8);
- a step added at (5, 1) with velocity 64, where the kick of step 5 belongs;
- velocity 90 set on the scrambled kick at (1, 5);
- the scrambled kick at (1, 9) deleted.

`tests/unit/interop/test_legacy_migration.cpp` checks the migration (`src/services/store/MigrationV3.cpp`) in two ways:

- the migrated `sp16-legacy` must equal a fresh import of `legacy.json`, both in the model and in the records the exporter writes;
- in `sp16-legacy-edited`, the app's edits must survive the migration.

These bundles cannot be regenerated with today's importer. Never edit them.
