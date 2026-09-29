# shellcheck shell=sh
QEMU_TAG=v11.0.3
QEMU_COMMIT=aeec49e8170de7846f476124602cf7acd400c3df

die() {
    printf '%s\n' "error: $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is required"
}

verify_checkout() {
    git -C "$SOURCE_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1 || \
        die "not a Git checkout: $SOURCE_DIR"
    tag_commit=$(git -C "$SOURCE_DIR" rev-parse "${QEMU_TAG}^{commit}") || \
        die "missing QEMU tag $QEMU_TAG in $SOURCE_DIR"
    head_commit=$(git -C "$SOURCE_DIR" rev-parse HEAD) || die "cannot read HEAD"
    [ "$tag_commit" = "$QEMU_COMMIT" ] || \
        die "tag $QEMU_TAG resolves to $tag_commit, expected $QEMU_COMMIT"
    [ "$head_commit" = "$QEMU_COMMIT" ] || \
        die "HEAD is $head_commit, expected $QEMU_COMMIT${QEMU_HEAD_REFUSAL_SUFFIX:-}"
}
