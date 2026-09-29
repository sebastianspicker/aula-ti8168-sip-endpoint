#!/bin/sh
# Create an isolated patched TI8168 model worktree from explicit local source.
set -eu

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)
PROTO_DIR=$(CDPATH='' cd "$SCRIPT_DIR/.." && pwd)
REPO_DIR=$(CDPATH='' cd "$PROTO_DIR/../.." && pwd)
WORK_DIR=${QEMU_WORK_DIR:-"$REPO_DIR/.work/build/qemu"}
BASE_SOURCE=${QEMU_BASE_SOURCE:-"$WORK_DIR/qemu-v11.0.3"}
SOURCE_DIR=${QEMU_SOURCE:-"$WORK_DIR/qemu-aula-v11.0.3"}

# shellcheck source=lib/common.sh
. "$SCRIPT_DIR/lib/common.sh"
# shellcheck source=lib/patch-stack.sh
. "$SCRIPT_DIR/lib/patch-stack.sh"

require_patch_stack
FINAL_PATCH=
# shellcheck disable=SC2086 # PATCH_STACK is the intentional argument list.
for patch_file in $PATCH_STACK; do
    FINAL_PATCH=$patch_file
done
[ -n "$FINAL_PATCH" ] || die "QEMU patch stack is empty"

require_command git
[ -d "$BASE_SOURCE" ] || die "base QEMU checkout is absent: $BASE_SOURCE"
git -C "$BASE_SOURCE" rev-parse --is-inside-work-tree >/dev/null 2>&1 || \
    die "base QEMU source is not a Git checkout: $BASE_SOURCE"
[ "$(git -C "$BASE_SOURCE" rev-parse HEAD)" = "$QEMU_COMMIT" ] || \
    die "base QEMU checkout is not at $QEMU_COMMIT"

if [ -e "$SOURCE_DIR" ]; then
    [ -d "$SOURCE_DIR" ] || die "patched QEMU path is not a directory: $SOURCE_DIR"
    SOURCE_DIR=$SOURCE_DIR verify_checkout
    if git -C "$SOURCE_DIR" apply --reverse --check "$FINAL_PATCH" >/dev/null 2>&1; then
        verify_prepared_source
        printf '%s\n' "Patched Aula QEMU worktree is ready: $SOURCE_DIR"
        exit 0
    fi
    [ -z "$(git -C "$SOURCE_DIR" status --porcelain --untracked-files=normal)" ] || \
        die "patched worktree contains an incomplete or unrelated change set: $SOURCE_DIR"
else
    git -C "$BASE_SOURCE" worktree add --detach "$SOURCE_DIR" "$QEMU_COMMIT"
fi

git -C "$SOURCE_DIR" apply "$FINAL_PATCH" || die "QEMU patch does not apply"
verify_prepared_source
printf '%s\n' "Prepared modular Aula QEMU source: $SOURCE_DIR"
