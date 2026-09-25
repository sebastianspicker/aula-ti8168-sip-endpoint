#!/bin/sh
# Build a root-relative, COW-safe QEMU overlay containing the deployment kit.
set -eu

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)
DEPLOYMENT_DIR=$(CDPATH='' cd "$SCRIPT_DIR/.." && pwd)
REPOSITORY=$(CDPATH='' cd "$DEPLOYMENT_DIR/.." && pwd)
QEMU_TARGET="$DEPLOYMENT_DIR/targets/qemu"
TRUSTED_VERIFIER="$DEPLOYMENT_DIR/targets/ls200/verify-payload.sh"
BOOTSTRAP_HELPER="$DEPLOYMENT_DIR/targets/ls200/bootstrap-transaction.sh"
PATH_GUARD="$REPOSITORY/tooling/workspace/protected_output.py"
PAYLOAD=${1:-}
OUTPUT=${2:-}
MANIFEST_SHA256=${3:-${LS200_ZOOM_MANIFEST_SHA256:-}}
QEMU_PROFILE=${LS200_ZOOM_QEMU_PROFILE:-0}
QEMU_ENTROPY_HELPER=${LS200_ZOOM_QEMU_ENTROPY_HELPER:-}
[ -n "$PAYLOAD" ] && [ -n "$OUTPUT" ] && [ -n "$MANIFEST_SHA256" ] || {
    printf '%s\n' "usage: $0 PAYLOAD_DIRECTORY OUTPUT_DIRECTORY MANIFEST_SHA256" >&2
    exit 1
}
case $OUTPUT in /*) ;; *) printf '%s\n' 'error: output must be absolute' >&2; exit 1 ;; esac
[ ! -e "$OUTPUT" ] || { printf '%s\n' "error: output already exists: $OUTPUT" >&2; exit 1; }
case $MANIFEST_SHA256 in *[!0-9a-f]*|'') printf '%s\n' 'error: manifest digest must be lowercase SHA-256' >&2; exit 1 ;; esac
[ "${#MANIFEST_SHA256}" -eq 64 ] || { printf '%s\n' 'error: manifest digest must be SHA-256' >&2; exit 1; }
[ -d "$PAYLOAD" ] && [ ! -L "$PAYLOAD" ] || {
    printf '%s\n' "error: payload must be a non-symlink directory: $PAYLOAD" >&2
    exit 1
}
[ -f "$TRUSTED_VERIFIER" ] && [ ! -L "$TRUSTED_VERIFIER" ] || {
    printf '%s\n' 'error: trusted installer verifier is absent or unsafe' >&2
    exit 1
}
[ -f "$BOOTSTRAP_HELPER" ] && [ ! -L "$BOOTSTRAP_HELPER" ] || {
    printf '%s\n' 'error: trusted bootstrap transaction helper is absent or unsafe' >&2
    exit 1
}
PAYLOAD=$(python3 "$PATH_GUARD" --input "$REPOSITORY" "$PAYLOAD") || exit 2
OUTPUT=$(python3 "$PATH_GUARD" "$REPOSITORY" "$OUTPUT") || exit 2
case $QEMU_PROFILE in
    0|1) ;;
    *) printf '%s\n' 'error: LS200_ZOOM_QEMU_PROFILE must be 0 or 1' >&2; exit 1 ;;
esac
sh "$TRUSTED_VERIFIER" --payload "$PAYLOAD" --manifest-sha256 "$MANIFEST_SHA256" >/dev/null
STAGING=$OUTPUT.staging.$$
[ ! -e "$STAGING" ] && [ ! -L "$STAGING" ] || {
    printf '%s\n' "error: staging path already exists: $STAGING" >&2
    exit 1
}
trap 'rm -rf "$STAGING"' EXIT HUP INT TERM
mkdir -p "$STAGING/opt/ls200-zoom/bootstrap"
cp "$SCRIPT_DIR/install.sh" "$SCRIPT_DIR/rollback.sh" "$SCRIPT_DIR/remove.sh" "$SCRIPT_DIR/nginx-records.sh" \
    "$QEMU_TARGET/qemu-install.sh" "$TRUSTED_VERIFIER" "$BOOTSTRAP_HELPER" \
    "$STAGING/opt/ls200-zoom/bootstrap/"
cp -Rp "$PAYLOAD" "$STAGING/opt/ls200-zoom/bootstrap/runtime"
BOOTSTRAP=$STAGING/opt/ls200-zoom/bootstrap
chmod 0755 "$STAGING" "$STAGING/opt" "$STAGING/opt/ls200-zoom" "$BOOTSTRAP"
printf '%s\n' "$MANIFEST_SHA256" > "$BOOTSTRAP/payload-manifest.sha256"
chmod 0444 "$BOOTSTRAP/payload-manifest.sha256"
chmod 0555 "$BOOTSTRAP/install.sh" "$BOOTSTRAP/rollback.sh" \
    "$BOOTSTRAP/remove.sh" "$BOOTSTRAP/nginx-records.sh" \
    "$BOOTSTRAP/qemu-install.sh" "$BOOTSTRAP/verify-payload.sh" \
    "$BOOTSTRAP/bootstrap-transaction.sh"
sh "$BOOTSTRAP/verify-payload.sh" --payload "$BOOTSTRAP/runtime" \
    --manifest-sha256 "$MANIFEST_SHA256" >/dev/null
if [ "$QEMU_PROFILE" = 1 ]; then
    command -v openssl >/dev/null 2>&1 || {
        printf '%s\n' 'error: openssl is required for the QEMU development profile' >&2
        exit 1
    }
    command -v python3 >/dev/null 2>&1 || {
        printf '%s\n' 'error: python3 is required for QEMU fixture generation' >&2
        exit 1
    }
    [ -f "$QEMU_ENTROPY_HELPER" ] && [ ! -L "$QEMU_ENTROPY_HELPER" ] &&
        [ -x "$QEMU_ENTROPY_HELPER" ] || {
        printf '%s\n' 'error: LS200_ZOOM_QEMU_ENTROPY_HELPER must name a reviewed ARM executable' >&2
        exit 1
    }
    QEMU_ENTROPY_HELPER=$(python3 "$PATH_GUARD" --input "$REPOSITORY" "$QEMU_ENTROPY_HELPER") || exit 2
    profile=$BOOTSTRAP/qemu-profile
    mkdir -p "$profile/media" "$profile/tls"
    cp "$QEMU_TARGET/qemu-profile.sh" "$profile/start.sh"
    cp "$QEMU_ENTROPY_HELPER" "$profile/entropy-seed"
    chmod 0555 "$profile/start.sh" "$profile/entropy-seed"
    python3 "$REPOSITORY/product/sipd/tests/fixtures/generate_private_lab_fixtures.py" \
        --output "$profile/media"
    openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 7 \
        -subj '/CN=console.invalid' \
        -addext 'subjectAltName=DNS:console.invalid' \
        -keyout "$profile/tls/server.key" -out "$profile/tls/server.crt" \
        >/dev/null 2>&1
    openssl rand -out "$profile/entropy.bin" 64
    chmod 0400 "$profile/tls/server.key"
    chmod 0400 "$profile/entropy.bin"
    chmod 0444 "$profile/tls/server.crt" "$profile/media/"*
    printf '%s\n' 'ls200-zoom-qemu-profile-v1' > "$profile/format"
    chmod 0444 "$profile/format"
fi
mv "$STAGING" "$OUTPUT"
trap - EXIT HUP INT TERM
printf '%s\n' "$OUTPUT"
