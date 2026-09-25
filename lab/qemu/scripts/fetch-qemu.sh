#!/bin/sh
# Fetch the exact QEMU source revision without altering an existing checkout.
set -eu

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)
PROTO_DIR=$(CDPATH='' cd "$SCRIPT_DIR/.." && pwd)
REPO_DIR=$(CDPATH='' cd "$PROTO_DIR/../.." && pwd)
WORK_DIR=${QEMU_WORK_DIR:-"$REPO_DIR/.work/build/qemu"}
SOURCE_DIR=${QEMU_SOURCE:-"$WORK_DIR/qemu-v11.0.3"}
QEMU_REMOTE=https://gitlab.com/qemu-project/qemu.git
QEMU_HEAD_REFUSAL_SUFFIX='; refusing to change it'
export QEMU_HEAD_REFUSAL_SUFFIX

# shellcheck source=lib/common.sh
. "$SCRIPT_DIR/lib/common.sh"

require_command git

if [ -e "$SOURCE_DIR" ]; then
    verify_checkout
    printf '%s\n' "QEMU checkout already verified: $SOURCE_DIR"
    exit 0
fi

[ -d "$WORK_DIR" ] || mkdir -p "$WORK_DIR"
printf '%s\n' "Cloning QEMU $QEMU_TAG into $SOURCE_DIR"
git clone --branch "$QEMU_TAG" --depth 1 "$QEMU_REMOTE" "$SOURCE_DIR"
verify_checkout
printf '%s\n' "Fetched and verified QEMU $QEMU_TAG at $QEMU_COMMIT"
