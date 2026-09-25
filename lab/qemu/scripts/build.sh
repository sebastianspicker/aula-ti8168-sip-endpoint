#!/bin/sh
# Configure and build a native-host arm-softmmu QEMU without modifying firmware evidence.
set -eu

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)
PROTO_DIR=$(CDPATH='' cd "$SCRIPT_DIR/.." && pwd)
REPO_DIR=$(CDPATH='' cd "$PROTO_DIR/../.." && pwd)
WORK_DIR=${QEMU_WORK_DIR:-"$REPO_DIR/.work/build/qemu"}
SOURCE_DIR=${QEMU_SOURCE:-"$WORK_DIR/qemu-ls200-v11.0.3"}
BUILD_DIR=${QEMU_BUILD_DIR:-"$WORK_DIR/build-ls200-v11.0.3"}

# shellcheck source=lib/common.sh
. "$SCRIPT_DIR/lib/common.sh"
# shellcheck source=lib/patch-stack.sh
. "$SCRIPT_DIR/lib/patch-stack.sh"

host_jobs() {
    if command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null || printf '%s\n' 1
    elif command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN 2>/dev/null || printf '%s\n' 1
    else
        printf '%s\n' 1
    fi
}

ensure_ninja() {
    if command -v ninja >/dev/null 2>&1; then
        NINJA_BIN=$(command -v ninja)
    else
        die "Ninja must be prepared before building QEMU; build dependencies are never downloaded"
    fi

    "$NINJA_BIN" --version >/dev/null || die "Ninja cannot run: $NINJA_BIN"
}

require_command git
require_command make
verify_checkout
require_patch_stack
FINAL_PATCH=
# shellcheck disable=SC2086 # PATCH_STACK is the intentional argument list.
for patch_file in $PATCH_STACK; do
    FINAL_PATCH=$patch_file
done
[ -n "$FINAL_PATCH" ] || die "QEMU patch stack is empty"
git -C "$SOURCE_DIR" apply --reverse --check "$FINAL_PATCH" || \
    die "complete modular LS-200 patch stack is not applied to $SOURCE_DIR"
verify_prepared_source
ensure_ninja

if [ -f "$BUILD_DIR/config-host.mak" ]; then
    configured_source=$(sed -n 's/^SRC_PATH=//p' "$BUILD_DIR/config-host.mak")
    expected_source=$(CDPATH='' cd "$SOURCE_DIR" && pwd -P)
    [ "$configured_source" = "$expected_source" ] || \
        die "build directory belongs to $configured_source, expected $expected_source"
fi

if [ ! -f "$BUILD_DIR/config-host.mak" ] || [ "${QEMU_RECONFIGURE:-0}" = 1 ]; then
    [ -d "$BUILD_DIR" ] || mkdir -p "$BUILD_DIR"
    printf '%s\n' "Configuring QEMU in $BUILD_DIR"
    (
        cd "$BUILD_DIR"
        "$SOURCE_DIR/configure" \
            "--ninja=$NINJA_BIN" \
            --disable-download \
            --target-list=arm-softmmu \
            --disable-docs \
            --disable-tools \
            --disable-guest-agent \
            --enable-slirp \
            --disable-capstone \
            --disable-werror
    )
else
    printf '%s\n' "Using existing QEMU configuration: $BUILD_DIR"
fi

jobs=${QEMU_JOBS:-$(host_jobs)}
case $jobs in
    ''|*[!0-9]*) die "QEMU_JOBS must be a positive integer" ;;
    0) die "QEMU_JOBS must be a positive integer" ;;
esac
make -C "$BUILD_DIR" -j "$jobs" qemu-system-arm
printf '%s\n' "Built: $BUILD_DIR/qemu-system-arm"
