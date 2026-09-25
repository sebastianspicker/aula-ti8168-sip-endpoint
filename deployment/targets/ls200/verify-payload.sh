#!/bin/sh
# Trusted installer-side verifier.  It never executes a payload artifact.
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

die() { printf '%s\n' "live-verify: $*" >&2; exit 1; }
sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk 'NR == 1 {print $1}';
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk 'NR == 1 {print $1}';
    else die 'sha256sum or shasum is required'; fi
}
payload= manifest_sha=
while [ "$#" -gt 0 ]; do
    case $1 in
        --payload) payload=$2; shift 2 ;;
        --manifest-sha256) manifest_sha=$2; shift 2 ;;
        *) die 'usage: verify-payload.sh --payload DIR --manifest-sha256 SHA256' ;;
    esac
done
case $payload in /*) ;; *) die 'payload must be absolute' ;; esac
for digest in "$manifest_sha"; do
    case $digest in ''|*[!0-9a-f]*) die 'expected digest is unsafe' ;; esac
    [ "${#digest}" = 64 ] || die 'expected digest must be SHA-256'
done
[ -d "$payload" ] && [ ! -L "$payload" ] || die 'payload directory is unsafe'
manifest=$payload/payload-manifest.tsv
[ -f "$manifest" ] && [ ! -L "$manifest" ] || die 'payload manifest is unsafe'
find "$payload" ! -exec sh -c '
    case $1 in "$2") exit 0 ;; "$2"/*) relative=${1#"$2"/} ;; *) exit 1 ;; esac
    newline="
"
    case $relative in *"$newline"*) exit 1 ;; esac
    case $relative in ""|*[!A-Za-z0-9._/+:-]*) exit 1 ;; esac
' sh {} "$payload" \; -print | grep -q . && die 'payload path contains an unsafe character'
[ "$(find "$payload" ! -type d ! -type f -print | wc -l | tr -d ' ')" = 0 ] || die 'payload contains a special file or symlink'
find "$payload" -type f \( -perm -020 -o -perm -002 \) -print | grep -q . && die 'payload contains a writable file'
actual=$(sha256_file "$manifest")
[ "$actual" = "$manifest_sha" ] || die 'manifest digest mismatch'
IFS= read -r header < "$manifest" || true
[ "$header" = ls200-zoom-payload-v1 ] || die 'unsupported manifest format'
tab=$(printf '\t')
count=0
unset TMPDIR
work=$(mktemp -d /tmp/ls200-live-verify.XXXXXX) || die 'cannot create private verifier workspace'
entries=$work/entries
files=$work/files
paths=$work/paths
trap 'rm -rf "$work"' 0 1 2 15
sed '1d' "$manifest" > "$entries"
for required in \
    bin/ls200-sipd bin/ls200-gateway-fcgi bin/ls200-device-control bin/nginx bin/ls200-atomic-replace \
    bin/ls200-zoom-service bin/verify-ls200-zoom-payload \
    etc/init.d/S99ls200-zoom etc/nginx/nginx.conf etc/nginx/mime.types \
    etc/nginx/fastcgi_params etc/ls200-zoom/ls200-sipd.conf.example \
    etc/ls200-zoom/gateway.conf.example ui/zoom/index.html
do
    [ "$(awk -F "$tab" -v path="$required" '$1 == path { count++ } END { print count + 0 }' "$entries")" = 1 ] ||
        die "manifest must list required path exactly once: $required"
done
: > "$paths"
while IFS="$tab" read -r relative mode expected size extra; do
    [ -z "$extra" ] || exit 1
    case $relative in ''|*[!A-Za-z0-9._/+:-]*) exit 1 ;; esac
    case $relative:$mode:$expected:$size in *'..'*:*|/*:*|*'//'*) exit 1 ;; esac
    case $mode in 0444|0555) ;; *) exit 1 ;; esac
    case $expected in [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) ;; *) exit 1 ;; esac
    [ "${#expected}" = 64 ] || exit 1
    case $size in ''|*[!0-9]*) exit 1 ;; esac
    file=$payload/$relative
    [ -f "$file" ] && [ ! -L "$file" ] || exit 1
    find "$file" -prune \( -perm -020 -o -perm -002 \) -print | grep -q . && exit 1
    permissions=$(LC_ALL=C ls -ld "$file" | awk 'NR == 1 {print $1}')
    permissions=${permissions%@}; permissions=${permissions%+}
    case $mode:$permissions in 0444:-r--r--r--|0555:-r-xr-xr-x) ;; *) exit 1 ;; esac
    [ "$(wc -c < "$file" | tr -d ' ')" = "$size" ] || exit 1
    [ "$(sha256_file "$file")" = "$expected" ] || exit 1
    printf '%s\n' "$relative" >> "$paths"
    count=$((count + 1))
done < "$entries" || die 'manifest content validation failed'
[ "$count" -gt 0 ] || die 'manifest has no entries'
LC_ALL=C sort "$paths" | uniq -d | grep -q . && die 'manifest contains a duplicate path'
find "$payload" -type f ! -path "$manifest" -print > "$files"
while IFS= read -r file; do
    relative=${file#"$payload/"}
    awk -F "$tab" -v path="$relative" '$1 == path { found = 1 } END { exit !found }' "$entries" || die "unlisted payload file: $relative"
done < "$files"
rm -rf "$work"
trap - 0 1 2 15
printf '%s\n' 'LS-200 Zoom payload verified by trusted installer' || true
exit 0
