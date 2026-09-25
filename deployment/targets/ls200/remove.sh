#!/bin/sh
# Remove only files and bind mounts recorded by the live profile journal.
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

BASE=/var/lib/cbox/ls200-zoom
RELEASES=$BASE/releases
IDENTITY=$BASE/live-identities
JOURNAL=$BASE/live-owned-files
CURRENT=$BASE/current
ROOT_CRON=/var/lib/cbox/crontabs/root
CRON_COMMENT='# ls200-zoom live autostart'
CRON_MARKER_PREFIX='# ls200-zoom live autostart'
CRON_JOB="* * * * * $BASE/live-autostart.sh >/dev/null 2>&1"
MEDIA_GATE_RECORD=$BASE/media-ready-gate
AUTOSTART_ENABLED=$BASE/autostart-enabled
PRIVATE_LAB_TX=$BASE/private-lab-transaction; BOOTSTRAP_TX=$BASE/bootstrap-transaction
LOCK=/var/run/ls200-zoom-operation.lock
CRON_TX=$BASE/cron-transaction
AUTOSTART_TX=$BASE/autostart-transaction
PURGE_TX=$BASE/purge-transaction
ROLLBACK_TX=$BASE/rollback-transaction
STARTER=$BASE/live-start.sh
INTERNAL_LOCK=ls200-live-internal-v1; SIP_CA=$BASE/tls/sip-ca.crt; SIP_CA_UID=1000; SIP_CA_GID=0

die() { printf '%s\n' "live-remove: $*" >&2; exit 1; }
durability_barrier() { sync || die 'filesystem durability barrier failed'; }
clear_transaction() { rm -f "$1"; durability_barrier; }
write_transaction() {
    transaction=$1
    temporary=$(mktemp "$2") || die "cannot create $4 transaction temporary"
    printf '%s\n' "$3" > "$temporary"; chown 0:0 "$temporary"; chmod 0600 "$temporary"
    mv "$temporary" "$transaction" || die "cannot atomically record $4 transaction"
    durability_barrier
}
write_purge_tx() { write_transaction "$PURGE_TX" "$BASE/.purge-transaction.XXXXXX" "purge-v1:$1" purge; }
read_purge_tx() {
    require_regular "$PURGE_TX"; purge_phase=$(cat "$PURGE_TX")
    case $purge_phase in purge-v1:prepared|purge-v1:profile-removed|purge-v1:state-purged) ;; *) die 'purge transaction is unsafe' ;; esac
}
mounted_from() {
    [ -e "$1" ] && [ -e "$2" ] && [ "$1" -ef "$2" ] || return 1
    awk -v target="$2" '$5 == target { count++ } END { exit count != 1 }' /proc/self/mountinfo
}
mount_root_for_target() { awk -v target="$1" '$5 == target {count++; root=$4} END {if (count == 1) print root; else print "invalid"}' /proc/self/mountinfo; }
ROOT_SSH_PROVISION=/var/lib/cbox/root-ssh-provision
ROOT_SSH_BACKUP=$BASE/root-ssh-passwd.before
ROOT_PASSWD_SOURCE= ROOT_SSH_APPLY= ROOT_PASSWD_MOUNT=
validate_root_ssh_overlay() {
    ROOT_PASSWD_MOUNT=$(mount_root_for_target /etc/passwd)
    case $ROOT_PASSWD_MOUNT in /root-ssh-provision/account-overlays/*/passwd) ;; *) die 'expected managed root-SSH passwd bind mount is absent' ;; esac
    root_user=${ROOT_PASSWD_MOUNT#/root-ssh-provision/account-overlays/}; root_user=${root_user%/passwd}
    case $root_user in ''|*[!a-z0-9_-]*) die 'managed root-SSH account marker is unsafe' ;; esac
    ROOT_PASSWD_SOURCE=$ROOT_SSH_PROVISION/account-overlays/$root_user/passwd
    shadow_source=$ROOT_SSH_PROVISION/account-overlays/$root_user/shadow
    ROOT_SSH_APPLY=$ROOT_SSH_PROVISION/account-overlays/$root_user/apply.sh
    marker=$ROOT_SSH_PROVISION/account-overlays/$root_user/.ls200-root-shell-managed
    cron=/var/lib/cbox/crontabs/root
    [ -f "$ROOT_PASSWD_SOURCE" ] && [ ! -L "$ROOT_PASSWD_SOURCE" ] && [ -f "$shadow_source" ] && [ ! -L "$shadow_source" ] && [ -x "$ROOT_SSH_APPLY" ] && [ ! -L "$ROOT_SSH_APPLY" ] && [ -f "$marker" ] && [ ! -L "$marker" ] && [ -f "$cron" ] && [ ! -L "$cron" ] || die 'managed root-SSH overlay files are unsafe'
    [ "$(wc -l < "$marker" | tr -d ' ')" = 2 ] && [ "$(grep -Fxc 'managed-by=open-ls200-root-shell.sh' "$marker")" -eq 1 ] && [ "$(grep -Fxc "account=$root_user" "$marker")" -eq 1 ] || die 'managed root-SSH overlay marker does not match mount'
    comment="# ls200-root-ssh-$root_user managed cron-bind"; job="* * * * * $ROOT_SSH_APPLY >/dev/null 2>&1"
    [ "$(grep -Fxc "$comment" "$cron")" -eq 1 ] && [ "$(grep -Fxc "$job" "$cron")" -eq 1 ] || die 'managed root-SSH cron apply contract does not match mount'
    grep -Fx "passwd_source='$ROOT_PASSWD_SOURCE'" "$ROOT_SSH_APPLY" >/dev/null && grep -Fx "shadow_source='$shadow_source'" "$ROOT_SSH_APPLY" >/dev/null && grep -F "/etc/passwd '$ROOT_PASSWD_MOUNT'" "$ROOT_SSH_APPLY" >/dev/null || die 'managed root-SSH apply contract does not match mount'
    cmp -s "$ROOT_PASSWD_SOURCE" /etc/passwd || die 'managed root-SSH passwd source does not match its active bind view'
}

purge=0
case ${1:-} in
    --apply) ;;
    --purge-owned-state) purge=1 ;;
    *) printf '%s\n' "usage: $0 {--apply|--purge-owned-state}" >&2; exit 64 ;;
esac
[ "$(id -u)" = 0 ] || die 'must run as root on the target'
if [ -f "$JOURNAL" ] && [ ! -L "$JOURNAL" ]; then
    journal_present=1
elif [ "$purge" = 1 ] && [ -f "$PURGE_TX" ] && [ ! -L "$PURGE_TX" ]; then
    # The final durable phase can remove the journal before it unlinks itself.
    # It is handled after taking the shared lock below.
    journal_present=0
else
    die 'ownership journal is absent or unsafe'
fi

root_owned_not_writable() {
    [ -e "$1" ] && [ ! -L "$1" ] || return 1
    [ "$(ls -nd "$1" | awk 'NR == 1 { print $3 }')" = 0 ] || return 1
    find "$1" -prune \( -perm -020 -o -perm -002 \) | grep -q . && return 1
    return 0
}
require_secure_path_chain() { path=$1; while [ "$path" != / ]; do root_owned_not_writable "$path" || die "unsafe root-owned path chain: $path"; path=$(dirname "$path"); done; }
acquire_lock() {
    if mkdir "$LOCK" 2>/dev/null; then printf '%s\n' "$$" > "$LOCK/pid"; return; fi
    [ -f "$LOCK/pid" ] && [ ! -L "$LOCK/pid" ] || die 'another live operation is active'
    pid=$(cat "$LOCK/pid"); case $pid in ''|*[!0-9]*) die 'live operation lock is unsafe' ;; esac
    kill -0 "$pid" 2>/dev/null && die 'another live operation is active'
    rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || die 'stale live operation lock cannot be recovered'
    mkdir "$LOCK" || die 'cannot acquire live operation lock'; printf '%s\n' "$$" > "$LOCK/pid"
}
release_lock() { rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true; }
require_regular() { [ -f "$1" ] && [ ! -L "$1" ] || die "recorded file is absent or unsafe: $1"; }
require_rollback_release() {
    rollback_version=$1
    case $rollback_version in ''|.|..|*[!A-Za-z0-9._-]*) die 'rollback transaction release is unsafe' ;; esac
    rollback_release=$RELEASES/$rollback_version
    [ -d "$rollback_release" ] && [ ! -L "$rollback_release" ] || die 'rollback transaction release is absent or unsafe'
    verify_installed_release "$rollback_version"
}
rollback_selector_version() {
    [ -L "$CURRENT" ] || die 'active selector is absent during rollback recovery'
    rollback_selector=$(readlink "$CURRENT") || die 'cannot read active selector during rollback recovery'
    case $rollback_selector in releases/*) rollback_selector=${rollback_selector#releases/} ;; *) die 'active selector is unsafe during rollback recovery' ;; esac
    case $rollback_selector in ''|.|..|*[!A-Za-z0-9._-]*) die 'active selector is unsafe during rollback recovery' ;; esac
    printf '%s\n' "$rollback_selector"
}
read_rollback_transaction() {
    require_regular "$ROLLBACK_TX"; root_owned_not_writable "$ROLLBACK_TX" || die 'rollback transaction is unsafe'
    rollback_values=$(awk '
        $0 == "rollback-v1" { header++; next }
        /^old:[A-Za-z0-9._-]+$/ { old++; old_value=substr($0, 5); next }
        /^new:[A-Za-z0-9._-]+$/ { new++; new_value=substr($0, 5); next }
        /^phase:(prepared|applied|healthy)$/ { phase++; phase_value=substr($0, 7); next }
        { exit 1 }
        END { if (header == 1 && old == 1 && new == 1 && phase == 1) print old_value, new_value, phase_value; else exit 1 }
    ' "$ROLLBACK_TX") || die 'rollback transaction is unsafe'
    set -- $rollback_values; [ "$#" -eq 3 ] || die 'rollback transaction is unsafe'
    rollback_old=$1 rollback_new=$2 rollback_phase=$3
    [ "$rollback_old" != "$rollback_new" ] || die 'rollback transaction selectors are unsafe'
    require_rollback_release "$rollback_old"; require_rollback_release "$rollback_new"
}
restore_rollback_selector_and_service() {
    current_rollback_version=$(rollback_selector_version)
    require_rollback_release "$current_rollback_version"
    if [ "$current_rollback_version" != "$rollback_old" ]; then
        "$CURRENT/etc/init.d/S99ls200-zoom" stop >/dev/null 2>&1 || true
        replacement=$BASE/.rollback-remove-recover-$rollback_old-$$
        ln -s "releases/$rollback_old" "$replacement" || die 'cannot prepare rollback recovery selector'
        "$RELEASES/$rollback_old/bin/ls200-atomic-replace" "$replacement" "$CURRENT" || die 'cannot restore rollback recovery selector'
        durability_barrier
    fi
    "$STARTER" start --internal-lock "$INTERNAL_LOCK" || die 'cannot restore service during rollback recovery'
    "$STARTER" health --internal-lock "$INTERNAL_LOCK" || die 'cannot verify service during rollback recovery'
}
reconcile_rollback_transaction() {
    [ -e "$ROLLBACK_TX" ] || [ -L "$ROLLBACK_TX" ] || return 0
    read_rollback_transaction
    current_rollback_version=$(rollback_selector_version)
    require_rollback_release "$current_rollback_version"
    case $rollback_phase:$current_rollback_version in
        prepared:$rollback_old) clear_transaction "$ROLLBACK_TX" ;;
        prepared:*) die 'prepared rollback transaction changed the active selector' ;;
        applied:$rollback_old|applied:$rollback_new)
            restore_rollback_selector_and_service
            clear_transaction "$ROLLBACK_TX"
            ;;
        applied:*) die 'applied rollback transaction has an unexpected active selector' ;;
        healthy:$rollback_new) clear_transaction "$ROLLBACK_TX" ;;
        healthy:*) die 'healthy rollback transaction has an unexpected active selector' ;;
        *) die 'rollback transaction is unsafe' ;;
    esac
}
sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk 'NR == 1 { print $1 }'
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk 'NR == 1 { print $1 }'
    else die 'sha256sum or shasum is required'; fi
}
require_integrity_evidence() {
    target=$1
    [ "$(grep -Fxc "file:$target" "$JOURNAL")" -eq 1 ] ||
        die "managed immutable file is not uniquely journaled: $target"
    evidence=$(awk -v prefix="sha256:$target:" 'index($0, prefix) == 1 { count++; line=$0 } END { if (count != 1) exit 1; print line }' "$JOURNAL") ||
        die "managed immutable file lacks unique SHA-256 evidence: $target"
    digest=${evidence#sha256:$target:}
    case $digest in *[!0-9a-f]*|'') die "managed immutable SHA-256 evidence is unsafe: $target" ;; esac
    [ "${#digest}" -eq 64 ] || die "managed immutable SHA-256 evidence has an unsafe length: $target"
    require_regular "$target"
    actual=$(sha256_file "$target") || die "cannot hash managed immutable file: $target"
    [ "$actual" = "$digest" ] || die "managed immutable file changed outside the campaign: $target"
}
validate_runtime_leaf() {
    relative=${1#"$BASE"/}
    [ -f "$1" ] && [ ! -L "$1" ] || die "campaign runtime state is unsafe: $relative"
    [ "$relative" != tls/sip-ca.crt ] || { [ "$(ls -nd "$1" | awk 'NR == 1 { print $3 ":" $4 }')" = "$SIP_CA_UID:$SIP_CA_GID" ] || die 'SIP CA ownership is unsafe'; find "$1" -prune -perm 0600 | grep -Fx "$1" >/dev/null || die 'SIP CA mode is unsafe'; }
    case $relative in
        gateway/account.json|gateway/service.log|nginx/service.log|nginx/nginx-error.log|nginx/nginx-access.log|nginx/nginx.pid|sip/settings.json|sip/service.log|tls/server.crt|tls/server.key|tls/sip-ca.crt|ls200-sipd.conf|gateway.conf|bootstrap-token) ;;
        *) die "unexpected object blocks owned-state purge: $relative" ;;
    esac
}
validate_runtime_directory() {
    relative=${1#"$BASE"/}
    [ -d "$1" ] && [ ! -L "$1" ] || die "campaign runtime directory is unsafe: $relative"
    case $relative in
        nginx/client_body_temp|nginx/fastcgi_temp) ;;
        *) die "unexpected directory blocks owned-state purge: $relative" ;;
    esac
    find "$1" -mindepth 1 -print | grep -q . &&
        die "campaign runtime directory is not empty: $relative"
    return 0
}
validate_service_directory() {
    directory=$1
    [ -e "$directory" ] || [ -L "$directory" ] || return 0
    [ -d "$directory" ] && [ ! -L "$directory" ] || die "campaign directory is unsafe: $directory"
    while IFS= read -r object; do
        [ -z "$object" ] || if [ -f "$object" ] && [ ! -L "$object" ]; then
            validate_runtime_leaf "$object"
        elif [ -d "$object" ] && [ ! -L "$object" ]; then
            validate_runtime_directory "$object"
        else
            die "campaign directory contains a special object: $object"
        fi
    done <<EOF
$(find "$directory" -mindepth 1 -maxdepth 1 -print)
EOF
}
validate_runtime_state() {
    for directory in "$BASE/gateway" "$BASE/nginx" "$BASE/sip" "$BASE/tls"; do validate_service_directory "$directory"; done
    for leaf in "$BASE/ls200-sipd.conf" "$BASE/gateway.conf" "$BASE/bootstrap-token"; do [ ! -e "$leaf" ] && [ ! -L "$leaf" ] || validate_runtime_leaf "$leaf"; done
}
purge_runtime_state() {
    validate_runtime_state
    for directory in "$BASE/gateway" "$BASE/nginx" "$BASE/sip" "$BASE/tls"; do
        [ ! -d "$directory" ] || find "$directory" -mindepth 1 -maxdepth 1 -type f -exec rm -f {} \;
    done
    for directory in "$BASE/nginx/client_body_temp" "$BASE/nginx/fastcgi_temp"; do
        [ ! -d "$directory" ] || rmdir "$directory" || die "owned nginx runtime directory did not become empty: $directory"
    done
    rm -f "$BASE/ls200-sipd.conf" "$BASE/gateway.conf" "$BASE/bootstrap-token"
    rm -f "$IDENTITY/group" "$IDENTITY/group.before" "$ROOT_SSH_BACKUP"
    for directory in "$BASE/gateway" "$BASE/nginx" "$BASE/sip" "$BASE/tls" "$IDENTITY" "$RELEASES"; do
        [ ! -d "$directory" ] || rmdir "$directory" || die "owned directory did not become empty: $directory"
    done
    durability_barrier
}
validate_purge_integrity_evidence() {
    require_integrity_evidence "$BASE/ls200-sipd.conf"
    tls_present=0
    for target in "$BASE/tls/server.crt" "$BASE/tls/server.key"; do
        [ ! -e "$target" ] && [ ! -L "$target" ] || tls_present=1
    done
    if [ "$tls_present" -eq 1 ]; then
        require_integrity_evidence "$BASE/tls/server.crt"
        require_integrity_evidence "$BASE/tls/server.key"
    else
        for target in "$BASE/tls/server.crt" "$BASE/tls/server.key"; do
            grep -F "file:$target" "$JOURNAL" >/dev/null 2>&1 && die "absent TLS file remains journaled: $target"
            grep -F "sha256:$target:" "$JOURNAL" >/dev/null 2>&1 && die "absent TLS file retains SHA-256 evidence: $target"
        done
    fi
    if [ -e "$SIP_CA" ] || [ -L "$SIP_CA" ]; then require_integrity_evidence "$SIP_CA"; else grep -F "file:$SIP_CA" "$JOURNAL" >/dev/null 2>&1 && die "absent SIP CA remains journaled: $SIP_CA"; grep -F "sha256:$SIP_CA:" "$JOURNAL" >/dev/null 2>&1 && die "absent SIP CA retains SHA-256 evidence: $SIP_CA"; fi
    validate_runtime_state
    return 0
}
require_root_crontab() {
    [ -f "$ROOT_CRON" ] && [ ! -L "$ROOT_CRON" ] && root_owned_not_writable "$ROOT_CRON" ||
        die 'root crontab is absent or unsafe'
    [ -d /var/lib/cbox/crontabs ] && root_owned_not_writable /var/lib/cbox/crontabs ||
        die 'root crontab directory is unsafe'
    [ ! -s "$ROOT_CRON" ] || [ "$(tail -c 1 "$ROOT_CRON" | od -An -tu1 | tr -d ' ')" = 10 ] ||
        die 'root crontab is not safely line-terminated'
}
validate_owned_cron() {
    require_root_crontab
    [ "$(cron_state)" = present ] || die 'owned ls200-zoom cron lines are absent or ambiguous'
}
cron_state() {
    markers=$(grep -F "$CRON_MARKER_PREFIX" "$ROOT_CRON" | wc -l | tr -d ' '); comments=$(grep -Fxc "$CRON_COMMENT" "$ROOT_CRON" || true); jobs=$(grep -Fxc "$CRON_JOB" "$ROOT_CRON" || true)
    case $markers:$comments:$jobs in 0:0:0) printf '%s\n' absent ;; 1:1:1) printf '%s\n' present ;; *) die 'root crontab contains an ambiguous or conflicting ls200-zoom marker' ;; esac
}
write_cron_tx() { write_transaction "$CRON_TX" "$BASE/.cron-transaction.XXXXXX" "$1" cron; }
remove_cron_journal_entries() {
    temporary=$(mktemp "$BASE/.live-owned-files.XXXXXX") || die 'cannot create journal temporary'
    while IFS= read -r entry; do
        [ "$entry" = "cron-comment:$CRON_COMMENT" ] || [ "$entry" = "cron-job:$CRON_JOB" ] || printf '%s\n' "$entry" >> "$temporary"
    done < "$JOURNAL"
    chown 0:0 "$temporary"; chmod 0600 "$temporary"
    mv "$temporary" "$JOURNAL" || die 'cannot atomically reconcile cron ownership journal'
    durability_barrier
}
record() {
    if ! grep -Fx "$1" "$JOURNAL" >/dev/null 2>&1; then printf '%s\n' "$1" >> "$JOURNAL"; durability_barrier; fi
}
recover_cron_transaction() {
    [ -e "$CRON_TX" ] || [ -L "$CRON_TX" ] || return 0
    require_regular "$CRON_TX"; root_owned_not_writable "$CRON_TX" || die 'cron transaction is unsafe'; phase=$(cat "$CRON_TX")
    case $phase in cron-v1:prepared|cron-v1:applied|cron-v1:remove-prepared|cron-v1:remove-applied) ;; *) die 'cron transaction is unsafe' ;; esac
    case $phase:$(cron_state) in
        cron-v1:prepared:absent|cron-v1:remove-prepared:present) clear_transaction "$CRON_TX" ;;
        cron-v1:prepared:present|cron-v1:applied:present)
            record "cron-comment:$CRON_COMMENT"; record "cron-job:$CRON_JOB"; clear_transaction "$CRON_TX"
            ;;
        cron-v1:remove-prepared:absent|cron-v1:remove-applied:absent)
            remove_cron_journal_entries; clear_transaction "$CRON_TX"
            ;;
        cron-v1:applied:absent) die 'cron transaction lost its applied rows' ;;
        cron-v1:remove-applied:present) die 'cron removal transaction retained owned rows' ;;
    esac
}
require_autostart_marker() {
    require_regular "$AUTOSTART_ENABLED"; root_owned_not_writable "$AUTOSTART_ENABLED" || die 'autostart marker is unsafe'
    [ "$(cat "$AUTOSTART_ENABLED")" = enabled-v1 ] || die 'autostart marker is unsafe'
}
reconcile_autostart_transaction() {
    if [ -e "$AUTOSTART_TX" ] || [ -L "$AUTOSTART_TX" ]; then
        require_regular "$AUTOSTART_TX"; root_owned_not_writable "$AUTOSTART_TX" || die 'autostart transaction is unsafe'; phase=$(cat "$AUTOSTART_TX")
        reconcile_pending_autostart require_autostart_marker
        return
    fi
    if [ -e "$AUTOSTART_ENABLED" ] || [ -L "$AUTOSTART_ENABLED" ]; then
        require_autostart_marker
        grep -Fxc "file:$AUTOSTART_ENABLED" "$JOURNAL" | grep -qx 1 || die 'unjournaled autostart marker is rejected'
    fi
}
remove_owned_cron() {
    cron_remove_original=$(mktemp /var/lib/cbox/crontabs/.root.ls200-zoom-remove-original.XXXXXX) || die 'cannot snapshot root crontab'
    cron_remove_temporary=$(mktemp /var/lib/cbox/crontabs/.root.ls200-zoom-remove.XXXXXX) || { rm -f "$cron_remove_original"; die 'cannot create root crontab temporary'; }
    cp -p "$ROOT_CRON" "$cron_remove_original" || { rm -f "$cron_remove_original" "$cron_remove_temporary"; die 'cannot snapshot root crontab'; }
    while IFS= read -r line; do
        [ "$line" = "$CRON_COMMENT" ] || [ "$line" = "$CRON_JOB" ] || printf '%s\n' "$line" >> "$cron_remove_temporary"
    done < "$cron_remove_original"
    chown 0:0 "$cron_remove_temporary"; chmod 0600 "$cron_remove_temporary"
    write_cron_tx cron-v1:remove-prepared
    cmp -s "$cron_remove_original" "$ROOT_CRON" || { rm -f "$cron_remove_original" "$cron_remove_temporary"; die 'root crontab changed concurrently'; }
    mv "$cron_remove_temporary" "$ROOT_CRON" || die 'cannot atomically remove ls200-zoom cron lines'
    rm -f "$cron_remove_original"
    durability_barrier
    write_cron_tx cron-v1:remove-applied
    remove_cron_journal_entries
    clear_transaction "$CRON_TX"
}
validate_current() {
    if [ -L "$CURRENT" ]; then
        target=$(readlink "$CURRENT") || die 'cannot read active release link'
        case $target in releases/*) version=${target#releases/} ;; *) die 'active release link changed outside profile' ;; esac
        case $version in ''|.|..|*[!A-Za-z0-9._-]*) die 'active release link changed outside profile' ;; esac
        [ -d "$RELEASES/$version" ] && [ ! -L "$RELEASES/$version" ] || die 'active release target is absent or unsafe'
        if [ -e "$CURRENT/etc/init.d/S99ls200-zoom" ] || [ -L "$CURRENT/etc/init.d/S99ls200-zoom" ]; then
            require_regular "$CURRENT/etc/init.d/S99ls200-zoom"
        fi
    elif [ -e "$CURRENT" ]; then
        die 'active release path changed outside profile'
    fi
}
journal_recovery=0
journal_count() { grep -Fxc "$1" "$JOURNAL" || true; }
remove_journaled() { [ "$(journal_count "$1")" = 0 ] || rm -f "$2"; }
require_complete_journal_set() {
    incomplete_message=$1; shift; present=0 absent=0
    for journal_entry in "$@"; do
        case $(journal_count "$journal_entry") in 0) absent=1 ;; 1) present=1 ;; esac
    done
    [ "$present" = 0 ] || [ "$absent" = 0 ] || die "$incomplete_message"
}
require_journal_file() {
    [ "$journal_recovery" = 1 ] || require_regular "$1"
}
validate_entry() {
    entry=$1
    [ "$(grep -Fxc "$entry" "$JOURNAL")" -eq 1 ] || die "duplicate journal entry: $entry"
    case $entry in
        release-sha256:*) validate_release_digest_entry "$entry" ;;
        release:*)
            version=${entry#release:}; case $version in ''|.|..|*[!A-Za-z0-9._-]*) die "unsafe journal release entry: $entry" ;; esac
            [ "$journal_recovery" = 1 ] || { [ -d "$RELEASES/$version" ] && [ ! -L "$RELEASES/$version" ]; } || die "recorded release is absent or unsafe: $RELEASES/$version"
            ;;
        mount:/run/ls200-zoom-state:$BASE)
            if [ "$journal_recovery" = 0 ] && awk -v target=/run/ls200-zoom-state '$2 == target { found = 1 } END { exit !found }' /proc/mounts; then mounted_from "$BASE" /run/ls200-zoom-state || die 'runtime state mount changed outside profile'; fi
            ;;
        mount:/etc/group:$IDENTITY/group)
            [ "$journal_recovery" = 1 ] || { mounted_from "$IDENTITY/group" /etc/group && require_regular "$IDENTITY/group"; } || die 'recorded group bind mount changed outside profile'
            ;;
        root-ssh-passwd:*)
            source=${entry#root-ssh-passwd:}; [ "$source" = "$ROOT_PASSWD_SOURCE" ] || die 'root-SSH passwd journal source does not match active overlay'
            ;;
        root-ssh-passwd-backup:$ROOT_SSH_BACKUP) require_journal_file "$ROOT_SSH_BACKUP" ;;
        root-ssh-apply:*) [ "${entry#root-ssh-apply:}" = "$ROOT_SSH_APPLY" ] || die 'root-SSH apply journal path does not match active overlay' ;;
        sha256:$BASE/ls200-sipd.conf:*|sha256:$BASE/tls/server.crt:*|sha256:$BASE/tls/server.key:*|sha256:$SIP_CA:*)
            validate_sha256_entry "$entry"
            ;;
        file:$IDENTITY/group|file:$IDENTITY/group.before|file:$BASE/ls200-sipd.conf|file:$BASE/gateway.conf|file:$BASE/bootstrap-token|file:$BASE/tls/server.crt|file:$BASE/tls/server.key|file:$SIP_CA) require_journal_file "${entry#file:}" ;;
        file:$BASE/live-start.sh|file:$BASE/live-autostart.sh|file:$BASE/live-rollback.sh|file:$BASE/live-remove.sh|file:$BASE/live-transaction-records.sh|file:$BASE/live-verify-payload.sh) require_journal_file "${entry#file:}" ;;
        file:$MEDIA_GATE_RECORD)
            require_journal_file "$MEDIA_GATE_RECORD"
            [ "$journal_recovery" = 1 ] || [ "$(cat "$MEDIA_GATE_RECORD")" = /usr/share/media/wait_media_ready ] || die 'media-ready gate record is unsafe'
            ;;
        file:$AUTOSTART_ENABLED)
            require_journal_file "$AUTOSTART_ENABLED"
            [ "$journal_recovery" = 1 ] || [ "$(cat "$AUTOSTART_ENABLED")" = enabled-v1 ] || die 'autostart marker is unsafe'
            ;;
        cron-comment:$CRON_COMMENT|cron-job:$CRON_JOB) [ "$journal_recovery" = 1 ] || validate_owned_cron ;;
        link:$CURRENT)
            [ "$journal_recovery" = 1 ] || { [ -L "$CURRENT" ] && validate_current; } || die 'recorded active release link is absent'
            ;;
        *) die "unsupported journal entry: $entry" ;;
    esac
}
validate_journal() {
    journal_recovery=${1:-0}
    root_owned_not_writable "$JOURNAL" || die 'ownership journal is unsafe'
    [ "$journal_recovery" = 1 ] || validate_current
    while IFS= read -r entry; do validate_entry "$entry"; done < "$JOURNAL"
    if [ "$journal_recovery" = 0 ]; then
        require_complete_journal_set 'incomplete root-SSH restore journal' "root-ssh-passwd:$ROOT_PASSWD_SOURCE" "root-ssh-passwd-backup:$ROOT_SSH_BACKUP" "root-ssh-apply:$ROOT_SSH_APPLY"
        require_complete_journal_set 'incomplete ls200-zoom autostart journal' "file:$BASE/live-autostart.sh" "file:$MEDIA_GATE_RECORD"
    fi
    file_count=$(journal_count "file:$SIP_CA"); digest_count=$(grep -Fc "sha256:$SIP_CA:" "$JOURNAL" || true); case $file_count:$digest_count in 0:0|1:1) ;; *) die 'incomplete SIP CA integrity journal' ;; esac
    require_complete_journal_set 'incomplete ls200-zoom cron journal' "cron-comment:$CRON_COMMENT" "cron-job:$CRON_JOB"
}

# A prepared purge has already completed the full mutable-state preflight and
# has a durable, root-owned recovery record.  A later invocation must accept
# exact journaled objects that an earlier interrupted pass has already removed,
# but must never broaden the fixed journal grammar into a best-effort delete.
validate_purge_recovery_journal() {
    validate_journal 1
}
finalize_purge() {
    # This is reachable only after `state-purged` was durably recorded.  The
    # final unlinks are exact recovery artifacts; rmdir still rejects any
    # unexpected object rather than deleting it.
    rm -f "$BASE/live-start.sh" "$BASE/live-autostart.sh" "$BASE/live-rollback.sh"
    rm -f "$JOURNAL"; durability_barrier
    rm -f "$BASE/live-remove.sh" "$BASE/live-transaction-records.sh" "$BASE/live-verify-payload.sh"; durability_barrier
    rm -f "$PURGE_TX"
    rmdir "$BASE" || die 'owned campaign root did not become empty'
    durability_barrier
    printf '%s\n' 'live-remove: campaign-owned state purged; managed root-SSH substrate preserved'
}

# Validate every journal target before the first service stop, unmount, restore,
# unlink, or release deletion.  The second pass below is mutation-only.
acquire_lock; trap release_lock EXIT HUP INT TERM
[ ! -e "$BOOTSTRAP_TX" ] && [ ! -L "$BOOTSTRAP_TX" ] || die 'bootstrap transaction is unresolved; rerun the installer before removal'
if [ "$purge" = 1 ] && [ "$journal_present" = 0 ]; then
    read_purge_tx
    [ "$purge_phase" = purge-v1:state-purged ] || die 'purge transaction lost its recovery journal'
    finalize_purge
    exit 0
fi
require_secure_path_chain "$BASE/live-transaction-records.sh"
require_regular "$BASE/live-transaction-records.sh"
# shellcheck source=transaction-records.sh
. "$BASE/live-transaction-records.sh"
reconcile_rollback_transaction
if grep -q '^root-ssh-passwd:' "$JOURNAL"; then
    validate_root_ssh_overlay
fi
[ ! -e "$PRIVATE_LAB_TX" ] && [ ! -L "$PRIVATE_LAB_TX" ] || die 'private-lab configuration transaction is unresolved; recover it before removal'
recover_cron_transaction
reconcile_autostart_transaction
profile_removal=1
if [ "$purge" = 1 ] && { [ -e "$PURGE_TX" ] || [ -L "$PURGE_TX" ]; }; then
    read_purge_tx
    validate_purge_recovery_journal
    case $purge_phase in
        purge-v1:prepared) : ;;
        purge-v1:profile-removed) profile_removal=0 ;;
        purge-v1:state-purged) finalize_purge; exit 0 ;;
    esac
    validate_runtime_state
else
    validate_journal
    if [ "$purge" = 1 ]; then
        validate_purge_integrity_evidence
        write_purge_tx prepared
    fi
fi

# Cron must stop re-invoking the profile before the first service stop.  The
# rewrite removes only the two exact owned lines and preserves every other row.
if [ "$profile_removal" = 1 ]; then
[ "$(journal_count "cron-comment:$CRON_COMMENT")" = 0 ] || remove_owned_cron

if [ -L "$CURRENT" ] && [ -x "$CURRENT/etc/init.d/S99ls200-zoom" ]; then
    stop_version=$(rollback_selector_version)
    verify_installed_release "$stop_version"
    "$CURRENT/etc/init.d/S99ls200-zoom" stop >/dev/null 2>&1 || die 'cannot stop active services'
fi

while IFS= read -r entry; do
    case $entry in
        release:*)
            version=${entry#release:}
            release=$RELEASES/$version
            if [ -e "$release" ] || [ -L "$release" ]; then
                require_secure_path_chain "$release"
                awk -v base="$BASE" -v releases="$RELEASES" -v release="$release" '$5 == base || $5 == releases || $5 == release || index($5, release "/") == 1 { found = 1 } END { exit !found }' /proc/self/mountinfo && die 'recorded release or managed ancestor is mounted'
                rm -rf "$release"
                durability_barrier
            fi
            ;;
        mount:/run/ls200-zoom-state:$BASE)
            if mounted_from "$BASE" /run/ls200-zoom-state; then umount /run/ls200-zoom-state; fi
            ;;
        mount:/etc/group:$IDENTITY/group)
            if mounted_from "$IDENTITY/group" /etc/group; then umount /etc/group || die 'cannot remove managed group bind mount'; fi
            ;;
        root-ssh-passwd:*)
            require_secure_path_chain "$ROOT_SSH_APPLY"
            temporary=$ROOT_SSH_PROVISION/account-overlays/${ROOT_PASSWD_MOUNT#/root-ssh-provision/account-overlays/}; temporary=${temporary%/passwd}/.passwd.ls200-zoom-restore.$$
            cp "$ROOT_SSH_BACKUP" "$temporary"; chown 0:0 "$temporary"; chmod 0644 "$temporary"; mv "$temporary" "$ROOT_PASSWD_SOURCE"
            "$ROOT_SSH_APPLY" || die 'managed root-SSH apply failed while restoring passwd source'
            cmp -s "$ROOT_SSH_BACKUP" /etc/passwd || die 'managed root-SSH passwd restore did not activate exact prior bytes'
            ;;
        release-sha256:*|root-ssh-passwd-backup:*|root-ssh-apply:*|file:*|link:*|cron-comment:*|cron-job:*) : ;;
    esac
done < "$JOURNAL"

remove_journaled "link:$CURRENT" "$CURRENT"
[ "$purge" = 1 ] || remove_journaled "file:$BASE/live-start.sh" "$BASE/live-start.sh"
[ "$purge" = 1 ] || remove_journaled "file:$BASE/live-autostart.sh" "$BASE/live-autostart.sh"
remove_journaled "file:$MEDIA_GATE_RECORD" "$MEDIA_GATE_RECORD"
remove_journaled "file:$AUTOSTART_ENABLED" "$AUTOSTART_ENABLED"
[ "$purge" = 1 ] || remove_journaled "file:$BASE/live-rollback.sh" "$BASE/live-rollback.sh"
[ "$purge" = 1 ] || remove_journaled "file:$BASE/live-verify-payload.sh" "$BASE/live-verify-payload.sh"
if [ "$purge" = 1 ]; then write_purge_tx profile-removed; fi
fi
if [ "$purge" = 1 ]; then
    # Revalidate the fixed allowlist so a retry can complete a partial purge
    # without trusting state that changed after the durable preflight.
    purge_runtime_state
    write_purge_tx state-purged
    finalize_purge
else
    printf '%s\n' 'live-remove: recorded releases and managed bind mounts removed; audit journal retained'
fi
