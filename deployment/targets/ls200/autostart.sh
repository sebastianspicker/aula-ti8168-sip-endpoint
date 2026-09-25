#!/bin/sh
# Root-cron wrapper for one quiet, reversible physical-device autostart attempt.
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

BASE=/var/lib/cbox/ls200-zoom
CURRENT=$BASE/current
STARTER=$BASE/live-start.sh
MEDIA_GATE_RECORD=$BASE/media-ready-gate
VENDOR_MEDIA_WAIT=/usr/share/media/wait_media_ready
LOCK=/var/run/ls200-zoom-operation.lock
SENTINEL=/run/ls200-zoom-autostart.ok
STARTUP_LOG=/run/ls200-zoom-autostart.log
AUTOSTART_ENABLED=$BASE/autostart-enabled
JOURNAL=$BASE/live-owned-files

root_owned_not_writable() {
    [ -e "$1" ] && [ ! -L "$1" ] || return 1
    [ "$(ls -nd "$1" | awk 'NR == 1 { print $3 }')" = 0 ] || return 1
    find "$1" -prune \( -perm -020 -o -perm -002 \) | grep -q . && return 1
    return 0
}
safe_executable() { [ -f "$1" ] && [ -x "$1" ] && root_owned_not_writable "$1"; }
validate_current() {
    [ -L "$CURRENT" ] || return 1
    target=$(readlink "$CURRENT") || return 1
    case $target in releases/*) version=${target#releases/} ;; *) return 1 ;; esac
    case $version in ''|.|..|*[!A-Za-z0-9._-]*) return 1 ;; esac
    [ -d "$BASE/releases/$version" ] && [ ! -L "$BASE/releases/$version" ]
}
validated_preflight() {
    [ -f "$MEDIA_GATE_RECORD" ] && [ ! -L "$MEDIA_GATE_RECORD" ] && root_owned_not_writable "$MEDIA_GATE_RECORD" || return 1
    [ "$(cat "$MEDIA_GATE_RECORD")" = "$VENDOR_MEDIA_WAIT" ] || return 1
    safe_executable "$VENDOR_MEDIA_WAIT"
}
record_healthy_sentinel() {
    sentinel_parent=$(dirname "$SENTINEL")
    [ -d "$sentinel_parent" ] && root_owned_not_writable "$sentinel_parent" || return 1
    (set -C; : > "$SENTINEL") 2>/dev/null || return 1
    chown 0:0 "$SENTINEL" && chmod 0600 "$SENTINEL" || {
        rm -f "$SENTINEL"
        return 1
    }
}
prepare_startup_log() {
    [ -d /run ] && root_owned_not_writable /run || return 1
    if [ -e "$STARTUP_LOG" ] || [ -L "$STARTUP_LOG" ]; then
        [ -f "$STARTUP_LOG" ] && root_owned_not_writable "$STARTUP_LOG" || return 1
    fi
    : > "$STARTUP_LOG" || return 1
    chmod 0600 "$STARTUP_LOG"
}

[ "$(id -u)" = 0 ] || exit 0
[ -f "$AUTOSTART_ENABLED" ] && [ ! -L "$AUTOSTART_ENABLED" ] && root_owned_not_writable "$AUTOSTART_ENABLED" && [ "$(cat "$AUTOSTART_ENABLED")" = enabled-v1 ] || exit 0
[ -f "$JOURNAL" ] && [ ! -L "$JOURNAL" ] && root_owned_not_writable "$JOURNAL" && grep -Fxc "file:$AUTOSTART_ENABLED" "$JOURNAL" | grep -qx 1 || exit 0
[ ! -e "$SENTINEL" ] && [ ! -L "$SENTINEL" ] || exit 0
validate_current && safe_executable "$STARTER" && validated_preflight || exit 0
[ ! -e "$SENTINEL" ] && [ ! -L "$SENTINEL" ] || exit 0
validate_current && safe_executable "$STARTER" && validated_preflight || exit 0
if "$STARTER" health >/dev/null 2>&1; then
    record_healthy_sentinel || exit 0
    exit 0
fi
prepare_startup_log || exit 0
"$STARTER" start >"$STARTUP_LOG" 2>&1 || exit 0
"$STARTER" health >/dev/null 2>&1 || exit 0
record_healthy_sentinel || exit 0
