#!/bin/sh
# Verify the synthetic TI8168 QEMU patch stack and machine.
set -eu

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)
PROTO_DIR=$(CDPATH='' cd "$SCRIPT_DIR/.." && pwd)
REPO_DIR=$(CDPATH='' cd "$PROTO_DIR/../.." && pwd)
WORK_DIR=${QEMU_WORK_DIR:-"$REPO_DIR/.work/build/qemu"}
SOURCE_DIR=${QEMU_SOURCE:-"$WORK_DIR/qemu-v11.0.3"}
PYTHONPYCACHEPREFIX=${PYTHONPYCACHEPREFIX:-"$REPO_DIR/.work/cache/python"}
export PYTHONPYCACHEPREFIX
require_source=0

# shellcheck source=lib/common.sh
. "$SCRIPT_DIR/lib/common.sh"
# shellcheck source=lib/patch-stack.sh
. "$SCRIPT_DIR/lib/patch-stack.sh"

case ${1:-} in
    '') ;;
    --require-source) require_source=1 ;;
    *) die "usage: $0 [--require-source]" ;;
esac

require_patch_stack
# shellcheck disable=SC2086 # PATCH_STACK is the intentional argument list.
set -- $PATCH_STACK

require_command python3

for script in "$SCRIPT_DIR"/*.sh; do
    sh -n "$script" || die "shell syntax failed: $script"
done
python3 -m py_compile "$SCRIPT_DIR"/probe_*.py

if command -v shellcheck >/dev/null 2>&1; then
    shellcheck -x -P "$SCRIPT_DIR" -s sh "$SCRIPT_DIR"/*.sh
else
    printf '%s\n' 'ShellCheck not installed; skipped optional lint.'
fi

verify_patch_stack() {
    stack_root=$(mktemp -d "${TMPDIR:-/tmp}/aula-patch-stack.XXXXXX") ||
        die "cannot create temporary patch-stack directory"
    stack_source="$stack_root/qemu"
    cleanup_stack() {
        git -C "$SOURCE_DIR" worktree remove --force "$stack_source" \
            >/dev/null 2>&1 || true
        rmdir "$stack_root" 2>/dev/null || true
    }
    trap cleanup_stack EXIT HUP INT TERM
    git -C "$SOURCE_DIR" worktree add --detach "$stack_source" "$QEMU_COMMIT" \
        >/dev/null || die "cannot create temporary QEMU worktree"
    git -C "$stack_source" apply "$@" ||
        die "ordered QEMU patch stack does not apply cleanly"
    git -C "$stack_source" diff --check ||
        die "ordered QEMU patch stack has whitespace errors"
    for unit in ti8168_mediaboard.c ti8168_intc.c ti8168_timer.c ti8168_nand.c \
        ti8168_gpmc.c ti8168_bootstrap.c ti8168_probe_regs.c ti8168_emac.c \
        ti8168_mediaboard.h
    do
        [ -s "$stack_source/hw/arm/$unit" ] ||
            die "modular QEMU source is missing: hw/arm/$unit"
    done
    cleanup_stack
    trap - EXIT HUP INT TERM
}

if [ -d "$SOURCE_DIR" ]; then
    require_command git
    verify_checkout
    verify_patch_stack "$@"
    printf '%s\n' "Ordered modular patch stack verified: $SOURCE_DIR"
elif [ "$require_source" -eq 1 ]; then
    die "QEMU checkout is required but absent: $SOURCE_DIR"
else
    printf '%s\n' "QEMU checkout absent; source patch application skipped."
fi

if [ -n "${QEMU_BINARY:-}" ]; then
    [ -x "$QEMU_BINARY" ] || die "QEMU binary is not executable: $QEMU_BINARY"
    "$QEMU_BINARY" -machine help | grep -Eq '^ti8168-mediaboard[[:space:]]' ||
        die "ti8168-mediaboard is absent from QEMU machine list"
    python3 -B "$SCRIPT_DIR/probe_synthetic_machine.py" "$QEMU_BINARY"
    printf '%s\n' "Built synthetic TI8168 machine verified: $QEMU_BINARY"
else
    printf '%s\n' 'QEMU_BINARY not supplied; built-machine qtests skipped.'
fi

printf '%s\n' 'Synthetic TI8168 QEMU checks passed.'
