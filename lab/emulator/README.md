# Synthetic local emulator

This Python package models recording and stream activity for local development
checks. It has no device, network, firmware, web, or serial interface. The
vendor-compatible logical emulator and its protocol investigations are frozen
in the private source snapshot; they are not a product runtime dependency.

The retained runtime exposes `recording` and `stream_active` booleans, a
monotonic revision, and bounded in-process events. State changes are saved
atomically when a JSON state file is supplied. Its schema is
`synthetic-local-state-v1` inside a version-2 persistence envelope; previous
emulator snapshots cannot be loaded into this runtime.

From the repository root:

```sh
make -C lab/emulator verify-core
PYTHONPATH=lab/emulator/src python3 -m aula_emulator status --state-file .work/cache/emulator-run/synthetic-state.json
PYTHONPATH=lab/emulator/src python3 -m aula_emulator record-start --state-file .work/cache/emulator-run/synthetic-state.json
PYTHONPATH=lab/emulator/src python3 -m aula_emulator stream-start --state-file .work/cache/emulator-run/synthetic-state.json
```

The CLI accepts state files only below this checkout's `.work/` directory.
The commands change synthetic state only; they produce no recording or stream
media.
