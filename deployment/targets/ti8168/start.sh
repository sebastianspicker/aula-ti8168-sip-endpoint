#!/bin/sh
# Start only the active profile after an evidenced media-ready gate succeeds.
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
# Physical profile uses the firmware decoder for bounded headless receive work.
AULA_SIPD_RECEIVE_MONITOR=/usr/bin/ffmpeg
export AULA_SIPD_RECEIVE_MONITOR

BASE=/var/lib/cbox/aula-ti8168-sip-endpoint
RUNTIME_STATE=/run/aula-state
CURRENT=$BASE/current
JOURNAL=$BASE/live-owned-files
VENDOR_MEDIA_WAIT=/usr/share/media/wait_media_ready
VENDOR_MEDIA_LIBRARY_PATH=/usr/lib/cbox
VENDOR_MEDIA_LIBRARY=$VENDOR_MEDIA_LIBRARY_PATH/libcbox_media.so.0
MEDIA_GATE_RECORD=$BASE/media-ready-gate
RUNTIME_GATE_MARKER=/run/aula-ti8168-sip-endpoint-media-ready
IDENTITY=$BASE/live-identities
AUTOSTART_ENABLED=$BASE/autostart-enabled
AUTOSTART_TX=$BASE/autostart-transaction
ROLLBACK_TX=$BASE/rollback-transaction
LOCK=/var/run/aula-ti8168-sip-endpoint-operation.lock
INTERNAL_LOCK=aula-ti8168-sip-endpoint-live-internal-v1
bounded_pid=
rollback_candidate=
owns_lock=0

die() { printf '%s\n' "live-start: $*" >&2; exit 1; }
usage() { printf '%s\n' "usage: $0 {start|stop|health|enable-autostart|disable-autostart} [--rollback-to VERSION]" >&2; exit 64; }
durability_barrier() { sync || die 'filesystem durability barrier failed'; }
clear_transaction() { rm -f "$1"; durability_barrier; }
mounted_from() {
    [ -e "$1" ] && [ -e "$2" ] && [ "$1" -ef "$2" ] || return 1
    awk -v target="$2" '$5 == target { count++ } END { exit count != 1 }' /proc/self/mountinfo
}
root_owned_not_writable() {
    [ -e "$1" ] && [ ! -L "$1" ] || return 1
    [ "$(ls -nd "$1" | awk 'NR == 1 { print $3 }')" = 0 ] || return 1
    find "$1" -prune \( -perm -020 -o -perm -002 \) | grep -q . && return 1
    return 0
}
require_safe_executable() { [ -f "$1" ] && [ -x "$1" ] && root_owned_not_writable "$1" || die "media-ready gate is unsafe: $1"; }
require_safe_media_environment() {
    [ -d "$VENDOR_MEDIA_LIBRARY_PATH" ] && root_owned_not_writable "$VENDOR_MEDIA_LIBRARY_PATH" ||
        die 'vendor media library directory is unsafe'
    [ -e "$VENDOR_MEDIA_LIBRARY" ] || die 'vendor media library is absent'
    vendor_media_library_target=$(readlink -f "$VENDOR_MEDIA_LIBRARY") || die 'vendor media library link is unsafe'
    case $vendor_media_library_target in
        "$VENDOR_MEDIA_LIBRARY_PATH"/*) ;;
        *) die 'vendor media library escaped its trusted directory' ;;
    esac
    root_owned_not_writable "$vendor_media_library_target" || die 'vendor media library target is unsafe'
}
terminate_bounded_process() {
    [ -n "$bounded_pid" ] || return 0
    kill "$bounded_pid" 2>/dev/null || true
    wait "$bounded_pid" 2>/dev/null || true
    bounded_pid=
}
run_with_timeout() {
    bounded_command=$1
    LD_LIBRARY_PATH=$VENDOR_MEDIA_LIBRARY_PATH "$bounded_command" >/dev/null 2>&1 &
    bounded_pid=$!
    bounded_elapsed=0
    while kill -0 "$bounded_pid" 2>/dev/null; do
        if [ "$bounded_elapsed" -ge 30 ]; then
            terminate_bounded_process
            die "media-ready gate timed out: $bounded_command"
        fi
        sleep 1 || {
            terminate_bounded_process
            die 'media-ready gate timer failed'
        }
        bounded_elapsed=$((bounded_elapsed + 1))
    done
    if wait "$bounded_pid"; then
        bounded_pid=
    else
        bounded_pid=
        die "media-ready gate did not succeed: $bounded_command"
    fi
}
run_media_ready_gate() {
    [ -f "$MEDIA_GATE_RECORD" ] && [ ! -L "$MEDIA_GATE_RECORD" ] && root_owned_not_writable "$MEDIA_GATE_RECORD" || die 'media-ready gate was not preflighted by the installer'
    [ "$(cat "$MEDIA_GATE_RECORD")" = "$VENDOR_MEDIA_WAIT" ] || die 'media-ready gate record is unsafe'
    require_safe_executable "$VENDOR_MEDIA_WAIT"
    require_safe_executable "$AULA_SIPD_RECEIVE_MONITOR"
    require_safe_media_environment
    run_with_timeout "$VENDOR_MEDIA_WAIT"
    record_media_ready_marker
}

record_media_ready_marker() {
    marker_parent=$(dirname "$RUNTIME_GATE_MARKER")
    [ -d "$marker_parent" ] && root_owned_not_writable "$marker_parent" ||
        die 'media-ready runtime marker parent is unsafe'
    marker_created=0
    if [ -e "$RUNTIME_GATE_MARKER" ] || [ -L "$RUNTIME_GATE_MARKER" ]; then
        [ -f "$RUNTIME_GATE_MARKER" ] && [ ! -L "$RUNTIME_GATE_MARKER" ] &&
            root_owned_not_writable "$RUNTIME_GATE_MARKER" ||
            die 'media-ready runtime marker is unsafe'
        : > "$RUNTIME_GATE_MARKER" || die 'cannot refresh the media-ready runtime marker'
    else
        (set -C; : > "$RUNTIME_GATE_MARKER") 2>/dev/null ||
            die 'cannot exclusively create the media-ready runtime marker'
        marker_created=1
    fi
    if ! chown 0:0 "$RUNTIME_GATE_MARKER" || ! chmod 0600 "$RUNTIME_GATE_MARKER"; then
        [ "$marker_created" = 0 ] || rm -f "$RUNTIME_GATE_MARKER"
        die 'cannot record a successful media-ready gate'
    fi
}

action=${1:-start}; shift || true
rollback= internal_lock=0 activation_attempt=0
while [ "$#" -gt 0 ]; do
    case $1 in
        --rollback-to) [ "$#" -ge 2 ] || usage; rollback=$2; shift 2 ;;
        --internal-lock) [ "${2:-}" = "$INTERNAL_LOCK" ] || usage; internal_lock=1; shift 2 ;;
        --activation-attempt) activation_attempt=1; shift ;;
        *) usage ;;
    esac
done
case $action in start|stop|health|enable-autostart|disable-autostart) ;; *) usage ;; esac
case $rollback in '') ;; .|..|*[!A-Za-z0-9._-]*) die 'rollback version is unsafe' ;; esac
[ "$activation_attempt" = 0 ] || [ "$internal_lock" = 1 ] || usage
[ -z "${AULA_MEDIA_READY_GATE+x}" ] || die 'AULA_MEDIA_READY_GATE is not accepted by the live profile'

acquire_lock() {
    if mkdir "$LOCK" 2>/dev/null; then printf '%s\n' "$$" > "$LOCK/pid"; return; fi
    [ -f "$LOCK/pid" ] && [ ! -L "$LOCK/pid" ] || die 'another live operation is active'
    pid=$(cat "$LOCK/pid"); case $pid in ''|*[!0-9]*) die 'live operation lock is unsafe' ;; esac
    kill -0 "$pid" 2>/dev/null && die 'another live operation is active'
    rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || die 'stale live operation lock cannot be recovered'
    mkdir "$LOCK" || die 'cannot acquire live operation lock'; printf '%s\n' "$$" > "$LOCK/pid"
}
release_lock() { rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true; }
cleanup_start() {
    trap - 0 1 2 15
    terminate_bounded_process
    [ -z "$rollback_candidate" ] || rm -f "$rollback_candidate"
    [ "$owns_lock" = 0 ] || release_lock
}
validate_internal_lock() {
    [ -d "$LOCK" ] && root_owned_not_writable "$LOCK" && [ -f "$LOCK/pid" ] && ! [ -L "$LOCK/pid" ] && root_owned_not_writable "$LOCK/pid" || die 'internal lock is unsafe'
    owner=$(cat "$LOCK/pid"); case $owner in ''|*[!0-9]*) die 'internal lock owner is unsafe' ;; esac
    kill -0 "$owner" 2>/dev/null || die 'internal lock owner is not live'
    ancestor=$$; steps=0
    while [ "$steps" -lt 8 ]; do
        [ "$ancestor" = "$owner" ] && return
        ancestor=$(awk '/^PPid:/{print $2; exit}' "/proc/$ancestor/status") || break
        steps=$((steps + 1))
    done
    die 'internal lock owner is not an ancestor'
}
if [ "$internal_lock" = 1 ]; then
    validate_internal_lock
else
    acquire_lock
    owns_lock=1
fi
trap cleanup_start 0
trap 'exit 129' 1
trap 'exit 130' 2
trap 'exit 143' 15

require_secure_file() { [ -f "$1" ] && [ ! -L "$1" ] && root_owned_not_writable "$1" || die "persistent file is unsafe: $1"; }
require_release() {
    candidate_version=$1
    case $candidate_version in ''|.|..|*[!A-Za-z0-9._-]*) die 'rollback transaction release is unsafe' ;; esac
    candidate_release=$BASE/releases/$candidate_version
    [ -d "$candidate_release" ] && [ ! -L "$candidate_release" ] || die 'rollback transaction release is absent or unsafe'
    verify_installed_release "$candidate_version"
}
rollback_selector_version() {
    [ -L "$CURRENT" ] || die 'active release link is unsafe during rollback recovery'
    selector=$(readlink "$CURRENT") || die 'cannot read active release link during rollback recovery'
    case $selector in releases/*) selector=${selector#releases/} ;; *) die 'active release link is unsafe during rollback recovery' ;; esac
    case $selector in ''|.|..|*[!A-Za-z0-9._-]*) die 'active release link is unsafe during rollback recovery' ;; esac
    printf '%s\n' "$selector"
}
read_rollback_transaction() {
    require_secure_file "$ROLLBACK_TX"
    rollback_values=$(read_rollback_values) || die 'rollback transaction is unsafe'
    set -- $rollback_values
    [ "$#" -eq 3 ] || die 'rollback transaction is unsafe'
    rollback_old=$1 rollback_new=$2 rollback_phase=$3
    [ "$rollback_old" != "$rollback_new" ] || die 'rollback transaction selectors are unsafe'
    require_release "$rollback_old"
    require_release "$rollback_new"
}
clear_rollback_transaction() { rm -f "$ROLLBACK_TX"; durability_barrier; }
reconcile_rollback_transaction() {
    [ -e "$ROLLBACK_TX" ] || [ -L "$ROLLBACK_TX" ] || return 0
    read_rollback_transaction
    current_rollback_version=$(rollback_selector_version)
    case $rollback_phase:$current_rollback_version in
        prepared:$rollback_old) clear_rollback_transaction ;;
        prepared:*) die 'prepared rollback transaction changed the active selector' ;;
        applied:$rollback_old)
            # `applied` is durable before the swap. A reset or rollback
            # recovery may therefore legitimately leave the old selector.
            clear_rollback_transaction
            ;;
        applied:$rollback_new)
            # A validated internal child belongs to the rollback operation
            # that wrote this transaction, so leave its selected release and
            # transaction for that owner to health-check and commit.
            if [ "$internal_lock" = 1 ]; then
                return 0
            fi
            "$CURRENT/etc/init.d/S99aula" stop >/dev/null 2>&1 || true
            replacement=$BASE/.rollback-start-recover-$rollback_old-$$
            ln -s "releases/$rollback_old" "$replacement" || die 'cannot prepare rollback recovery selector'
            rollback_candidate=$replacement
            "$BASE/releases/$rollback_old/bin/aula-atomic-replace" "$replacement" "$CURRENT" || die 'cannot restore rollback recovery selector'
            rollback_candidate=
            durability_barrier
            clear_rollback_transaction
            ;;
        applied:*) die 'applied rollback transaction has an unexpected active selector' ;;
        healthy:$rollback_new) clear_rollback_transaction ;;
        healthy:*) die 'healthy rollback transaction has an unexpected active selector' ;;
        *) die 'rollback transaction is unsafe' ;;
    esac
}
record() {
    if ! grep -Fx "$1" "$JOURNAL" >/dev/null 2>&1; then printf '%s\n' "$1" >> "$JOURNAL"; durability_barrier; fi
}

reconcile_autostart_transaction() {
    if [ -e "$AUTOSTART_TX" ] || [ -L "$AUTOSTART_TX" ]; then
        require_secure_file "$AUTOSTART_TX"; phase=$(cat "$AUTOSTART_TX")
        reconcile_pending_autostart require_secure_autostart_marker
        return
    fi
    if [ -e "$AUTOSTART_ENABLED" ] || [ -L "$AUTOSTART_ENABLED" ]; then
        require_secure_autostart_marker
        grep -Fxc "file:$AUTOSTART_ENABLED" "$JOURNAL" | grep -qx 1 || die 'unjournaled autostart marker is rejected'
    fi
}
write_autostart_marker() {
    temporary=$(mktemp "$BASE/.autostart-enabled.XXXXXX") || die 'cannot create autostart marker temporary'
    printf '%s\n' enabled-v1 > "$temporary"; chown 0:0 "$temporary"; chmod 0600 "$temporary"
    mv "$temporary" "$AUTOSTART_ENABLED" || die 'cannot atomically enable autostart'
    durability_barrier
}
disable_autostart() {
    require_secure_autostart_marker
    [ "$(grep -Fxc "file:$AUTOSTART_ENABLED" "$JOURNAL")" -eq 1 ] || die 'autostart marker is not uniquely journaled'
    temporary=$(mktemp "$BASE/.live-owned-files.XXXXXX") || die 'cannot create journal temporary'
    while IFS= read -r entry; do [ "$entry" = "file:$AUTOSTART_ENABLED" ] || printf '%s\n' "$entry" >> "$temporary"; done < "$JOURNAL"
    chown 0:0 "$temporary"; chmod 0600 "$temporary"
    rm -f "$AUTOSTART_ENABLED"
    mv "$temporary" "$JOURNAL"
    durability_barrier
}

[ "$(id -u)" = 0 ] || die 'must run as root on the target'
[ -f "$JOURNAL" ] && [ ! -L "$JOURNAL" ] || die 'ownership journal is absent or unsafe'
require_secure_file "$JOURNAL"
require_secure_file "$BASE/live-transaction-records.sh"
for helper_parent in /var /var/lib /var/lib/cbox "$BASE"; do
    root_owned_not_writable "$helper_parent" || die 'transaction helper parent is unsafe'
done
# shellcheck source=transaction-records.sh
. "$BASE/live-transaction-records.sh"
reconcile_rollback_transaction
reconcile_autostart_transaction
[ -f "$IDENTITY/group" ] && [ ! -L "$IDENTITY/group" ] || die 'managed group overlay is absent or unsafe'
if ! mounted_from "$IDENTITY/group" /etc/group; then
    awk '$5 == "/etc/group" {count++} END {exit count != 0}' /proc/self/mountinfo || die 'group bind mount is not owned by this profile'
    mount -o bind "$IDENTITY/group" /etc/group || die 'cannot restore managed group bind mount'
fi
id aula-web >/dev/null && id aula-gateway >/dev/null && id aula-sip >/dev/null || die 'managed service identities do not resolve'
[ -L "$CURRENT" ] || die 'no active release'
current_target=$(readlink "$CURRENT") || die 'cannot read active release link'
case $current_target in releases/*) current_version=${current_target#releases/} ;; *) die 'active release link is unsafe' ;; esac
case $current_version in ''|.|..|*[!A-Za-z0-9._-]*) die 'active release link is unsafe' ;; esac
ACTIVE_RELEASE=$BASE/releases/$current_version
RUNTIME_ACTIVE_RELEASE=$RUNTIME_STATE/releases/$current_version
[ -d "$ACTIVE_RELEASE" ] && [ ! -L "$ACTIVE_RELEASE" ] || die 'active release target is unsafe'
[ -x "$CURRENT/etc/init.d/S99aula" ] || die 'active release is incomplete'
verify_installed_release "$current_version"
[ -z "$rollback" ] || require_release "$rollback"

rollback_activation() {
    if [ -z "$rollback" ]; then
        target=$(readlink "$CURRENT") || return 1
        case $target in releases/*) target_version=${target#releases/} ;; *) return 1 ;; esac
        case $target_version in ''|.|..|*[!A-Za-z0-9._-]*) return 1 ;; esac
        rm -f "$CURRENT" || return 1
        durability_barrier
        printf '%s\n' 'live-start: deactivated failed first release' >&2
        return
    fi
    [ -d "$BASE/releases/$rollback" ] && [ ! -L "$BASE/releases/$rollback" ] || return 1
    rollback_candidate=$BASE/.rollback-$rollback-$$
    ln -s "releases/$rollback" "$rollback_candidate" || return 1
    if ! "$BASE/releases/$rollback/bin/aula-atomic-replace" "$rollback_candidate" "$CURRENT"; then
        rm -f "$rollback_candidate"
        rollback_candidate=
        return 1
    fi
    rollback_candidate=
    durability_barrier
    printf '%s\n' "live-start: rolled back to $rollback" >&2
}

case $action in
    disable-autostart)
        disable_autostart
        printf '%s\n' 'live-start: autostart disabled'
        exit 0
        ;;
    stop)
        "$CURRENT/etc/init.d/S99aula" stop
        exit 0
        ;;
    health)
        for service in device sip gateway web; do
            case $service in device) identity=root; executable=$RUNTIME_ACTIVE_RELEASE/bin/aula-device-control ;; sip) identity=aula-sip; executable=$RUNTIME_ACTIVE_RELEASE/bin/aula-sipd ;; gateway) identity=aula-gateway; executable=$RUNTIME_ACTIVE_RELEASE/bin/aula-gateway-fcgi ;; web) identity=aula-web; executable=$RUNTIME_ACTIVE_RELEASE/bin/nginx ;; esac
            pid=/var/run/aula-ti8168-sip-endpoint/$service.pid
            [ -f "$pid" ] && [ ! -L "$pid" ] || die "missing PID file: $pid"
            value=$(cat "$pid")
            case $value in ''|*[!0-9]*) die "invalid PID file: $pid" ;; esac
            kill -0 "$value" 2>/dev/null || die "service is not alive: $pid"
            [ "$(readlink "/proc/$value/exe")" = "$executable" ] || die "PID executable changed: $pid"
            expected_uid=$(id -u "$identity") || die "cannot resolve service identity: $identity"
            actual_uid=$(awk '/^Uid:/{print $2; exit}' "/proc/$value/status")
            [ "$actual_uid" = "$expected_uid" ] || die "PID identity changed: $pid"
        done
        [ -S /run/aula-device/control.sock ] || die 'device companion socket is absent'
        [ -S /run/aula-sipd/control.sock ] || die 'SIP control socket is absent'
        [ -S /run/aula-console/gateway.fcgi.sock ] || die 'gateway socket is absent'
        [ -x /usr/bin/curl ] || die 'recovered target curl helper is absent'
        status=$(/usr/bin/curl -m 5 -k -sS -o /dev/null -w '%{http_code}' \
            -H 'Host: localhost:8443' -H 'Sec-Fetch-Site: same-origin' \
            https://127.0.0.1:8443/zoom/api/v1/auth/session) || die 'local HTTPS health probe failed'
        [ "$status" = 401 ] || die "unexpected unauthenticated /auth/session status: $status"
        printf '%s\n' 'live-start: service health passed'
        ;;
    start|enable-autostart)
        run_media_ready_gate
        if ! AULA_VENDOR_MEDIA_READY_MARKER=$RUNTIME_GATE_MARKER "$CURRENT/etc/init.d/S99aula" start || ! "$0" health --internal-lock "$INTERNAL_LOCK"; then
            "$CURRENT/etc/init.d/S99aula" stop >/dev/null 2>&1 || true
            rm -f "$RUNTIME_GATE_MARKER"
            if [ "$activation_attempt" = 1 ]; then
                rollback_activation || die 'service health failed and automatic rollback failed'
                if [ -z "$rollback" ]; then
                    die 'service health failed; failed first release was deactivated and services remain stopped'
                fi
                die 'service health failed; previous release selector was restored and services remain stopped'
            fi
            die 'service health failed; active release was preserved for retry'
        fi
        grep -Fx "mount:/run/aula-state:$BASE" "$JOURNAL" >/dev/null 2>&1 ||
            printf '%s\n' "mount:/run/aula-state:$BASE" >> "$JOURNAL"
        if [ "$action" = enable-autostart ]; then
            if [ -e "$AUTOSTART_ENABLED" ] || [ -L "$AUTOSTART_ENABLED" ]; then
                require_secure_autostart_marker
                grep -Fxc "file:$AUTOSTART_ENABLED" "$JOURNAL" | grep -qx 1 || die 'unjournaled autostart marker is rejected'
            else
                write_autostart_tx enable-v1:prepared
                write_autostart_marker
                write_autostart_tx enable-v1:applied
                record "file:$AUTOSTART_ENABLED"
                clear_transaction "$AUTOSTART_TX"
            fi
        fi
        ;;
esac
