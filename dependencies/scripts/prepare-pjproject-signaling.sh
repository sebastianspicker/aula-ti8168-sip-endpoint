#!/bin/sh
# Apply the one reviewed source-boundary patch to the exact pjproject pin.
set -eu

usage() {
    echo "usage: $0 PJPROJECT_SOURCE_DIR" >&2
    exit 2
}

[ "$#" -eq 1 ] || usage
source_dir=$1
[ -d "$source_dir/pjlib/build" ] || usage

makefile=$source_dir/pjlib/build/Makefile
patch_file=$(CDPATH='' cd -- "$(dirname -- "$0")/../patches" && pwd -P)/pjproject-signaling-only.patch
[ -f "$makefile" ] && [ ! -L "$makefile" ] || {
    echo "prepare-pjproject-signaling: Makefile must be a regular non-symlink file" >&2
    exit 2
}
command -v shasum >/dev/null 2>&1 || {
    echo "prepare-pjproject-signaling: shasum is required" >&2
    exit 2
}
command -v patch >/dev/null 2>&1 || {
    echo "prepare-pjproject-signaling: patch is required" >&2
    exit 2
}

original_sha256=2b23595a6fed6fd3c49c4d691ce96d0713c640780c35386cdfd8b89824aac571
patched_sha256=84028db12b18327e48591426b8fd7ffe70400f3ad829fe2fbab346b5e2f10d3c
actual_sha256=$(shasum -a 256 "$makefile" | awk '{print $1}')
case "$actual_sha256" in
    "$patched_sha256")
        echo "prepare-pjproject-signaling: already prepared"
        exit 0
        ;;
    "$original_sha256") ;;
    *)
        echo "prepare-pjproject-signaling: pjproject Makefile is not the reviewed pin" >&2
        exit 1
        ;;
esac

patch --batch --forward --directory "$source_dir" --strip 1 < "$patch_file"
actual_sha256=$(shasum -a 256 "$makefile" | awk '{print $1}')
[ "$actual_sha256" = "$patched_sha256" ] || {
    echo "prepare-pjproject-signaling: patched Makefile digest mismatch" >&2
    exit 1
}

# The excluded C++ atomic_queue.o is used only by Android MediaCodec and Oboe media
# implementations, both outside this signaling-only build. Keeping it out of
# pjlib prevents an unreviewed C++ runtime dependency in the ARM payload.
echo "prepare-pjproject-signaling: signaling-only source prepared"
