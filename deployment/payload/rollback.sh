#!/bin/sh
# Atomically select an already installed immutable LS-200 Zoom release.
set -eu

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)
TRUSTED_VERIFIER=$SCRIPT_DIR/verify-payload.sh
if [ ! -e "$TRUSTED_VERIFIER" ] && [ ! -L "$TRUSTED_VERIFIER" ]; then
    TRUSTED_VERIFIER=$SCRIPT_DIR/../targets/ls200/verify-payload.sh
fi
ROOT=${LS200_ZOOM_ROOT:-/}
VERSION=${1:-}
case $ROOT in /) ;; /*) ROOT=${ROOT%/} ;; *) printf '%s\n' 'error: root must be absolute' >&2; exit 1 ;; esac
case $VERSION in ''|.|..|*[!A-Za-z0-9._-]*) printf '%s\n' 'error: provide an installed version' >&2; exit 1 ;; esac

PREFIX=$ROOT/opt/ls200-zoom
STATE=$ROOT/var/lib/cbox/ls200-zoom
JOURNAL=$STATE/owned-files
die() { printf '%s\n' "error: $*" >&2; exit 1; }
[ -d "$STATE" ] && [ ! -L "$STATE" ] && [ -O "$STATE" ] || die 'unsafe rollback state directory'
[ -f "$JOURNAL" ] && [ ! -L "$JOURNAL" ] && [ -O "$JOURNAL" ] || die 'unsafe rollback journal'
find "$STATE" "$JOURNAL" -prune \( -perm -020 -o -perm -002 \) | grep -q . && die 'rollback trust state is writable by others'
[ "$(grep -Fxc "release:$VERSION" "$JOURNAL")" = 1 ] || die 'rollback release is not uniquely owned'
MANIFEST_SHA256=$(awk -F: -v version="$VERSION" '
    $1 == "release-sha256" && $2 == version { count++; digest=$3; if (NF != 3) bad=1 }
    END { if (count != 1 || bad) exit 1; print digest }
' "$JOURNAL") || die 'independent rollback digest is absent or ambiguous'
RELEASE=$PREFIX/releases/$VERSION
[ -d "$RELEASE" ] || { printf '%s\n' "error: release is absent: $VERSION" >&2; exit 1; }
[ -e "$PREFIX/current" ] || [ -L "$PREFIX/current" ] || { printf '%s\n' 'error: no active release' >&2; exit 1; }
[ -L "$PREFIX/current" ] || { printf '%s\n' 'error: activation target is not a symlink' >&2; exit 1; }
[ -f "$TRUSTED_VERIFIER" ] && [ ! -L "$TRUSTED_VERIFIER" ] && [ -O "$TRUSTED_VERIFIER" ] || {
    printf '%s\n' 'error: trusted rollback verifier is absent or unsafe' >&2; exit 1;
}
find "$TRUSTED_VERIFIER" -prune \( -perm -020 -o -perm -002 \) | grep -q . && die 'rollback verifier is writable by others'
sh "$TRUSTED_VERIFIER" --payload "$RELEASE" --manifest-sha256 "$MANIFEST_SHA256" >/dev/null
ln -s "releases/$VERSION" "$PREFIX/.rollback-$VERSION-$$"
"$RELEASE/bin/ls200-atomic-replace" \
    "$PREFIX/.rollback-$VERSION-$$" "$PREFIX/current" || {
    printf '%s\n' 'error: atomic rollback activation failed' >&2
    exit 1
}
sh "$TRUSTED_VERIFIER" --payload "$RELEASE" --manifest-sha256 "$MANIFEST_SHA256" >/dev/null
printf '%s\n' "$VERSION" > "$STATE/active-version"
printf '%s\n' "rolled back LS-200 Zoom to $VERSION"
