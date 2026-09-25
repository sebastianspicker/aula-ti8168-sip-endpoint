#!/bin/sh
# Install a manifest-verified release on a physical LS-200 without writing UBIFS.
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

BASE=/var/lib/cbox/ls200-zoom
RELEASES=$BASE/releases
IDENTITY=$BASE/live-identities
JOURNAL=$BASE/live-owned-files
CURRENT=$BASE/current
FIXED_PREFIX=/run/ls200-zoom-state/current
ROOT_CRON=/var/lib/cbox/crontabs/root
CRON_COMMENT='# ls200-zoom live autostart'
CRON_MARKER_PREFIX='# ls200-zoom live autostart'
CRON_JOB="* * * * * $BASE/live-autostart.sh >/dev/null 2>&1"
VENDOR_MEDIA_WAIT=/usr/share/media/wait_media_ready
MEDIA_GATE_RECORD=$BASE/media-ready-gate
AUTOSTART_ENABLED=$BASE/autostart-enabled
CRON_TX=$BASE/cron-transaction
AUTOSTART_TX=$BASE/autostart-transaction
LOCK=/var/run/ls200-zoom-operation.lock
INTERNAL_LOCK=ls200-live-internal-v1
SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)

die() { printf '%s\n' "live-install: $*" >&2; exit 1; }
usage() { printf '%s\n' "usage: $0 --payload ABSOLUTE_PATH --version VERSION --manifest-sha256 SHA256 [--start] [--dry-run]" >&2; exit 64; }
durability_barrier() { sync || die 'filesystem durability barrier failed'; }
clear_transaction() { rm -f "$1"; durability_barrier; }

payload= version= manifest_sha= start=0 dry_run=0
while [ "$#" -gt 0 ]; do
    case $1 in
        --payload) [ "$#" -ge 2 ] || usage; payload=$2; shift 2 ;;
        --version) [ "$#" -ge 2 ] || usage; version=$2; shift 2 ;;
        --manifest-sha256) [ "$#" -ge 2 ] || usage; manifest_sha=$2; shift 2 ;;
        --start) start=1; shift ;;
        --dry-run) dry_run=1; shift ;;
        *) usage ;;
    esac
done
case $payload in /*) ;; *) die 'payload must be an absolute path' ;; esac
case $version in ''|.|..|*[!A-Za-z0-9._-]*) die 'version must contain only A-Z, a-z, 0-9, dot, underscore, or dash, and must not be dot or dot-dot' ;; esac
case $manifest_sha in ''|*[!0-9a-f]*) die 'expected digest must be lowercase SHA-256' ;; esac
[ "${#manifest_sha}" = 64 ] || die 'expected digest must be SHA-256'
case $payload in /dev/mtd*|*/evidence/firmware-analysis/*|*/archive/*|*/recovered/*|*/dump/*) die 'payload path is protected evidence' ;; esac

if [ -d "$payload/runtime" ]; then payload=$payload/runtime; fi
[ -d "$payload" ] && [ ! -L "$payload" ] || die "payload is not a regular directory: $payload"
[ -f "$SCRIPT_DIR/verify-payload.sh" ] && [ ! -L "$SCRIPT_DIR/verify-payload.sh" ] || die 'trusted verifier is absent or unsafe'

if [ "$dry_run" = 1 ]; then
    sh "$SCRIPT_DIR/verify-payload.sh" --payload "$payload" --manifest-sha256 "$manifest_sha" >/dev/null || die 'trusted dry-run payload verification failed'
    printf '%s\n' "live-install dry-run: payload=$payload version=$version destination=$RELEASES/$version"
    printf '%s\n' 'live-install dry-run: writes are limited to /var/lib/cbox/ls200-zoom; /etc/passwd and /etc/group are bind mounts only'
    exit 0
fi

[ "$(id -u)" = 0 ] || die 'must run as root on the target'
[ -f /etc/passwd ] && [ ! -L /etc/passwd ] || die '/etc/passwd is not a regular file view'
[ -f /etc/group ] && [ ! -L /etc/group ] || die '/etc/group is not a regular file view'

root_owned_not_writable() {
    [ -e "$1" ] && [ ! -L "$1" ] || return 1
    [ "$(ls -nd "$1" | awk 'NR == 1 { print $3 }')" = 0 ] || return 1
    find "$1" -prune \( -perm -020 -o -perm -002 \) | grep -q . && return 1
    return 0
}
require_secure_dir() { [ -d "$1" ] && root_owned_not_writable "$1" || die "persistent directory is unsafe: $1"; }
create_secure_dir() {
    path=$1; mode=$2
    if [ -e "$path" ] || [ -L "$path" ]; then require_secure_dir "$path"; return; fi
    mkdir "$path" || die "cannot create persistent directory: $path"
    chown 0:0 "$path"; chmod "$mode" "$path"; require_secure_dir "$path"
}
require_secure_file() { [ -f "$1" ] && root_owned_not_writable "$1" || die "persistent file is unsafe: $1"; }
require_secure_path_chain() {
    path=$1
    while [ "$path" != / ]; do root_owned_not_writable "$path" || die "unsafe root-owned path chain: $path"; path=$(dirname "$path"); done
}
require_root_crontab() {
    [ -f "$ROOT_CRON" ] && [ ! -L "$ROOT_CRON" ] && root_owned_not_writable "$ROOT_CRON" ||
        die 'root crontab is absent or unsafe'
    [ ! -s "$ROOT_CRON" ] || [ "$(tail -c 1 "$ROOT_CRON" | od -An -tu1 | tr -d ' ')" = 10 ] ||
        die 'root crontab is not safely line-terminated'
}
require_vendor_media_wait() {
    [ -f "$VENDOR_MEDIA_WAIT" ] && [ ! -L "$VENDOR_MEDIA_WAIT" ] && [ -x "$VENDOR_MEDIA_WAIT" ] &&
        root_owned_not_writable "$VENDOR_MEDIA_WAIT" || die 'vendor media-ready helper is absent or unsafe'
}
cron_marker_count() { grep -F "$CRON_MARKER_PREFIX" "$ROOT_CRON" | wc -l | tr -d ' '; }
cron_line_count() { grep -Fxc "$1" "$ROOT_CRON" || true; }
cron_state() {
    markers=$(cron_marker_count); comments=$(cron_line_count "$CRON_COMMENT"); jobs=$(cron_line_count "$CRON_JOB")
    case $markers:$comments:$jobs in
        0:0:0) printf '%s\n' absent ;;
        1:1:1) printf '%s\n' present ;;
        *) die 'root crontab contains an ambiguous or conflicting ls200-zoom marker' ;;
    esac
}
write_cron_tx() {
    cron_transaction_temporary=$(mktemp "$BASE/.cron-transaction.XXXXXX") || die 'cannot create cron transaction temporary'
    printf '%s\n' "$1" > "$cron_transaction_temporary"
    chown 0:0 "$cron_transaction_temporary"; chmod 0600 "$cron_transaction_temporary"
    mv "$cron_transaction_temporary" "$CRON_TX" || die 'cannot atomically record cron transaction'
    durability_barrier
}
remove_cron_journal_entries() {
    temporary=$(mktemp "$BASE/.live-owned-files.XXXXXX") || die 'cannot create journal temporary'
    while IFS= read -r entry; do
        [ "$entry" = "cron-comment:$CRON_COMMENT" ] || [ "$entry" = "cron-job:$CRON_JOB" ] || printf '%s\n' "$entry" >> "$temporary"
    done < "$JOURNAL"
    chown 0:0 "$temporary"; chmod 0600 "$temporary"
    mv "$temporary" "$JOURNAL" || die 'cannot atomically reconcile cron ownership journal'
    durability_barrier
}
recover_cron_transaction() {
    [ -e "$CRON_TX" ] || [ -L "$CRON_TX" ] || return 0
    require_secure_file "$CRON_TX"; phase=$(cat "$CRON_TX")
    case $phase in cron-v1:prepared|cron-v1:applied|cron-v1:remove-prepared|cron-v1:remove-applied) ;; *) die 'cron transaction is unsafe' ;; esac
    case $(cron_state) in
        absent)
            case $phase in
                cron-v1:prepared) clear_transaction "$CRON_TX" ;;
                cron-v1:remove-prepared|cron-v1:remove-applied) remove_cron_journal_entries; clear_transaction "$CRON_TX" ;;
                *) die 'cron transaction lost its applied rows' ;;
            esac
            ;;
        present)
            case $phase in
                cron-v1:prepared|cron-v1:applied) record "cron-comment:$CRON_COMMENT"; record "cron-job:$CRON_JOB"; clear_transaction "$CRON_TX" ;;
                cron-v1:remove-prepared) clear_transaction "$CRON_TX" ;;
                *) die 'cron removal transaction retained owned rows' ;;
            esac
            ;;
    esac
}

reconcile_autostart_transaction() {
    if [ -e "$AUTOSTART_TX" ] || [ -L "$AUTOSTART_TX" ]; then
        require_secure_file "$AUTOSTART_TX"; phase=$(cat "$AUTOSTART_TX")
        reconcile_pending_autostart require_secure_autostart_marker
        return
    fi
    if [ -e "$AUTOSTART_ENABLED" ] || [ -L "$AUTOSTART_ENABLED" ]; then
        require_secure_autostart_marker
        if ! grep -Fxc "file:$AUTOSTART_ENABLED" "$JOURNAL" | grep -qx 1; then
            rm -f "$AUTOSTART_ENABLED"
            durability_barrier
        fi
    fi
}
install_root_cron() {
    require_secure_dir /var/lib/cbox/crontabs
    require_root_crontab
    recover_cron_transaction
    case $(cron_state) in
        present)
            grep -Fxc "cron-comment:$CRON_COMMENT" "$JOURNAL" | grep -qx 1 &&
                grep -Fxc "cron-job:$CRON_JOB" "$JOURNAL" | grep -qx 1 ||
                die 'preexisting ls200-zoom cron lines are not recorded as owned'
            return
            ;;
        absent) ;;
        *) die 'cannot determine root crontab state' ;;
    esac
    grep -Fq 'cron-comment:' "$JOURNAL" && die 'journal contains stale ls200-zoom cron ownership'
    cron_install_original=$(mktemp /var/lib/cbox/crontabs/.root.ls200-zoom-original.XXXXXX) || die 'cannot snapshot root crontab'
    cron_install_temporary=$(mktemp /var/lib/cbox/crontabs/.root.ls200-zoom.XXXXXX) || { rm -f "$cron_install_original"; die 'cannot create root crontab temporary'; }
    cp -p "$ROOT_CRON" "$cron_install_original" || { rm -f "$cron_install_original" "$cron_install_temporary"; die 'cannot snapshot root crontab'; }
    cp -p "$cron_install_original" "$cron_install_temporary" || { rm -f "$cron_install_original" "$cron_install_temporary"; die 'cannot stage root crontab update'; }
    printf '%s\n%s\n' "$CRON_COMMENT" "$CRON_JOB" >> "$cron_install_temporary"
    chown 0:0 "$cron_install_temporary"; chmod 0600 "$cron_install_temporary"
    write_cron_tx cron-v1:prepared
    cmp -s "$cron_install_original" "$ROOT_CRON" || { rm -f "$cron_install_original" "$cron_install_temporary"; die 'root crontab changed concurrently'; }
    mv "$cron_install_temporary" "$ROOT_CRON" || die 'cannot atomically install ls200-zoom cron lines'
    rm -f "$cron_install_original"
    durability_barrier
    write_cron_tx cron-v1:applied
    record "cron-comment:$CRON_COMMENT"
    record "cron-job:$CRON_JOB"
    durability_barrier
    clear_transaction "$CRON_TX"
}

require_secure_dir /var/lib/cbox
create_secure_dir "$BASE" 0755
create_secure_dir "$RELEASES" 0755
create_secure_dir "$IDENTITY" 0700
require_secure_dir /var/lib/cbox/crontabs
require_root_crontab
require_vendor_media_wait
require_secure_path_chain "$SCRIPT_DIR"
require_secure_path_chain "$SCRIPT_DIR/verify-payload.sh"
require_secure_path_chain "$SCRIPT_DIR/transaction-records.sh"
require_secure_file "$SCRIPT_DIR/transaction-records.sh"
# shellcheck source=transaction-records.sh
. "$SCRIPT_DIR/transaction-records.sh"
require_secure_path_chain "$payload"
sh "$SCRIPT_DIR/verify-payload.sh" --payload "$payload" --manifest-sha256 "$manifest_sha" >/dev/null || die 'trusted payload verification failed after path preflight'

payload_kb=$(du -sk "$payload" | awk 'NR == 1 { print $1 }')
free_kb=$(df -k "$BASE" | awk 'NR == 2 { print $4 }')
case $payload_kb:$free_kb in *[!0-9:]*|:*) die 'cannot determine persistent free space' ;; esac
[ "$free_kb" -ge $((payload_kb + 2048)) ] || die 'insufficient persistent free space for payload plus rollback reserve'

acquire_lock() {
    if mkdir "$LOCK" 2>/dev/null; then printf '%s\n' "$$" > "$LOCK/pid"; return; fi
    [ -f "$LOCK/pid" ] && [ ! -L "$LOCK/pid" ] || die 'another live operation is active'
    pid=$(cat "$LOCK/pid"); case $pid in ''|*[!0-9]*) die 'live operation lock is unsafe' ;; esac
    kill -0 "$pid" 2>/dev/null && die 'another live operation is active'
    rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || die 'stale live operation lock cannot be recovered'
    mkdir "$LOCK" || die 'cannot acquire live operation lock'; printf '%s\n' "$$" > "$LOCK/pid"
}
release_lock() { rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true; }
live_helper_temporary=
cleanup_install() {
    [ -z "$live_helper_temporary" ] || rm -f "$live_helper_temporary" || true
    release_lock
}
require_inert_install() {
    for pidfile in /var/run/ls200-zoom/device.pid /var/run/ls200-zoom/sip.pid /var/run/ls200-zoom/gateway.pid /var/run/ls200-zoom/web.pid; do
        [ ! -e "$pidfile" ] && [ ! -L "$pidfile" ] || die "managed service pidfile blocks inert install: $pidfile"
    done
}
acquire_lock
trap cleanup_install EXIT HUP INT TERM
require_inert_install
if [ -e "$JOURNAL" ] || [ -L "$JOURNAL" ]; then
    require_secure_file "$JOURNAL"
else
    touch "$JOURNAL"; chown 0:0 "$JOURNAL"; chmod 0600 "$JOURNAL"; require_secure_file "$JOURNAL"
fi

record() {
    if ! grep -Fx "$1" "$JOURNAL" >/dev/null 2>&1; then printf '%s\n' "$1" >> "$JOURNAL"; durability_barrier; fi
}
BOOTSTRAP_HELPER=$SCRIPT_DIR/bootstrap-transaction.sh
require_secure_path_chain "$BOOTSTRAP_HELPER"
require_secure_file "$BOOTSTRAP_HELPER"
BOOTSTRAP_ROOT=$BASE
BOOTSTRAP_RELEASES=$RELEASES
BOOTSTRAP_JOURNAL=$JOURNAL
BOOTSTRAP_ROOT_UID=0
BOOTSTRAP_ROOT_GID=0
BOOTSTRAP_GATEWAY_OWNER=ls200-gateway
bootstrap_release_identity() {
    [ "$(release_manifest_digest "$1")" = "$2" ] || die 'bootstrap transaction release identity changed'
    verify_installed_release "$1"
}
bootstrap_user_id() { user_id "$1"; }
bootstrap_record() { record "$1"; }
bootstrap_journal_count() { grep -Fxc "$1" "$JOURNAL" || true; }
require_bootstrap_journal_ownership() {
    [ "$(bootstrap_journal_count "file:$BASE/gateway.conf")" = 1 ] &&
        [ "$(bootstrap_journal_count "file:$BASE/bootstrap-token")" = 1 ] ||
        die 'gateway configuration ownership journal is incomplete'
}
# shellcheck source=bootstrap-transaction.sh
. "$BOOTSTRAP_HELPER"
publish_live_helper() {
    helper_source=$1
    helper_destination=$2
    helper_mode=$3
    helper_name=$4
    live_helper_temporary=$(mktemp "$BASE/.live-helper.XXXXXX") || die "cannot stage $helper_name"
    cp "$helper_source" "$live_helper_temporary" || die "cannot copy staged $helper_name"
    chown 0:0 "$live_helper_temporary" || die "cannot own staged $helper_name"
    chmod "$helper_mode" "$live_helper_temporary" || die "cannot seal staged $helper_name"
    require_secure_file "$live_helper_temporary"
    find "$live_helper_temporary" -prune -perm "$helper_mode" | grep -Fx "$live_helper_temporary" >/dev/null ||
        die "staged $helper_name mode is unsafe"
    cmp -s "$helper_source" "$live_helper_temporary" || die "staged $helper_name differs from its source"
    durability_barrier
    mv "$live_helper_temporary" "$helper_destination" || die "cannot atomically publish $helper_name"
    live_helper_temporary=
    durability_barrier
    require_secure_file "$helper_destination"
    find "$helper_destination" -prune -perm "$helper_mode" | grep -Fx "$helper_destination" >/dev/null ||
        die "published $helper_name mode is unsafe"
    cmp -s "$helper_source" "$helper_destination" || die "published $helper_name differs from its source"
    record "file:$helper_destination"
}
recover_cron_transaction
reconcile_autostart_transaction
mounted_from() {
    [ -e "$1" ] && [ -e "$2" ] && [ "$1" -ef "$2" ] || return 1
    awk -v target="$2" '$5 == target { count++ } END { exit count != 1 }' /proc/self/mountinfo
}
mount_root_for_target() { awk -v target="$1" '$5 == target {count++; root=$4} END {if (count == 0) print "none"; else if (count == 1) print root; else print "invalid"}' /proc/self/mountinfo; }
ROOT_SSH_PROVISION=/var/lib/cbox/root-ssh-provision
ROOT_SSH_BACKUP=$BASE/root-ssh-passwd.before
ROOT_PASSWD_SOURCE= ROOT_PASSWD_MOUNT= ROOT_SSH_APPLY= ROOT_SSH_USER=

validate_root_ssh_overlay() {
    ROOT_PASSWD_MOUNT=$(mount_root_for_target /etc/passwd)
    case $ROOT_PASSWD_MOUNT in /root-ssh-provision/account-overlays/*/passwd) ;; *) die 'expected managed root-SSH passwd bind mount is absent' ;; esac
    ROOT_SSH_USER=${ROOT_PASSWD_MOUNT#/root-ssh-provision/account-overlays/}
    ROOT_SSH_USER=${ROOT_SSH_USER%/passwd}
    case $ROOT_SSH_USER in ''|*[!a-z0-9_-]*) die 'managed root-SSH account marker is unsafe' ;; esac
    ROOT_PASSWD_SOURCE=$ROOT_SSH_PROVISION/account-overlays/$ROOT_SSH_USER/passwd
    ROOT_SHADOW_SOURCE=$ROOT_SSH_PROVISION/account-overlays/$ROOT_SSH_USER/shadow
    ROOT_SSH_APPLY=$ROOT_SSH_PROVISION/account-overlays/$ROOT_SSH_USER/apply.sh
    ROOT_SSH_MARKER=$ROOT_SSH_PROVISION/account-overlays/$ROOT_SSH_USER/.ls200-root-shell-managed
    ROOT_SSH_CRON=/var/lib/cbox/crontabs/root
    [ -f "$ROOT_PASSWD_SOURCE" ] && [ ! -L "$ROOT_PASSWD_SOURCE" ] && [ -f "$ROOT_SHADOW_SOURCE" ] && [ ! -L "$ROOT_SHADOW_SOURCE" ] && [ -x "$ROOT_SSH_APPLY" ] && [ ! -L "$ROOT_SSH_APPLY" ] && [ -f "$ROOT_SSH_MARKER" ] && [ ! -L "$ROOT_SSH_MARKER" ] && [ -f "$ROOT_SSH_CRON" ] && [ ! -L "$ROOT_SSH_CRON" ] || die 'managed root-SSH overlay files are unsafe'
    require_secure_path_chain "$ROOT_SSH_APPLY"
    [ "$(wc -l < "$ROOT_SSH_MARKER" | tr -d ' ')" = 2 ] && [ "$(grep -Fxc 'managed-by=open-ls200-root-shell.sh' "$ROOT_SSH_MARKER")" -eq 1 ] && [ "$(grep -Fxc "account=$ROOT_SSH_USER" "$ROOT_SSH_MARKER")" -eq 1 ] || die 'managed root-SSH overlay marker does not match mount'
    root_comment="# ls200-root-ssh-$ROOT_SSH_USER managed cron-bind"
    root_job="* * * * * $ROOT_SSH_APPLY >/dev/null 2>&1"
    [ "$(grep -Fxc "$root_comment" "$ROOT_SSH_CRON")" -eq 1 ] && [ "$(grep -Fxc "$root_job" "$ROOT_SSH_CRON")" -eq 1 ] || die 'managed root-SSH cron apply contract does not match mount'
    grep -Fx "passwd_source='$ROOT_PASSWD_SOURCE'" "$ROOT_SSH_APPLY" >/dev/null && grep -Fx "shadow_source='$ROOT_SHADOW_SOURCE'" "$ROOT_SSH_APPLY" >/dev/null && grep -F "/etc/passwd '$ROOT_PASSWD_MOUNT'" "$ROOT_SSH_APPLY" >/dev/null || die 'managed root-SSH apply contract does not match mount'
    [ "$(mount_root_for_target /etc/shadow)" = "${ROOT_PASSWD_MOUNT%/passwd}/shadow" ] || die 'managed root-SSH shadow bind mount does not match passwd overlay'
    cmp -s "$ROOT_PASSWD_SOURCE" /etc/passwd || die 'managed root-SSH passwd source does not match its active bind view'
}

next_unused_id() {
    candidate=200
    while [ "$candidate" -le 65000 ]; do
        if ! awk -F: -v value="$candidate" '$3 == value || $4 == value { found = 1 } END { exit found }' \
            "$ROOT_PASSWD_SOURCE" "$IDENTITY/group"; then
            printf '%s\n' "$candidate"
            return
        fi
        candidate=$((candidate + 1))
    done
    die 'cannot allocate a safe service identity number'
}

append_group() {
    name=$1; members=$2; gid=$(next_unused_id)
    printf '%s:x:%s:%s\n' "$name" "$gid" "$members" >> "$IDENTITY/group"
}
group_id() { awk -F: -v name="$1" '$1 == name { print $3; found = 1 } END { exit !found }' "$IDENTITY/group"; }
append_user() { name=$1; primary=$2; uid=$(next_unused_id); gid=$(group_id "$primary") || die "missing primary group $primary"; printf '%s:x:%s:%s:LS200 Zoom service:/nonexistent:/sbin/nologin\n' "$name" "$uid" "$gid" >> "$ROOT_PASSWD_TEMP"; }
user_id() { awk -F: -v name="$1" '$1 == name { print $3; found = 1 } END { exit !found }' /etc/passwd; }
require_configured_uid() {
    file=$1; key=$2; value=$3
    case $key in
        gateway_uid) pattern="^[[:space:]]*gateway_uid[[:space:]]*=[[:space:]]*${value}[[:space:]]*$" ;;
        expected_sipd_uid) pattern="^[[:space:]]*\"expected_sipd_uid\"[[:space:]]*:[[:space:]]*${value}[[:space:]]*,?[[:space:]]*$" ;;
        *) die "unsupported UID configuration key: $key" ;;
    esac
    grep -Eq "$pattern" "$file" || die "persistent configuration has stale $key; migrate it before activation: $file"
}
require_live_prefix() {
    [ -f "$1" ] && [ ! -L "$1" ] || die "payload artifact is absent or unsafe: $1"
    grep -F /opt/ls200-zoom "$1" >/dev/null && die "payload retains unsupported /opt prefix: $1"
    grep -F "$BASE/current" "$1" >/dev/null && die "payload retains inaccessible persistent runtime prefix: $1"
    return 0
}
require_live_payload_layout() {
    require_live_prefix "$payload/bin/ls200-zoom-service"
    require_live_prefix "$payload/etc/init.d/S99ls200-zoom"
    require_live_prefix "$payload/etc/nginx/nginx.conf"
    grep -F "$FIXED_PREFIX/bin/ls200-sipd" "$payload/bin/ls200-zoom-service" >/dev/null || die 'service dispatcher lacks fixed SIP path'
    grep -F "$FIXED_PREFIX/bin/nginx" "$payload/bin/ls200-zoom-service" >/dev/null || die 'service dispatcher lacks fixed nginx path'
    grep -Fx "PREFIX=$FIXED_PREFIX" "$payload/etc/init.d/S99ls200-zoom" >/dev/null || die 'init script lacks the exact fixed release prefix'
    grep -F "root $FIXED_PREFIX/ui;" "$payload/etc/nginx/nginx.conf" >/dev/null || die 'nginx config lacks the fixed UI prefix'
    grep -F "$FIXED_PREFIX/bin/ls200-gateway-fcgi" "$payload/etc/nginx/nginx.conf" >/dev/null || die 'nginx config lacks the fixed gateway prefix'
}

prepare_group_view() {
    if mounted_from "$IDENTITY/group" /etc/group; then return; fi
    [ "$(mount_root_for_target /etc/group)" = none ] || die 'group bind mount is not owned by this profile'
    if [ -e "$IDENTITY/group" ] || [ -e "$IDENTITY/group.before" ]; then
        [ -f "$IDENTITY/group" ] && [ ! -L "$IDENTITY/group" ] || die 'saved group overlay is unsafe'
        mount -o bind "$IDENTITY/group" /etc/group || die 'cannot restore managed group bind mount'
        return
    fi
    cp /etc/group "$IDENTITY/group.before"
    cp "$IDENTITY/group.before" "$IDENTITY/group"
    cmp -s /etc/group "$IDENTITY/group.before" || die 'visible group changed while preparing managed overlay'
    chmod 0644 "$IDENTITY/group" "$IDENTITY/group.before"
    for name in ls200-web ls200-gateway ls200-sip ls200-control ls200-web-gateway ls200-web-tls ls200-media-video ls200-media-audio; do
        grep -q "^$name:" "$ROOT_PASSWD_SOURCE" "$IDENTITY/group" && die "service identity already exists outside this profile: $name"
    done
    append_group ls200-web ''
    append_group ls200-gateway ''
    append_group ls200-sip ''
    append_group ls200-control 'ls200-gateway,ls200-sip'
    append_group ls200-web-gateway 'ls200-web,ls200-gateway'
    append_group ls200-web-tls 'ls200-web'
    append_group ls200-media-video 'ls200-sip'
    append_group ls200-media-audio 'ls200-sip'
    cmp -s /etc/group "$IDENTITY/group.before" || die 'refusing to stack over a changed group view'
    mount -o bind "$IDENTITY/group" /etc/group || die 'cannot install managed group bind mount'
    record "mount:/etc/group:$IDENTITY/group"
    record "file:$IDENTITY/group"
    record "file:$IDENTITY/group.before"
}

install_service_users_into_root_ssh() {
    if grep -Fx "root-ssh-passwd:$ROOT_PASSWD_SOURCE" "$JOURNAL" >/dev/null 2>&1; then return; fi
    [ ! -e "$ROOT_SSH_BACKUP" ] || die 'root-SSH passwd backup already exists without journal ownership'
    cp -p "$ROOT_PASSWD_SOURCE" "$ROOT_SSH_BACKUP"
    chmod 0600 "$ROOT_SSH_BACKUP"
    ROOT_PASSWD_TEMP=$ROOT_SSH_PROVISION/account-overlays/$ROOT_SSH_USER/.passwd.ls200-zoom.$$
    [ ! -e "$ROOT_PASSWD_TEMP" ] && [ ! -L "$ROOT_PASSWD_TEMP" ] || die 'root-SSH passwd temporary path already exists'
    cp "$ROOT_PASSWD_SOURCE" "$ROOT_PASSWD_TEMP"
    append_user ls200-web ls200-web; append_user ls200-gateway ls200-gateway; append_user ls200-sip ls200-sip
    chown 0:0 "$ROOT_PASSWD_TEMP"; chmod 0644 "$ROOT_PASSWD_TEMP"
    cmp -s "$ROOT_PASSWD_SOURCE" /etc/passwd || die 'root-SSH passwd bind view changed during update'
    mv "$ROOT_PASSWD_TEMP" "$ROOT_PASSWD_SOURCE"
    require_secure_path_chain "$ROOT_SSH_APPLY"
    "$ROOT_SSH_APPLY" || die 'managed root-SSH apply failed after passwd update'
    cmp -s "$ROOT_PASSWD_SOURCE" /etc/passwd || die 'managed root-SSH apply did not activate updated passwd source'
    record "root-ssh-passwd:$ROOT_PASSWD_SOURCE"
    record "root-ssh-passwd-backup:$ROOT_SSH_BACKUP"
    record "root-ssh-apply:$ROOT_SSH_APPLY"
}

validate_root_ssh_overlay
prepare_group_view
install_service_users_into_root_ssh
id ls200-web >/dev/null && id ls200-gateway >/dev/null && id ls200-sip >/dev/null || die 'managed service identities do not resolve'
[ -f "$SCRIPT_DIR/start.sh" ] && [ ! -L "$SCRIPT_DIR/start.sh" ] || die 'live starter is absent or unsafe'
[ -f "$SCRIPT_DIR/autostart.sh" ] && [ ! -L "$SCRIPT_DIR/autostart.sh" ] || die 'live autostart wrapper is absent or unsafe'
[ -f "$SCRIPT_DIR/rollback.sh" ] && [ ! -L "$SCRIPT_DIR/rollback.sh" ] || die 'live rollback helper is absent or unsafe'
[ -f "$SCRIPT_DIR/remove.sh" ] && [ ! -L "$SCRIPT_DIR/remove.sh" ] || die 'live removal helper is absent or unsafe'
require_vendor_media_wait
trust_verifier=$BASE/live-verify-payload.sh
if [ -e "$trust_verifier" ] || [ -L "$trust_verifier" ]; then
    require_secure_file "$trust_verifier"
    [ "$(grep -Fxc "file:$trust_verifier" "$JOURNAL")" = 1 ] || die 'preexisting trusted verifier is not uniquely owned'
fi
if ! cmp -s "$SCRIPT_DIR/verify-payload.sh" "$trust_verifier"; then
    while IFS=: read -r trust_kind trust_release; do
        [ "$trust_kind" = release ] || continue
        trust_digest=$(release_manifest_digest "$trust_release") || die 'cannot rotate verifier without independent release trust'
        sh "$SCRIPT_DIR/verify-payload.sh" --payload "$BASE/releases/$trust_release" \
            --manifest-sha256 "$trust_digest" >/dev/null || die 'replacement verifier rejects an installed release'
    done < "$JOURNAL"
    trust_temporary=$(mktemp "$BASE/.verify-payload.XXXXXX") || die 'cannot stage trusted verifier'
    cp "$SCRIPT_DIR/verify-payload.sh" "$trust_temporary"
    chown 0:0 "$trust_temporary"; chmod 0555 "$trust_temporary"
    mv "$trust_temporary" "$trust_verifier"
    durability_barrier
    record "file:$trust_verifier"
fi
transaction_helper=$BASE/live-transaction-records.sh
if [ -e "$transaction_helper" ] || [ -L "$transaction_helper" ]; then
    require_secure_file "$transaction_helper"
    grep -Fx "file:$transaction_helper" "$JOURNAL" >/dev/null 2>&1 || die 'preexisting transaction helper is not recorded as owned'
fi
publish_live_helper "$SCRIPT_DIR/transaction-records.sh" "$transaction_helper" 0444 'transaction helper'
if [ -e "$BASE/live-start.sh" ] || [ -L "$BASE/live-start.sh" ]; then
    require_secure_file "$BASE/live-start.sh"
    grep -Fx "file:$BASE/live-start.sh" "$JOURNAL" >/dev/null 2>&1 || die 'preexisting live starter is not recorded as owned'
fi
publish_live_helper "$SCRIPT_DIR/start.sh" "$BASE/live-start.sh" 0555 'live starter'
if [ -e "$BASE/live-autostart.sh" ] || [ -L "$BASE/live-autostart.sh" ]; then
    require_secure_file "$BASE/live-autostart.sh"
    grep -Fx "file:$BASE/live-autostart.sh" "$JOURNAL" >/dev/null 2>&1 || die 'preexisting live autostart wrapper is not recorded as owned'
fi
publish_live_helper "$SCRIPT_DIR/autostart.sh" "$BASE/live-autostart.sh" 0555 'live autostart wrapper'
for helper in rollback remove; do
    destination=$BASE/live-$helper.sh
    if [ -e "$destination" ] || [ -L "$destination" ]; then
        require_secure_file "$destination"
        grep -Fx "file:$destination" "$JOURNAL" >/dev/null 2>&1 || die "preexisting live $helper helper is not recorded as owned"
    fi
    publish_live_helper "$SCRIPT_DIR/$helper.sh" "$destination" 0555 "live $helper helper"
done
if [ -e "$MEDIA_GATE_RECORD" ] || [ -L "$MEDIA_GATE_RECORD" ]; then
    require_secure_file "$MEDIA_GATE_RECORD"
    [ "$(cat "$MEDIA_GATE_RECORD")" = "$VENDOR_MEDIA_WAIT" ] || die 'preexisting media-ready gate record is unsafe'
    grep -Fx "file:$MEDIA_GATE_RECORD" "$JOURNAL" >/dev/null 2>&1 || die 'preexisting media-ready gate record is not recorded as owned'
else
    temporary=$BASE/.media-ready-gate.$$
    printf '%s\n' "$VENDOR_MEDIA_WAIT" > "$temporary"
    chown 0:0 "$temporary"; chmod 0444 "$temporary"; mv "$temporary" "$MEDIA_GATE_RECORD"
fi
require_secure_file "$MEDIA_GATE_RECORD"
record "file:$MEDIA_GATE_RECORD"

reconcile_bootstrap_transaction
release=$RELEASES/$version
[ ! -e "$release" ] && [ ! -L "$release" ] || die "release already exists: $release"
staging=$RELEASES/.staging-$version-$$
[ ! -e "$staging" ] && [ ! -L "$staging" ] || die "staging path already exists: $staging"
mkdir "$staging"
cp -Rp "$payload/." "$staging/"
sh "$SCRIPT_DIR/verify-payload.sh" --payload "$staging" --manifest-sha256 "$manifest_sha" >/dev/null || die 'trusted staging verification failed'
find "$staging" -type f -exec chmod a-w {} \;
find "$staging" -type d -exec chmod a-w {} \;
chmod 0555 "$staging/bin/ls200-zoom-service" "$staging/bin/verify-ls200-zoom-payload" "$staging/etc/init.d/S99ls200-zoom"
sh "$SCRIPT_DIR/verify-payload.sh" --payload "$staging" --manifest-sha256 "$manifest_sha" >/dev/null || die 'trusted staging revalidation failed after mode sealing'
payload=$staging
require_live_payload_layout
mv "$staging" "$release"
sh "$SCRIPT_DIR/verify-payload.sh" --payload "$release" --manifest-sha256 "$manifest_sha" >/dev/null || die 'trusted installed release verification failed'
record "release:$version"
record "release-sha256:$version:$manifest_sha"

# Seed only absent operator state.  Later releases validate dynamic UID anchors
# rather than replacing files that an operator may have configured.
gateway_uid=$(user_id ls200-gateway) || die 'cannot resolve gateway service UID'
sip_uid=$(user_id ls200-sip) || die 'cannot resolve SIP service UID'
for directory in "$BASE/gateway" "$BASE/nginx" "$BASE/tls" "$BASE/device"; do
    [ ! -e "$directory" ] && mkdir "$directory"
    [ -d "$directory" ] && [ ! -L "$directory" ] || die "unsafe service state directory: $directory"
done
chown root:root "$BASE/device"
chmod 0700 "$BASE/device"
chown ls200-gateway:ls200-web-gateway "$BASE/gateway"
chmod 0700 "$BASE/gateway"
chown ls200-web:ls200-web-gateway "$BASE/nginx"
chmod 0700 "$BASE/nginx"
chown root:ls200-web-tls "$BASE/tls"
chmod 0750 "$BASE/tls"
if [ ! -e "$BASE/ls200-sipd.conf" ] && [ ! -L "$BASE/ls200-sipd.conf" ]; then
    sed -e 's/^enable_local_control = false$/enable_local_control = true/' \
        -e 's#^settings_file = /var/lib/ls200-sipd/settings.json$#settings_file = /run/ls200-zoom-state/sip/settings.json#' \
        -e "s/^gateway_uid = [0-9][0-9]*$/gateway_uid = $gateway_uid/" \
        "$release/etc/ls200-zoom/ls200-sipd.conf.example" > "$BASE/.ls200-sipd.conf.$$"
    chown ls200-sip "$BASE/.ls200-sipd.conf.$$"
    chmod 0600 "$BASE/.ls200-sipd.conf.$$"
    mv "$BASE/.ls200-sipd.conf.$$" "$BASE/ls200-sipd.conf"
    record "file:$BASE/ls200-sipd.conf"
fi
[ -f "$BASE/ls200-sipd.conf" ] && [ ! -L "$BASE/ls200-sipd.conf" ] || die 'SIP configuration is unsafe'
require_configured_uid "$BASE/ls200-sipd.conf" gateway_uid "$gateway_uid"
if [ ! -e "$BASE/gateway.conf" ] && [ ! -L "$BASE/gateway.conf" ] &&
   [ ! -e "$BASE/bootstrap-token" ] && [ ! -L "$BASE/bootstrap-token" ]; then
    bootstrap_code=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
    [ "${#bootstrap_code}" -eq 32 ] || die 'cannot generate bootstrap token'
    write_bootstrap_transaction "$version" "$manifest_sha" "$sip_uid" "$bootstrap_code"
    complete_bootstrap_transaction
elif [ ! -e "$BASE/gateway.conf" ] || [ -L "$BASE/gateway.conf" ] ||
     [ ! -e "$BASE/bootstrap-token" ] || [ -L "$BASE/bootstrap-token" ]; then
    die 'gateway configuration and bootstrap token must be a complete pair'
fi
[ -f "$BASE/gateway.conf" ] && [ ! -L "$BASE/gateway.conf" ] || die 'gateway configuration is unsafe'
require_bootstrap_journal_ownership
require_configured_uid "$BASE/gateway.conf" expected_sipd_uid "$sip_uid"

previous=
if [ -L "$CURRENT" ]; then
    previous=$(readlink "$CURRENT") || die 'cannot read current release link'
    case $previous in releases/*) previous_version=${previous#releases/} ;; *) die 'current release link is not profile-owned' ;; esac
    case $previous_version in ''|.|..|*[!A-Za-z0-9._-]*) die 'current release link is not profile-owned' ;; esac
elif [ -e "$CURRENT" ]; then
    die 'current release path is not a symlink'
fi
install_root_cron
verify_installed_release "$version"
ln -s "releases/$version" "$BASE/.current-$version-$$"
if [ -n "$previous" ]; then
    "$release/bin/ls200-atomic-replace" "$BASE/.current-$version-$$" "$CURRENT" || die 'atomic activation failed'
else
    mv "$BASE/.current-$version-$$" "$CURRENT"
fi
record "link:$CURRENT"
sh "$SCRIPT_DIR/verify-payload.sh" --payload "$release" --manifest-sha256 "$manifest_sha" >/dev/null || die 'active release changed during activation'

if [ "$start" = 1 ]; then
    "$BASE/live-start.sh" enable-autostart --internal-lock "$INTERNAL_LOCK" --activation-attempt --rollback-to "${previous#releases/}" || die 'service activation failed; live-start reported the recovery result'
fi
printf '%s\n' "live-install: activated $version"
