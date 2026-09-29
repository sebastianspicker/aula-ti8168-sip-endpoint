#!/bin/sh
# Execute a candidate only inside a read-only, networkless container.
set -eu

usage() { echo "usage: $0 --sysroot DIR --binary FILE [-- ARGS...]" >&2; exit 2; }
sysroot='' binary=''
while [ "$#" -gt 0 ]; do
    case "$1" in
        --sysroot) sysroot=${2:?missing sysroot}; shift 2 ;;
        --binary) binary=${2:?missing binary}; shift 2 ;;
        --) shift; break ;;
        *) usage ;;
    esac
done
[ -d "$sysroot" ] && [ ! -L "$sysroot" ] && [ -f "$binary" ] && [ ! -L "$binary" ] || {
    echo "run-qemu-arm: sysroot and binary must be regular non-symlink inputs" >&2; exit 2;
}
project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
repository_root=$(CDPATH='' cd -- "$project_dir/../.." && pwd -P)
path_guard=$repository_root/tooling/workspace/protected_output.py
canonical_sysroot=$(python3 "$path_guard" --input "$repository_root" "$sysroot") || exit 2
canonical_binary=$(python3 "$path_guard" --input "$repository_root" "$binary") || exit 2
[ "${AULA_QEMU_ARM_ENABLE:-}" = 1 ] || { echo "run-qemu-arm: set AULA_QEMU_ARM_ENABLE=1 after authorization" >&2; exit 2; }
engine=${AULA_QEMU_ARM_ENGINE:-docker}
case "$engine" in docker|podman) ;; *) echo "run-qemu-arm: engine must be docker or podman" >&2; exit 2;; esac
command -v "$engine" >/dev/null 2>&1 || { echo "run-qemu-arm: required container engine is unavailable" >&2; exit 2; }
image=${AULA_QEMU_ARM_IMAGE:-}
printf '%s\n' "$image" | grep -Eq '.+@sha256:[0-9a-f]{64}$' || {
    echo "run-qemu-arm: set AULA_QEMU_ARM_IMAGE to a reviewed image digest" >&2; exit 2;
}
# The image must provide qemu-arm.  This script intentionally does not fall
# back to a host qemu binary, host network namespace, or writable sysroot.
exec "$engine" run --pull never --rm --network none --read-only --tmpfs /tmp:rw,noexec,nosuid,size=16m \
    --cap-drop ALL --security-opt no-new-privileges --user 65534:65534 \
    --env PATH=/usr/bin:/bin \
    --volume "$canonical_sysroot:/sysroot:ro" \
    --volume "$canonical_binary:/candidate/aula-sipd:ro" \
    "$image" qemu-arm -L /sysroot /candidate/aula-sipd "$@"
