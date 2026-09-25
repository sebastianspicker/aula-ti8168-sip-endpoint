#!/bin/sh
# Select a journaled immutable live release with crash-safe reversion.
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

BASE=/var/lib/cbox/ls200-zoom
RELEASES=$BASE/releases
CURRENT=$BASE/current
JOURNAL=$BASE/live-owned-files
STARTER=$BASE/live-start.sh
LOCK=/var/run/ls200-zoom-operation.lock
ROLLBACK_TX=$BASE/rollback-transaction
INTERNAL_LOCK=ls200-live-internal-v1

die() { printf '%s\n' "live-rollback: $*" >&2; exit 1; }
durability_barrier() { sync || die 'filesystem durability barrier failed'; }
root_owned_not_writable() {
    [ -e "$1" ] && [ ! -L "$1" ] || return 1
    [ "$(ls -nd "$1" | awk 'NR == 1 { print $3 }')" = 0 ] || return 1
    find "$1" -prune \( -perm -020 -o -perm -002 \) | grep -q . && return 1
    return 0
}
require_regular() { [ -f "$1" ] && [ ! -L "$1" ] && root_owned_not_writable "$1" || die "persistent file is unsafe: $1"; }
require_release() {
    candidate_version=$1
    case $candidate_version in ''|.|..|*[!A-Za-z0-9._-]*) die 'release version is unsafe' ;; esac
    candidate_release=$RELEASES/$candidate_version
    [ -d "$candidate_release" ] && [ ! -L "$candidate_release" ] || die 'release is absent or unsafe'
    verify_installed_release "$candidate_version"
}
acquire_lock() {
    if mkdir "$LOCK" 2>/dev/null; then printf '%s\n' "$$" > "$LOCK/pid"; return; fi
    [ -f "$LOCK/pid" ] && [ ! -L "$LOCK/pid" ] || die 'another live operation is active'
    pid=$(cat "$LOCK/pid"); case $pid in ''|*[!0-9]*) die 'live operation lock is unsafe' ;; esac
    kill -0 "$pid" 2>/dev/null && die 'another live operation is active'
    rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || die 'stale live operation lock cannot be recovered'
    mkdir "$LOCK" || die 'cannot acquire live operation lock'
    printf '%s\n' "$$" > "$LOCK/pid"
}
release_lock() { rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true; }
write_rollback_tx() {
    old_selector=$1 new_selector=$2 phase=$3
    temporary=$(mktemp "$BASE/.rollback-transaction.XXXXXX") || die 'cannot create rollback transaction temporary'
    {
        printf '%s\n' rollback-v1
        printf 'old:%s\n' "$old_selector"
        printf 'new:%s\n' "$new_selector"
        printf 'phase:%s\n' "$phase"
    } > "$temporary"
    chown 0:0 "$temporary"; chmod 0600 "$temporary"
    mv "$temporary" "$ROLLBACK_TX" || die 'cannot atomically record rollback transaction'
    durability_barrier
}
read_rollback_tx() {
    require_regular "$ROLLBACK_TX"
    rollback_values=$(read_rollback_values) || die 'rollback transaction is unsafe'
    set -- $rollback_values
    [ "$#" -eq 3 ] || die 'rollback transaction is unsafe'
    TX_OLD=$1 TX_NEW=$2 TX_PHASE=$3
    [ "$TX_OLD" != "$TX_NEW" ] || die 'rollback transaction selectors are unsafe'
    require_release "$TX_OLD"
    require_release "$TX_NEW"
}
clear_rollback_tx() { rm -f "$ROLLBACK_TX"; durability_barrier; }
selector_version() {
    [ -L "$CURRENT" ] || die 'active selector is absent or unsafe'
    selector=$(readlink "$CURRENT") || die 'cannot read active selector'
    case $selector in releases/*) selector=${selector#releases/} ;; *) die 'active selector is outside the live profile' ;; esac
    case $selector in ''|.|..|*[!A-Za-z0-9._-]*) die 'active release version is unsafe' ;; esac
    printf '%s\n' "$selector"
}
restore_selector_and_service() {
    restore_version=$1
    require_release "$restore_version"
    current_version=$(selector_version)
    require_release "$current_version"
    if [ "$current_version" != "$restore_version" ]; then
        "$CURRENT/etc/init.d/S99ls200-zoom" stop >/dev/null 2>&1 || true
        replacement=$BASE/.rollback-revert-$restore_version-$$
        ln -s "releases/$restore_version" "$replacement" || return 1
        rollback_candidate=$replacement
        "$RELEASES/$restore_version/bin/ls200-atomic-replace" "$replacement" "$CURRENT" || return 1
        rollback_candidate=
        durability_barrier || return 1
    fi
    "$STARTER" start --internal-lock "$INTERNAL_LOCK" || return 1
    "$STARTER" health --internal-lock "$INTERNAL_LOCK" || return 1
}
reconcile_rollback_transaction() {
    [ -e "$ROLLBACK_TX" ] || [ -L "$ROLLBACK_TX" ] || return 0
    read_rollback_tx
    current_version=$(selector_version)
    require_release "$current_version"
    case $TX_PHASE:$current_version in
        prepared:$TX_OLD) clear_rollback_tx ;;
        prepared:*) die 'prepared rollback transaction changed the active selector' ;;
        applied:$TX_OLD|applied:$TX_NEW)
            # `applied` is durable before the selector swap.  Either selector is
            # therefore recoverable to the known old release after a reset.
            restore_selector_and_service "$TX_OLD" || die 'cannot recover interrupted rollback'
            clear_rollback_tx
            ;;
        applied:*) die 'applied rollback transaction has an unexpected active selector' ;;
        healthy:$TX_NEW) clear_rollback_tx ;;
        healthy:*) die 'healthy rollback transaction has an unexpected active selector' ;;
        *) die 'rollback transaction is unsafe' ;;
    esac
}

[ "$#" -eq 2 ] && [ "$1" = --to ] || die 'usage: rollback.sh --to VERSION'
version=$2
case $version in ''|.|..|*[!A-Za-z0-9._-]*) die 'rollback version is unsafe' ;; esac
[ "$(id -u)" = 0 ] || die 'must run as root on the target'

# The lock is intentionally first: journal and selector reads describe shared
# mutable state and must not race a concurrent installer, starter, or remover.
rollback_applied=0
rollback_committed=0
rollback_old=
rollback_candidate=
cleanup() {
    result=$?
    trap - EXIT HUP INT TERM
    if [ "$result" -ne 0 ] && [ "$rollback_applied" = 1 ] && [ "$rollback_committed" = 0 ]; then
        if restore_selector_and_service "$rollback_old"; then
            clear_rollback_tx || true
        else
            printf '%s\n' 'live-rollback: failed to restore prior selector and service during recovery' >&2
        fi
    fi
    [ -z "$rollback_candidate" ] || rm -f "$rollback_candidate"
    release_lock
    exit "$result"
}
acquire_lock
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

[ -f "$JOURNAL" ] && [ ! -L "$JOURNAL" ] || die 'ownership journal is absent or unsafe'
require_regular "$JOURNAL"
require_regular "$BASE/live-transaction-records.sh"
for helper_parent in /var /var/lib /var/lib/cbox "$BASE"; do
    root_owned_not_writable "$helper_parent" || die 'transaction helper parent is unsafe'
done
# shellcheck source=transaction-records.sh
. "$BASE/live-transaction-records.sh"
reconcile_rollback_transaction
[ "$(grep -Fxc "release:$version" "$JOURNAL")" -eq 1 ] || die 'rollback release is not uniquely journaled'
require_release "$version"
rollback_old=$(selector_version)
[ "$rollback_old" != "$version" ] || die 'rollback target is already active'
require_release "$rollback_old"

write_rollback_tx "$rollback_old" "$version" prepared
write_rollback_tx "$rollback_old" "$version" applied
rollback_applied=1
"$CURRENT/etc/init.d/S99ls200-zoom" stop >/dev/null 2>&1 || die 'cannot stop active services'
candidate=$BASE/.rollback-$version-$$
ln -s "releases/$version" "$candidate"
rollback_candidate=$candidate
"$RELEASES/$version/bin/ls200-atomic-replace" "$candidate" "$CURRENT" || die 'atomic rollback selection failed'
rollback_candidate=
durability_barrier
if "$STARTER" start --internal-lock "$INTERNAL_LOCK" &&
   "$STARTER" health --internal-lock "$INTERNAL_LOCK"; then
    [ "$(selector_version)" = "$version" ] || die 'active selector changed during rollback verification'
    write_rollback_tx "$rollback_old" "$version" healthy
    clear_rollback_tx
    rollback_committed=1
    printf '%s\n' "live-rollback: selected and verified $version"
    exit 0
fi
die 'selected release failed health; previous release will be restored'
