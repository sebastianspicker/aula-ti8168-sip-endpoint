# shellcheck shell=sh
# The single maintained QEMU overlay patch shared by preparation, verification, and apply.

BOARD_PATCH="$PROTO_DIR/patches/ti8168-mediaboard-model.patch"

PATCH_STACK="$BOARD_PATCH"

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
