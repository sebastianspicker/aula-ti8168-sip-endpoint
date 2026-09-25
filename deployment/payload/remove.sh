#!/bin/sh
# Remove only paths recorded by this deployment kit. Modified links are retained.
set -eu
umask 077

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)
die() { printf '%s\n' "error: $*" >&2; exit 1; }
ROOT=${LS200_ZOOM_ROOT:-/}
case $ROOT in /) ROOT= ;; /*) ROOT=${ROOT%/} ;; *) printf '%s\n' 'error: root must be absolute' >&2; exit 1 ;; esac
STATE=$ROOT/var/lib/cbox/ls200-zoom
JOURNAL=$STATE/owned-files
NGINX_SELECTOR=$ROOT/var/lib/cbox/etc/nginx/nginx.conf
NGINX_COPY_HTTP=$STATE/nginx_zoom_http.conf
NGINX_COPY_HTTPS=$STATE/nginx_zoom_https.conf
NGINX_OWNED_HTTP_TARGET=/var/lib/cbox/ls200-zoom/nginx_zoom_http.conf
NGINX_OWNED_HTTPS_TARGET=/var/lib/cbox/ls200-zoom/nginx_zoom_https.conf
NGINX_SELECTOR_BACKUP=$STATE/nginx-selector.original
LOCK=$STATE/remove.lock
BOOTSTRAP_TX=$STATE/bootstrap-transaction
NGINX_SELECTOR_RESTORED=0
NGINX_SELECTOR_SEEN=0
NGINX_COPY_HTTP_SEEN=0
NGINX_COPY_HTTPS_SEEN=0
[ -d "$STATE" ] && [ ! -L "$STATE" ] || { printf '%s\n' 'error: unsafe state directory' >&2; exit 1; }
[ -O "$STATE" ] || { printf '%s\n' 'error: state directory has unexpected owner' >&2; exit 1; }
if find "$STATE" -prune \( -perm -002 -o -perm -020 \) | grep -q .; then
    printf '%s\n' 'error: state directory is group/world writable' >&2
    exit 1
fi
[ -f "$JOURNAL" ] && [ ! -L "$JOURNAL" ] || { printf '%s\n' 'error: owned-file journal is absent or unsafe' >&2; exit 1; }
[ -O "$JOURNAL" ] || { printf '%s\n' 'error: owned-file journal has unexpected owner' >&2; exit 1; }
if find "$JOURNAL" -prune \( -perm -002 -o -perm -020 \) | grep -q .; then
    printf '%s\n' 'error: owned-file journal is group/world writable' >&2
    exit 1
fi
[ ! -e "$LOCK" ] || { printf '%s\n' 'error: another removal is active' >&2; exit 1; }
mkdir -m 0700 "$LOCK"
trap 'rm -f "$LOCK/journal"; rmdir "$LOCK"' EXIT HUP INT TERM
[ ! -e "$BOOTSTRAP_TX" ] && [ ! -L "$BOOTSTRAP_TX" ] ||
    die 'bootstrap transaction is unresolved; rerun the installer before removal'
# The validated state directory is private to its owner, so copying the journal
# into the removal lock pins the bytes used by both validation and deletion.
cp "$JOURNAL" "$LOCK/journal"
chmod 0600 "$LOCK/journal"
PINNED_JOURNAL=$LOCK/journal

release_path() {
    case $1 in
        ''|.|..|*[!A-Za-z0-9._-]*) return 1 ;;
    esac
    printf '%s/opt/ls200-zoom/releases/%s\n' "$ROOT" "$1"
}

require_regular_file() {
    [ -f "$1" ] && [ ! -L "$1" ] || { printf '%s\n' "error: required file is absent or unsafe: $1" >&2; exit 1; }
}

sha256_file() {
    require_regular_file "$1"
    command -v sha256sum >/dev/null 2>&1 || { printf '%s\n' 'error: sha256sum is required for vendor rollback safety' >&2; exit 1; }
    digest=$(sha256sum "$1" | awk 'NR == 1 { print $1 }') || { printf '%s\n' "error: cannot hash required file: $1" >&2; exit 1; }
    [ "${#digest}" -eq 64 ] || { printf '%s\n' "error: invalid SHA-256 digest for file: $1" >&2; exit 1; }
    case $digest in *[!0123456789abcdef]*) printf '%s\n' "error: invalid SHA-256 digest for file: $1" >&2; exit 1 ;; esac
    printf '%s\n' "$digest"
}

NGINX_RECORDS=$SCRIPT_DIR/nginx-records.sh
require_regular_file "$NGINX_RECORDS"
[ -O "$NGINX_RECORDS" ] || die 'nginx record helper has unexpected owner'
find "$NGINX_RECORDS" -prune \( -perm -020 -o -perm -002 \) | grep -q . && die 'nginx record helper is writable by others'
# shellcheck source=nginx-records.sh
. "$NGINX_RECORDS"








verify_nginx_copy_for_removal() {
    mode=$1
    copy=$(nginx_copy_path "$mode")
    require_regular_file "$copy"
    set -- $(read_nginx_copy_hashes "$mode")
    patched=$2
    [ "$(sha256_file "$copy")" = "$patched" ] || {
            printf '%s\n' "error: refusing to remove changed nginx owned copy: $copy" >&2
            exit 1
        }
}

verify_nginx_selector_for_restore() {
    [ -L "$NGINX_SELECTOR" ] || { printf '%s\n' "error: nginx selector is not a symlink: $NGINX_SELECTOR" >&2; exit 1; }
    set -- $(read_selector_backup)
    original_target=$1
    mode=$2
    expected=$(nginx_owned_target "$mode")
    case $mode:$original_target in
        http:nginx_http.conf|http:/var/lib/cbox/etc/nginx/nginx_http.conf|http:/usr/share/nginx/nginx_http.conf|https:nginx_https.conf|https:/var/lib/cbox/etc/nginx/nginx_https.conf|https:/usr/share/nginx/nginx_https.conf) ;;
        *) printf '%s\n' 'error: nginx selector backup has an unfamiliar target' >&2; exit 1 ;;
    esac
    [ "$(readlink "$NGINX_SELECTOR")" = "$expected" ] || {
        printf '%s\n' 'error: refusing to restore changed nginx selector' >&2
        exit 1
    }
}

restore_nginx_selector() {
    verify_nginx_selector_for_restore
    set -- $(read_selector_backup)
    original_target=$1
    temporary=$(dirname "$NGINX_SELECTOR")/.nginx.conf.ls200-zoom-remove.$$
    [ ! -e "$temporary" ] && [ ! -L "$temporary" ] || { printf '%s\n' "error: nginx selector removal temporary path already exists: $temporary" >&2; exit 1; }
    ln -s "$original_target" "$temporary" || { printf '%s\n' 'error: cannot prepare nginx selector restoration' >&2; exit 1; }
    mv "$temporary" "$NGINX_SELECTOR" || { rm -f "$temporary"; printf '%s\n' 'error: cannot restore nginx selector' >&2; exit 1; }
    [ "$(readlink "$NGINX_SELECTOR")" = "$original_target" ] || { printf '%s\n' 'error: nginx selector restoration mismatch' >&2; exit 1; }
    NGINX_SELECTOR_RESTORED=1
}

remove_nginx_copy() {
    mode=$1
    copy=$(nginx_copy_path "$mode")
    hashes=$(nginx_hash_path "$mode")
    verify_nginx_copy_for_removal "$mode"
    rm -f "$copy" "$hashes"
}

while IFS=: read -r kind value; do
    [ -n "$kind" ] || continue
    case $kind:$value in
        link:/etc/init.d/S99ls200-zoom)
            target=$ROOT$value
            expected=/opt/ls200-zoom/current/etc/init.d/S99ls200-zoom ;;
        link:/etc/nginx/conf.d/zz-ls200-zoom-legacy.conf)
            target=$ROOT$value
            expected=/opt/ls200-zoom/current/etc/nginx/conf.d/zz-ls200-zoom-legacy.conf ;;
        link:/opt/ls200-zoom/current)
            target=$ROOT$value
            expected= ;;
        nginx-copy:http)
            [ "$NGINX_COPY_HTTP_SEEN" = 0 ] || { printf '%s\n' 'error: duplicate nginx HTTP copy journal entry' >&2; exit 1; }
            verify_nginx_copy_for_removal http
            NGINX_COPY_HTTP_SEEN=1
            continue ;;
        nginx-copy:https)
            [ "$NGINX_COPY_HTTPS_SEEN" = 0 ] || { printf '%s\n' 'error: duplicate nginx HTTPS copy journal entry' >&2; exit 1; }
            verify_nginx_copy_for_removal https
            NGINX_COPY_HTTPS_SEEN=1
            continue ;;
        nginx-selector:/var/lib/cbox/etc/nginx/nginx.conf)
            [ "$NGINX_SELECTOR_SEEN" = 0 ] || { printf '%s\n' 'error: duplicate nginx selector journal entry' >&2; exit 1; }
            verify_nginx_selector_for_restore
            NGINX_SELECTOR_SEEN=1
            continue ;;
        release-sha256:*)
            version=${value%:*}; digest=${value##*:}
            release_path "$version" >/dev/null || die 'unsafe release digest identifier'
            case $digest in ''|*[!0-9a-f]*) die 'unsafe release digest' ;; esac
            [ "${#digest}" = 64 ] || die 'release digest is not SHA-256'
            [ "$(grep -Fxc "release:$version" "$PINNED_JOURNAL")" = 1 ] || die 'release digest is not uniquely paired'
            [ "$(grep -Fc "release-sha256:$version:" "$PINNED_JOURNAL")" = 1 ] || die 'release digest is ambiguous'
            continue ;;
        release:*)
            release=$(release_path "$value") || { printf '%s\n' "error: invalid release identifier: $value" >&2; exit 1; }
            [ -d "$release" ] && [ ! -L "$release" ] || { printf '%s\n' "error: owned release is absent or unsafe: $release" >&2; exit 1; }
            continue ;;
        *) printf '%s\n' "error: journal contains unsupported entry: $kind:$value" >&2; exit 1 ;;
    esac
    [ -L "$target" ] || { printf '%s\n' "error: refusing to remove changed path: $target" >&2; exit 1; }
    [ -z "$expected" ] || [ "$(readlink "$target")" = "$expected" ] ||
        { printf '%s\n' "error: refusing to remove retargeted path: $target" >&2; exit 1; }
done < "$PINNED_JOURNAL"

[ "$NGINX_COPY_HTTP_SEEN" = 1 ] && [ "$NGINX_COPY_HTTPS_SEEN" = 1 ] &&
    [ "$NGINX_SELECTOR_SEEN" = 1 ] || {
        printf '%s\n' 'error: nginx owned state journal is incomplete' >&2
        exit 1
    }

while IFS=: read -r kind value; do
    case $kind:$value in
        nginx-selector:/var/lib/cbox/etc/nginx/nginx.conf)
            restore_nginx_selector ;;
        nginx-copy:http|nginx-copy:https)
            : ;;
        link:*) rm "$ROOT$value" ;;
        release:*)
            release=$(release_path "$value") || exit 1
            # Releases are immutable while active; make only this journaled
            # release removable after every link has passed validation. find
            # does not follow a substituted symlink, and the identifier cannot
            # escape the releases directory.
            find "$release" -type d -exec chmod u+w {} +
            rm -rf "$release"
            ;;
    esac
done < "$PINNED_JOURNAL"
if [ "$NGINX_SELECTOR_RESTORED" = 1 ]; then
    remove_nginx_copy http
    remove_nginx_copy https
    rm -f "$NGINX_SELECTOR_BACKUP"
fi
rm -f "$JOURNAL" "$STATE/active-version"
printf '%s\n' 'removed LS-200 Zoom owned files; persistent state directory retained'
