#!/bin/sh
# Cross-build the deployment-only atomic symlink activation helper.
set -eu

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)
REPOSITORY=$(CDPATH='' cd "$SCRIPT_DIR/../.." && pwd -P)
PATH_GUARD=$REPOSITORY/product/sipd/tools/protected_output.py
OUTPUT=${1:-}
CC=${CC:-arm-linux-gnueabi-gcc}

[ -n "$OUTPUT" ] || {
    printf '%s\n' "usage: CC=arm-linux-gnueabi-gcc $0 OUTPUT" >&2
    exit 64
}
case $OUTPUT in
    /*) ;;
    *) OUTPUT=$PWD/$OUTPUT ;;
esac
[ -d "$(dirname "$OUTPUT")" ] || {
    printf '%s\n' "error: output parent does not exist: $(dirname "$OUTPUT")" >&2
    exit 1
}
OUTPUT=$(python3 "$PATH_GUARD" "$REPOSITORY" "$OUTPUT") || exit 2
CC_PATH=$(command -v "$CC") || {
    printf '%s\n' "error: cross compiler is unavailable: $CC" >&2
    exit 2
}
CC_PATH=$(python3 "$PATH_GUARD" --input "$REPOSITORY" "$CC_PATH") || exit 2

"$CC_PATH" -std=c11 -Os -Wall -Wextra -Werror -fPIE -fstack-protector-strong \
    -D_FORTIFY_SOURCE=2 -Wl,-z,relro,-z,now -pie \
    "$SCRIPT_DIR/atomic-replace.c" -o "$OUTPUT"
chmod 0555 "$OUTPUT"
printf '%s\n' "built $OUTPUT"
