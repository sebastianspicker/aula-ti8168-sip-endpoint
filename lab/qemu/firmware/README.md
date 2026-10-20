# Private original-kernel checks

These opt-in tools boot explicitly selected, hashed local inputs on
`ti8168-mediaboard`. They distribute no firmware or board configuration.
Keep input manifests, generated archives, register snapshots, NAND banks and
run receipts below ignored `.work/`. Original evidence is read-only.

All three CLIs enforce output containment below this checkout's `.work` before
reading private inputs. Symlink aliases, `..`, public source destinations and
existing archive/composition directories are rejected. Missing parent
directories are created with mode `0700`; existing descendants of `.work` must
also be owned private directories. The runner may reuse a private output
container because every run receives a fresh directory. Lower-level builder
functions remain usable with generated temporary fixtures in unit tests.

Current checks have reached the recovered Linux 2.6.37 kernel, its
BusyBox/glibc 2.12.2 runtime, and its UBIFS root. The maintained ARM daemon and
C tests also execute in a generated rescue environment on that kernel.
See [the fidelity matrix](../FIDELITY.md) for remaining hardware gaps.

## Select inputs

`rescue_initramfs.py` accepts a JSON manifest of this form:

```json
{
  "kind": "aula-rescue-inputs-v1",
  "kernel": {"path": "/private/inputs/kernel.uImage", "sha256": "<sha256>"},
  "runtime_root": "/private/inputs/rootfs",
  "busybox": "bin/busybox",
  "files": [
    {"path": "bin/busybox", "kind": "file", "sha256": "<sha256>"},
    {"path": "lib/ld-linux.so.3", "kind": "rclonelink", "sha256": "<sha256>"},
    {"path": "lib/ld-example.so", "kind": "file", "sha256": "<sha256>"}
  ]
}
```

This abbreviated example is not a complete runtime. Select the actual loader
and all required libraries explicitly. `file`, `symlink` and `rclonelink`
describe the input representation. For archived links, the hash covers the
`.rclonelink` bytes; the generated guest receives a symlink. Every link must
resolve to another selected regular file within the guest root. Extraction
does not follow arbitrary source-directory symlinks.

Optional `supplemental` entries contain `source` (absolute host path), `path`
(relative guest path), and `sha256`. Their default `kind`, `executable`,
requires ARM32 little-endian ELF. `kind: "data"` adds an owner-readable file,
such as an owned configuration template. Optional `checks` is a list of argv
arrays whose first element names a selected executable's absolute guest path.
Each command must exit zero before the runner accepts the final marker.

Build an archive without executing it:

```sh
python3 lab/qemu/firmware/rescue_initramfs.py \
  --inputs "$PWD/.work/board/inputs.json" \
  --output-dir "$PWD/.work/board/rescue"
```

The generated init mounts proc/sys/dev, verifies the selected BusyBox hash,
reports `uname`, runs selected checks and waits. Vendor init is bypassed.
The archive and receipt retain hashes of every selected input.

## Boot the rescue archive

Supply the reviewed board settings explicitly; these environment variables
stand for values from your private evidence:

```sh
python3 lab/qemu/firmware/run_original.py \
  --qemu "$PWD/.work/build/qemu/build-aula-v11.0.3/qemu-system-arm" \
  --inputs "$PWD/.work/board/inputs.json" \
  --output-dir "$PWD/.work/board/runs" \
  --memory "$BOARD_MEMORY" \
  --machine-option "boot-machine-id=$BOARD_ID" \
  --machine-option "boot-ram-start=$BOOT_BASE" \
  --machine-option "boot-ram-size=$BOOT_BYTES" \
  --machine-option "early-uart-base=$EARLY_UART" \
  --machine-option nand-profile=synthetic-read-only \
  --nand-id "$NAND_ID" \
  --seed "$PWD/.work/board/control.bin" --seed-sha256 "$CONTROL_SHA256" \
  --append "$KERNEL_ARGUMENTS" --timeout 60
```

Do not put `init=` or `rdinit=` in `KERNEL_ARGUMENTS`; the runner supplies them.
Networking and semihosting are disabled. The runner retains its exact command,
QEMU identity, UART/stderr logs, selected check results and hashes, and stops
the guest after success or the bounded timeout. Missing, failed or out-of-order
markers cannot produce a passing receipt.

## Build and boot a NAND fixture

`compose_nand.py MANIFEST OUTPUT_DIRECTORY` requires a new output directory.
The manifest has `schema_version: 1` and a `segments` array. Each segment has
exactly `path`, `offset`, `size` and `sha256`; offsets and sizes are integer
bytes or hexadecimal strings. Source paths may be relative to the manifest.
Segments must not overlap or exceed the two-bank capacity.

The composer produces two 256 MiB main banks, two 8 MiB OOB banks and a receipt.
Unspecified main bytes and all generated OOB bytes are FF. These are synthetic
backings, **not physical MTD/OOB captures**. No BCH/ECC, bad-block map, partition
layout or second slot is inferred.

For a root-filesystem boot, add an explicit `rootfs` descriptor with absolute
`path` and `sha256` to the input manifest. Invoke the same runner with
`--rootfs-boot --nand-composition .work/board/nand/composition.json` and the
appropriate root/UBI kernel arguments. The runner independently reconstructs
expected bank hashes from the pinned segments, verifies erased gaps/OOB, and
checks the files again after execution. Where UBI attachment needs writes,
`--nand-volatile-writes` enables RAM-only COW; original banks stay unchanged.

This mode boots `/bin/sh`, then verifies the selected BusyBox and a read-only
UBIFS `/` mount using nonce-bound serial responses. Supplemental executables
and checks require rescue mode, which actually loads those files.

## Maintained software and limits

Build owned ARM payloads using the [legacy application lane](../../arm-userspace/LEGACY.md).
It uses authenticated public development inputs; recovered libraries are
private execution references only. Add the resulting ELF files, minimal
runtime dependency closure and explicit check argv to a rescue manifest.

The [owned SIP fixture](../guest/LEGACY_SIP_SMOKE.md) additionally verifies a
real LSZ1-authorized INVITE/ACK/BYE dialog entirely inside the original-kernel
guest. Enable guest loopback, run its protocol self-test, then run the fixture.
The maintained runner keeps external networking disabled.

A pass covers the selected software paths. It does not validate vendor service
startup, DSP/M3 firmware, hardware media, clock/power management, physical
timing, the missing complete flash dump, or redundant-slot recovery.

`make qemu-input-test` runs generated input/receipt tests without QEMU or
firmware; it is included in `make verify`. Runtime model tests remain
`make qemu-verify`, and private boots require the explicit inputs above.
