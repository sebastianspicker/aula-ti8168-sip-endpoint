#!/bin/sh
# QEMU-only idempotent entrypoint for a staged, already verified payload.
set -eu

CURRENT=/opt/aula-ti8168-sip-endpoint/current
BOOTSTRAP=/opt/aula-ti8168-sip-endpoint/bootstrap
TRUSTED_VERIFIER=$BOOTSTRAP/verify-payload.sh
DIGEST_FILE=$BOOTSTRAP/payload-manifest.sha256

[ -f "$TRUSTED_VERIFIER" ] && [ ! -L "$TRUSTED_VERIFIER" ] || {
    printf '%s\n' 'error: trusted QEMU payload verifier is absent or unsafe' >&2
    exit 1
}
[ -f "$DIGEST_FILE" ] && [ ! -L "$DIGEST_FILE" ] &&
    [ "$(wc -l < "$DIGEST_FILE" | tr -d ' ')" = 1 ] || {
    printf '%s\n' 'error: QEMU payload digest receipt is absent or unsafe' >&2
    exit 1
}
IFS= read -r MANIFEST_SHA256 < "$DIGEST_FILE"
case $MANIFEST_SHA256 in ''|*[!0-9a-f]*) printf '%s\n' 'error: QEMU payload digest is unsafe' >&2; exit 1 ;; esac
[ "${#MANIFEST_SHA256}" -eq 64 ] || {
    printf '%s\n' 'error: QEMU payload digest is not SHA-256' >&2
    exit 1
}

if [ -L "$CURRENT" ]; then
    current_target=$(readlink "$CURRENT") || exit 1
    case $current_target in releases/*) current_version=${current_target#releases/} ;; *)
        printf '%s\n' 'error: active QEMU release selector is unsafe' >&2
        exit 1 ;
        ;;
    esac
    case $current_version in ''|*[!A-Za-z0-9._-]*)
        printf '%s\n' 'error: active QEMU release version is unsafe' >&2
        exit 1 ;;
    esac
    sh "$TRUSTED_VERIFIER" --payload "/opt/aula-ti8168-sip-endpoint/$current_target" \
        --manifest-sha256 "$MANIFEST_SHA256" >/dev/null
    printf '%s\n' 'AULA_ZOOM_ALREADY_INSTALLED'
    exit 0
fi
[ ! -e "$CURRENT" ] || {
    printf '%s\n' 'error: QEMU activation path exists and is not a symlink' >&2
    exit 1
}
[ -x "$BOOTSTRAP/install.sh" ] || {
    printf '%s\n' 'error: staged QEMU installer is absent' >&2
    exit 1
}
sh "$TRUSTED_VERIFIER" --payload "$BOOTSTRAP/runtime" \
    --manifest-sha256 "$MANIFEST_SHA256" >/dev/null

AULA_ZOOM_PAYLOAD_DIR=$BOOTSTRAP \
AULA_ZOOM_VERSION=qemu-prototype \
AULA_ZOOM_MANIFEST_SHA256=$MANIFEST_SHA256 \
    "$BOOTSTRAP/install.sh"
printf '%s\n' 'AULA_ZOOM_INSTALLED'
