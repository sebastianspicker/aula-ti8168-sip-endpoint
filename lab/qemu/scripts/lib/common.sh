# shellcheck shell=sh
QEMU_TAG=v11.0.3
QEMU_COMMIT=aeec49e8170de7846f476124602cf7acd400c3df
DEFAULT_KERNEL_SHA256=e2a7e1bcf10bf78022a86dbf469c30a7af26405e2c6f5d35eae4fb42776225fb

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

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        die "shasum or sha256sum is required to verify the default kernel"
    fi
}

verify_default_kernel() {
    actual_sha256=$(sha256_file "$DEFAULT_KERNEL_IMAGE")
    [ "$actual_sha256" = "$DEFAULT_KERNEL_SHA256" ] || \
        die "default kernel SHA-256 is $actual_sha256, expected $DEFAULT_KERNEL_SHA256"
}
