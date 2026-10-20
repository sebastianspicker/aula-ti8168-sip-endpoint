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
require_runtime=0

# shellcheck source=lib/common.sh
. "$SCRIPT_DIR/lib/common.sh"
# shellcheck source=lib/patch-stack.sh
. "$SCRIPT_DIR/lib/patch-stack.sh"

while [ "$#" -gt 0 ]; do
    case $1 in
        --require-source) require_source=1 ;;
        --require-runtime) require_source=1; require_runtime=1 ;;
        *) die "usage: $0 [--require-source] [--require-runtime]" ;;
    esac
    shift
done

if [ "$require_runtime" -eq 1 ] && [ -z "${QEMU_BINARY:-}" ]; then
    die "--require-runtime requires QEMU_BINARY; runtime checks cannot be skipped"
fi

require_patch_stack
# shellcheck disable=SC2086 # PATCH_STACK is the intentional argument list.
set -- $PATCH_STACK

require_command python3

for script in "$SCRIPT_DIR"/*.sh; do
    sh -n "$script" || die "shell syntax failed: $script"
done
python3 -m py_compile "$SCRIPT_DIR"/probe_*.py
python3 -B "$SCRIPT_DIR/check_public_sources.py"

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
        ti8168_prcm.c ti8168_watchdog.c \
        ti8168_mediaboard.h
    do
        [ -s "$stack_source/hw/arm/$unit" ] ||
            die "modular QEMU source is missing: hw/arm/$unit"
    done
    [ -s "$stack_source/hw/gpio/ti8168_gpio.c" ] &&
        [ -s "$stack_source/include/hw/gpio/ti8168_gpio.h" ] ||
        die "modular QEMU GPIO source is missing"
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
    python3 -B "$SCRIPT_DIR/probe_hardware.py" "$QEMU_BINARY"
    python3 -B "$SCRIPT_DIR/probe_prcm.py" "$QEMU_BINARY"
    python3 -B "$SCRIPT_DIR/probe_gpio.py" "$QEMU_BINARY"
    python3 -B "$SCRIPT_DIR/probe_watchdog.py" "$QEMU_BINARY"
    python3 -B "$SCRIPT_DIR/probe_emac.py" "$QEMU_BINARY"
    python3 -B "$SCRIPT_DIR/probe_emac_chains.py" "$QEMU_BINARY"
    python3 -B "$SCRIPT_DIR/probe_phy.py" "$QEMU_BINARY"
    python3 -B "$SCRIPT_DIR/probe_boot.py" "$QEMU_BINARY"
    python3 -B "$SCRIPT_DIR/probe_bootstrap.py" "$QEMU_BINARY"
    python3 -B "$SCRIPT_DIR/probe_guest.py" "$QEMU_BINARY" \
        --assembler "${QEMU_ARM_AS:-arm-none-eabi-as}" \
        --linker "${QEMU_ARM_LD:-arm-none-eabi-ld}"
    printf '%s\n' "Built TI8168 model and ARM guest execution verified: $QEMU_BINARY"
else
    printf '%s\n' 'QEMU_BINARY not supplied; built-machine probe skipped.'
fi

if [ -n "${QEMU_BINARY:-}" ]; then
    printf '%s\n' 'TI8168 model checks passed; see FIDELITY.md for unvalidated hardware.'
else
    printf '%s\n' 'Source-only checks passed; no runtime or hardware fidelity result.'
fi
