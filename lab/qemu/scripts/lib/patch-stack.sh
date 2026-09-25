# shellcheck shell=sh
# Ordered QEMU overlay paths shared by preparation, verification, and apply.

BOARD_PATCH=${QEMU_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-ls200.patch"}
INITRD_PATCH=${QEMU_INITRD_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-initrd-placement.patch"}
BOOT_MATRIX_PATCH=${QEMU_BOOT_MATRIX_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-boot-matrix.patch"}
SYNTHETIC_NAND_PATCH=${QEMU_SYNTHETIC_NAND_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-synthetic-nand.patch"}
OFFLINE_PROGRESS_PATCH=${QEMU_OFFLINE_PROGRESS_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-offline-progress.patch"}
MUX_FAPLL_PATCH=${QEMU_MUX_FAPLL_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-mux-fapll-compat.patch"}
EDMA_CC_BOOTSTRAP_PATCH=${QEMU_EDMA_CC_BOOTSTRAP_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-edma-cc-bootstrap.patch"}
PCIESS_BOOTSTRAP_PATCH=${QEMU_PCIESS_BOOTSTRAP_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-pciess-bootstrap.patch"}
MCSPI_BOOTSTRAP_PATCH=${QEMU_MCSPI_BOOTSTRAP_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-mcspi-bootstrap.patch"}
RECOVERED_NAND_PATCH=${QEMU_RECOVERED_NAND_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-recovered-nand.patch"}
MODULE_REFACTOR_PATCH=${QEMU_MODULE_REFACTOR_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-module-refactor.patch"}
FULL_NAND_PATCH=${QEMU_FULL_NAND_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-full-nand-map.patch"}
NAND_COW_PATCH=${QEMU_NAND_COW_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-nand-cow.patch"}
GPMC_PREFETCH_PATCH=${QEMU_GPMC_PREFETCH_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-gpmc-prefetch.patch"}
OMAP_UART_IDLE_PATCH=${QEMU_OMAP_UART_IDLE_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-omap-uart-idle.patch"}
EMAC_CPDMA_PATCH=${QEMU_EMAC_CPDMA_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-emac-cpdma.patch"}
QUALITY_REFACTOR_PATCH=${QEMU_QUALITY_REFACTOR_PATCH_FILE:-"$PROTO_DIR/patches/ti8168-quality-refactor.patch"}

PATCH_STACK="$BOARD_PATCH $INITRD_PATCH $BOOT_MATRIX_PATCH
$SYNTHETIC_NAND_PATCH $OFFLINE_PROGRESS_PATCH $MUX_FAPLL_PATCH
$EDMA_CC_BOOTSTRAP_PATCH $PCIESS_BOOTSTRAP_PATCH $MCSPI_BOOTSTRAP_PATCH
$RECOVERED_NAND_PATCH $MODULE_REFACTOR_PATCH $FULL_NAND_PATCH
$GPMC_PREFETCH_PATCH $NAND_COW_PATCH $OMAP_UART_IDLE_PATCH $EMAC_CPDMA_PATCH
$QUALITY_REFACTOR_PATCH"

require_patch_stack() {
    # shellcheck disable=SC2086 # PATCH_STACK is the intentional argument list.
    for patch_file in $PATCH_STACK; do
        [ -s "$patch_file" ] || die "required QEMU patch is missing: $patch_file"
    done
}

verify_prepared_source() {
    # shellcheck disable=SC2086 # PATCH_STACK is the intentional argument list.
    python3 -B "$PROTO_DIR/scripts/verify_prepared_source.py" \
        --source "$SOURCE_DIR" --commit "$QEMU_COMMIT" $PATCH_STACK
}
