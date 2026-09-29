#!/bin/sh
# Install a maintained Aula Zoom release without changing recovered images.
set -eu
umask 077

SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)
PAYLOAD_DIR=${AULA_ZOOM_PAYLOAD_DIR:-"$SCRIPT_DIR"}
ROOT=${AULA_ZOOM_ROOT:-/}
VERSION=${AULA_ZOOM_VERSION:-0.1.0}
MANIFEST_SHA256=${AULA_ZOOM_MANIFEST_SHA256:-}
TRUSTED_VERIFIER=$SCRIPT_DIR/verify-payload.sh
if [ ! -e "$TRUSTED_VERIFIER" ] && [ ! -L "$TRUSTED_VERIFIER" ]; then
    TRUSTED_VERIFIER=$SCRIPT_DIR/../targets/ti8168/verify-payload.sh
fi

die() {
    printf '%s\n' "error: $*" >&2
    exit 1
}

case $ROOT in
    /) ;;
    /*) ROOT=${ROOT%/} ;;
    *) die 'AULA_ZOOM_ROOT must be an absolute path' ;;
esac
case $VERSION in
    ''|.|..|*[!A-Za-z0-9._-]*) die 'AULA_ZOOM_VERSION contains unsupported characters' ;;
esac
case $MANIFEST_SHA256 in
    ''|*[!0-9a-f]*) die 'AULA_ZOOM_MANIFEST_SHA256 must be lowercase SHA-256' ;;
esac
[ "${#MANIFEST_SHA256}" -eq 64 ] || die 'AULA_ZOOM_MANIFEST_SHA256 must be SHA-256'

root_path() {
    if [ "$ROOT" = / ]; then
        printf '%s\n' "$1"
    else
        printf '%s%s\n' "$ROOT" "$1"
    fi
}

# BEGIN LEGACY NAME BOUNDARY: retain these paths during project renames.
for legacy_path in /opt/ls200-zoom /var/lib/cbox/ls200-zoom /run/ls200-zoom-state /var/run/ls200-zoom-operation.lock; do
    legacy_candidate=$(root_path "$legacy_path")
    if [ -e "$legacy_candidate" ] || [ -L "$legacy_candidate" ]; then
        die 'legacy installation is present; follow docs/operator/upgrade-to-aula.md before installing'
    fi
done
# END LEGACY NAME BOUNDARY

PREFIX=$(root_path /opt/aula-ti8168-sip-endpoint)
STATE=$(root_path /var/lib/cbox/aula-ti8168-sip-endpoint)
RELEASES=$PREFIX/releases
RELEASE=$RELEASES/$VERSION
CURRENT=$PREFIX/current
JOURNAL=$STATE/owned-files
NGINX_SELECTOR=$(root_path /var/lib/cbox/etc/nginx/nginx.conf)
NGINX_SELECTOR_DIR=$(dirname "$NGINX_SELECTOR")
NGINX_CANONICAL_HTTP=$(root_path /usr/share/nginx/nginx_http.conf)
NGINX_CANONICAL_HTTPS=$(root_path /usr/share/nginx/nginx_https.conf)
NGINX_INCLUDE='include /etc/nginx/conf.d/zz-aula-ti8168-sip-endpoint-legacy.conf;'
NGINX_COPY_HTTP=$STATE/nginx_zoom_http.conf
NGINX_COPY_HTTPS=$STATE/nginx_zoom_https.conf
NGINX_OWNED_HTTP_TARGET=/var/lib/cbox/aula-ti8168-sip-endpoint/nginx_zoom_http.conf
NGINX_OWNED_HTTPS_TARGET=/var/lib/cbox/aula-ti8168-sip-endpoint/nginx_zoom_https.conf
NGINX_SELECTOR_BACKUP=$STATE/nginx-selector.original
NGINX_COPIES_CREATED=''
NGINX_SELECTOR_REPOINTED=0
NGINX_SELECTOR_BACKUP_CREATED=0
STAGING=$RELEASES/.staging-$VERSION-$$
LOCK=$STATE/install.lock

durability_barrier() { sync || die 'filesystem durability barrier failed'; }
BOOTSTRAP_HELPER=$SCRIPT_DIR/bootstrap-transaction.sh
if [ ! -e "$BOOTSTRAP_HELPER" ] && [ ! -L "$BOOTSTRAP_HELPER" ]; then
    BOOTSTRAP_HELPER=$SCRIPT_DIR/../targets/ti8168/bootstrap-transaction.sh
fi
[ -f "$BOOTSTRAP_HELPER" ] && [ ! -L "$BOOTSTRAP_HELPER" ] && [ -O "$BOOTSTRAP_HELPER" ] ||
    die 'bootstrap transaction helper is absent or unsafe'
find "$BOOTSTRAP_HELPER" -prune \( -perm -020 -o -perm -002 \) | grep -q . &&
    die 'bootstrap transaction helper is writable by others'
BOOTSTRAP_ROOT=$STATE
BOOTSTRAP_RELEASES=$RELEASES
BOOTSTRAP_JOURNAL=
BOOTSTRAP_ROOT_UID=$(id -u)
BOOTSTRAP_ROOT_GID=$(id -g)
BOOTSTRAP_GATEWAY_OWNER=aula-gateway
bootstrap_release_identity() {
    sh "$TRUSTED_VERIFIER" --payload "$BOOTSTRAP_RELEASES/$1" --manifest-sha256 "$2" >/dev/null ||
        die 'bootstrap transaction release identity changed'
}
bootstrap_user_id() { id -u "$1"; }
bootstrap_record() { :; }
# shellcheck source=../targets/ti8168/bootstrap-transaction.sh
. "$BOOTSTRAP_HELPER"

[ -d "$PAYLOAD_DIR/runtime" ] || die "runtime payload is absent: $PAYLOAD_DIR/runtime"
[ -f "$TRUSTED_VERIFIER" ] && [ ! -L "$TRUSTED_VERIFIER" ] ||
    die "trusted installer verifier is absent or unsafe"
sh "$TRUSTED_VERIFIER" --payload "$PAYLOAD_DIR/runtime" \
    --manifest-sha256 "$MANIFEST_SHA256" >/dev/null ||
    die "trusted source payload verification failed"
mkdir -p "$RELEASES" "$STATE"
chmod 0755 "$PREFIX" "$RELEASES"
chmod 0700 "$STATE"
[ ! -L "$STATE" ] || die "state directory must not be a symlink: $STATE"
[ ! -e "$LOCK" ] || die "another Aula Zoom install is active: $LOCK"
mkdir "$LOCK"
trap 'rmdir "$LOCK"' EXIT HUP INT TERM
reconcile_bootstrap_transaction
[ ! -e "$RELEASE" ] || die "release already exists: $RELEASE"
[ ! -e "$STAGING" ] || die "staging path already exists: $STAGING"

if [ -e "$CURRENT" ] && [ ! -L "$CURRENT" ]; then
    die "activation target is not a symlink: $CURRENT"
fi

mkdir "$STAGING"
cp -Rp "$PAYLOAD_DIR/runtime/." "$STAGING/"
find "$STAGING" -type f -exec chmod a-w {} \;
find "$STAGING" -type d -exec chmod a-w {} \;
chmod u+w "$STAGING" "$STAGING/etc" "$STAGING/etc/init.d" "$STAGING/etc/nginx" "$STAGING/etc/nginx/conf.d"
chmod 0555 "$STAGING/etc/init.d/S99aula" "$STAGING/bin/aula-service"
sh "$TRUSTED_VERIFIER" --payload "$STAGING" \
    --manifest-sha256 "$MANIFEST_SHA256" >/dev/null ||
    die "trusted staging payload verification failed"
mv "$STAGING" "$RELEASE"
sh "$TRUSTED_VERIFIER" --payload "$RELEASE" \
    --manifest-sha256 "$MANIFEST_SHA256" >/dev/null ||
    die "trusted installed payload verification failed"

seed_state_file() {
    source=$1
    destination=$2
    if [ -e "$destination" ] || [ -L "$destination" ]; then
        [ -f "$destination" ] && [ ! -L "$destination" ] ||
            die "state configuration is not a regular file: $destination"
        return 0
    fi
    cp "$source" "$destination"
    chmod 0600 "$destination"
}

# User and group creation is intentionally separate from media authorization.
# Only the SIP identity joins media-video and media-audio.
ensure_group() {
    group=$1
    grep -q "^$group:" "$(root_path /etc/group)" 2>/dev/null ||
        addgroup "$group" >/dev/null 2>&1 || die "cannot create group: $group"
}

group_gid() {
    group=$1
    awk -F: -v group="$group" '
        $1 == group {
            if (seen++ || $3 !~ /^[0-9]+$/) invalid = 1
            gid = $3
        }
        END {
            if (!seen || invalid) exit 1
            print gid
        }
    ' "$(root_path /etc/group)"
}

user_primary_gid() {
    user=$1
    awk -F: -v user="$user" '
        $1 == user {
            if (seen++ || $4 !~ /^[0-9]+$/) invalid = 1
            gid = $4
        }
        END {
            if (!seen || invalid) exit 1
            print gid
        }
    ' "$(root_path /etc/passwd)"
}

ensure_user() {
    user=$1
    primary_group=$2
    primary_gid=$(group_gid "$primary_group") ||
        die "cannot resolve primary group: $primary_group"

    if grep -q "^$user:" "$(root_path /etc/passwd)" 2>/dev/null; then
        actual_gid=$(user_primary_gid "$user") ||
            die "cannot resolve primary GID for service user: $user"
        [ "$actual_gid" = "$primary_gid" ] ||
            die "service user $user must use primary group $primary_group"
        return
    fi

    adduser -D -H -s /sbin/nologin -G "$primary_group" "$user" >/dev/null 2>&1 ||
        die "cannot create service user: $user"
    actual_gid=$(user_primary_gid "$user") ||
        die "cannot resolve primary GID for service user: $user"
    [ "$actual_gid" = "$primary_gid" ] ||
        die "service user $user must use primary group $primary_group"
}

group_has_member() {
    user=$1
    group=$2
    awk -F: -v user="$user" -v group="$group" '
        $1 == group {
            count = split($4, members, ",")
            for (member_index = 1; member_index <= count; member_index++) {
                if (members[member_index] == user) found = 1
            }
        }
        END { exit !found }
    ' "$(root_path /etc/group)"
}

write_group_membership() {
    user=$1
    group=$2
    database=$(root_path /etc/group)
    temporary=$(root_path "/etc/.group.aula-ti8168-sip-endpoint.$$")
    [ -f "$database" ] && [ ! -L "$database" ] ||
        die "group database is not a regular file: $database"
    [ ! -e "$temporary" ] && [ ! -L "$temporary" ] ||
        die "temporary group database path already exists: $temporary"
    if ! awk -F: -v OFS=: -v user="$user" -v group="$group" '
        $1 == group {
            matches++
            count = split($4, members, ",")
            found = 0
            for (member_index = 1; member_index <= count; member_index++) {
                if (members[member_index] == user) found = 1
            }
            if (!found) $4 = ($4 == "" ? user : $4 "," user)
        }
        { print }
        END { if (matches != 1) exit 1 }
    ' "$database" > "$temporary"; then
        rm -f "$temporary"
        die "cannot update required group membership: $user -> $group"
    fi
    chown root:root "$temporary"
    chmod 0644 "$temporary"
    mv "$temporary" "$database"
}

ensure_membership() {
    user=$1
    group=$2
    group_has_member "$user" "$group" && return
    # BusyBox has shipped both positional and -G membership forms. The
    # recovered applet advertises both, so try each and then trust only the
    # parsed group database below.
    adduser "$user" "$group" >/dev/null 2>&1 ||
        adduser -G "$group" "$user" >/dev/null 2>&1 ||
        write_group_membership "$user" "$group"
    group_has_member "$user" "$group" ||
        die "required group membership was not established: $user -> $group"
}

require_configured_uid() {
    file=$1
    key=$2
    value=$3
    case $key in
        gateway_uid)
            pattern="^[[:space:]]*gateway_uid[[:space:]]*=[[:space:]]*${value}[[:space:]]*$"
            ;;
        expected_sipd_uid)
            pattern="^[[:space:]]*\"expected_sipd_uid\"[[:space:]]*:[[:space:]]*${value}[[:space:]]*,?[[:space:]]*$"
            ;;
        *) die "unsupported UID configuration key: $key" ;;
    esac
    grep -Eq "$pattern" "$file" ||
        die "preserved configuration has stale $key; migrate it to UID $value before activation: $file"
}

# The root-prefix mode is a staging aid. Actual account provisioning is done
# only inside the guest root, where BusyBox account tools update its databases.
if [ "$ROOT" = / ]; then
    ensure_group aula-web
    ensure_group aula-gateway
    ensure_group aula-sip
    ensure_group aula-media-video
    ensure_group aula-media-audio
    ensure_group aula-control
    ensure_group aula-web-gateway
    ensure_group aula-web-tls
    ensure_user aula-web aula-web
    ensure_user aula-gateway aula-gateway
    ensure_user aula-sip aula-sip
    web_gid=$(group_gid aula-web) || die 'cannot resolve web service group'
    gateway_gid=$(group_gid aula-gateway) || die 'cannot resolve gateway service group'
    sip_gid=$(group_gid aula-sip) || die 'cannot resolve SIP service group'
    [ "$web_gid" != "$gateway_gid" ] && [ "$web_gid" != "$sip_gid" ] &&
        [ "$gateway_gid" != "$sip_gid" ] ||
        die 'service users must have distinct primary groups'
    ensure_membership aula-sip aula-media-video
    ensure_membership aula-sip aula-media-audio
    ensure_membership aula-sip aula-control
    ensure_membership aula-gateway aula-control
    ensure_membership aula-gateway aula-web-gateway
    ensure_membership aula-web aula-web-gateway
    ensure_membership aula-web aula-web-tls

    sip_uid=$(id -u aula-sip) || die 'cannot resolve SIP service UID'
    gateway_uid=$(id -u aula-gateway) || die 'cannot resolve gateway service UID'
    chmod 0755 "$STATE"
    for service_dir in gateway nginx device; do
        directory=$STATE/$service_dir
        if [ ! -e "$directory" ]; then mkdir "$directory"; fi
        [ -d "$directory" ] && [ ! -L "$directory" ] ||
            die "unsafe service state directory: $directory"
    done
    chown root:root "$STATE/device"
    chmod 0700 "$STATE/device"
    chown aula-gateway:aula-web-gateway "$STATE/gateway"
    chmod 0700 "$STATE/gateway"
    chown aula-web:aula-web-gateway "$STATE/nginx"
    chmod 0700 "$STATE/nginx"
    if [ ! -e "$STATE/tls" ]; then mkdir "$STATE/tls"; fi
    [ -d "$STATE/tls" ] && [ ! -L "$STATE/tls" ] ||
        die "unsafe TLS state directory: $STATE/tls"
    chown root:aula-web-tls "$STATE/tls"
    chmod 0750 "$STATE/tls"

    if [ ! -e "$STATE/aula-sipd.conf" ] && [ ! -L "$STATE/aula-sipd.conf" ]; then
        sed -e 's/^enable_local_control = false$/enable_local_control = true/' \
            -e 's#^settings_file = /var/lib/aula-sipd/settings.json$#settings_file = /run/aula-state/sip/settings.json#' \
            -e "s/^gateway_uid = [0-9][0-9]*$/gateway_uid = $gateway_uid/" \
            "$RELEASE/etc/aula-ti8168-sip-endpoint/aula-sipd.conf.example" > "$STATE/.aula-sipd.conf.$$"
        chmod 0600 "$STATE/.aula-sipd.conf.$$"
        chown aula-sip "$STATE/.aula-sipd.conf.$$"
        mv "$STATE/.aula-sipd.conf.$$" "$STATE/aula-sipd.conf"
        grep -Fxq 'settings_file = /run/aula-state/sip/settings.json' \
            "$STATE/aula-sipd.conf" ||
            die 'runtime settings path anchor was not replaced'
    else
        [ -f "$STATE/aula-sipd.conf" ] && [ ! -L "$STATE/aula-sipd.conf" ] ||
            die "state configuration is not a regular file: $STATE/aula-sipd.conf"
    fi
    require_configured_uid "$STATE/aula-sipd.conf" gateway_uid "$gateway_uid"
    if [ ! -e "$STATE/gateway.conf" ] && [ ! -L "$STATE/gateway.conf" ] &&
       [ ! -e "$STATE/bootstrap-token" ] && [ ! -L "$STATE/bootstrap-token" ]; then
        bootstrap_code=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
        [ "${#bootstrap_code}" -eq 32 ] || die 'cannot generate bootstrap token'
        write_bootstrap_transaction "$VERSION" "$MANIFEST_SHA256" "$sip_uid" "$bootstrap_code"
        complete_bootstrap_transaction
        printf '%s\n' "first-use bootstrap token written to $STATE/bootstrap-token"
    elif [ ! -e "$STATE/gateway.conf" ] || [ -L "$STATE/gateway.conf" ] ||
         [ ! -e "$STATE/bootstrap-token" ] || [ -L "$STATE/bootstrap-token" ]; then
        die 'gateway configuration and bootstrap token must be a complete pair'
    fi
    require_configured_uid "$STATE/gateway.conf" expected_sipd_uid "$sip_uid"
else
    seed_state_file "$RELEASE/etc/aula-ti8168-sip-endpoint/aula-sipd.conf.example" \
        "$STATE/aula-sipd.conf"
    seed_state_file "$RELEASE/etc/aula-ti8168-sip-endpoint/gateway.conf.example" \
        "$STATE/gateway.conf"
fi

record_owned() {
    entry=$1
    grep -Fx "$entry" "$JOURNAL" >/dev/null 2>&1 || printf '%s\n' "$entry" >> "$JOURNAL"
}

atomic_replace() {
    source=$1
    destination=$2
    helper=$3
    [ -x "$helper" ] && [ ! -L "$helper" ] ||
        die "atomic replacement helper is absent or unsafe: $helper"
    "$helper" "$source" "$destination" ||
        die "atomic replacement failed: $destination"
}

require_regular_file() {
    [ -f "$1" ] && [ ! -L "$1" ] || die "required file is absent or unsafe: $1"
}

sha256_file() {
    require_regular_file "$1"
    command -v sha256sum >/dev/null 2>&1 || die 'sha256sum is required for vendor rollback safety'
    digest=$(sha256sum "$1" | awk 'NR == 1 { print $1 }') ||
        die "cannot hash required file: $1"
    [ "${#digest}" -eq 64 ] || die "invalid SHA-256 digest for file: $1"
    case $digest in *[!0123456789abcdef]*) die "invalid SHA-256 digest for file: $1" ;; esac
    printf '%s\n' "$digest"
}

nginx_include_state() {
    require_regular_file "$1"
    awk -v include="$NGINX_INCLUDE" '
        {
            line = $0
            sub(/^[[:space:]]*/, "", line)
            sub(/[[:space:]]*$/, "", line)
            if (line == "listen 80;") {
                anchors++
                anchor_line = NR
            }
            if (line == include) {
                includes++
                include_line = NR
            }
        }
        END {
            if (anchors != 1) exit 1
            if (includes == 0) print "original"
            else if (includes == 1 && include_line == anchor_line + 1) print "patched"
            else exit 1
        }
    ' "$1" || die "nginx configuration has no unique safe web-listener anchor: $1"
}

NGINX_RECORDS=$SCRIPT_DIR/nginx-records.sh
require_regular_file "$NGINX_RECORDS"
[ -O "$NGINX_RECORDS" ] || die 'nginx record helper has unexpected owner'
find "$NGINX_RECORDS" -prune \( -perm -020 -o -perm -002 \) | grep -q . && die 'nginx record helper is writable by others'
# shellcheck source=nginx-records.sh
. "$NGINX_RECORDS"



nginx_canonical_path() {
    case $1 in
        http) printf '%s\n' "$NGINX_CANONICAL_HTTP" ;;
        https) printf '%s\n' "$NGINX_CANONICAL_HTTPS" ;;
        *) die "unsupported nginx mode: $1" ;;
    esac
}





write_nginx_copy_hashes() {
    mode=$1
    original=$2
    patched=$3
    hashes=$(nginx_hash_path "$mode")
    temporary=$STATE/.nginx_zoom_$mode.conf.sha256.$$
    [ ! -e "$temporary" ] && [ ! -L "$temporary" ] ||
        die "nginx hash temporary path already exists: $temporary"
    {
        printf 'original:%s\n' "$original"
        printf 'patched:%s\n' "$patched"
    } > "$temporary"
    chmod 0600 "$temporary"
    mv "$temporary" "$hashes"
}

selector_original_mode() {
    [ -L "$NGINX_SELECTOR" ] || die "nginx selector is not a symlink: $NGINX_SELECTOR"
    target=$(readlink "$NGINX_SELECTOR") || die "cannot read nginx selector: $NGINX_SELECTOR"
    case $target in
        nginx_http.conf|/var/lib/cbox/etc/nginx/nginx_http.conf|/usr/share/nginx/nginx_http.conf)
            printf '%s\n' http ;;
        nginx_https.conf|/var/lib/cbox/etc/nginx/nginx_https.conf|/usr/share/nginx/nginx_https.conf)
            printf '%s\n' https ;;
        *) die "nginx selector has an unfamiliar target: $target" ;;
    esac
}

selector_owned_mode() {
    [ -L "$NGINX_SELECTOR" ] || die "nginx selector is not a symlink: $NGINX_SELECTOR"
    target=$(readlink "$NGINX_SELECTOR") || die "cannot read nginx selector: $NGINX_SELECTOR"
    case $target in
        "$NGINX_OWNED_HTTP_TARGET") printf '%s\n' http ;;
        "$NGINX_OWNED_HTTPS_TARGET") printf '%s\n' https ;;
        *) die "nginx selector changed outside this installer: $target" ;;
    esac
}


write_selector_backup() {
    target=$1
    mode=$2
    temporary=$STATE/.nginx-selector.original.$$
    [ ! -e "$temporary" ] && [ ! -L "$temporary" ] ||
        die "nginx selector backup temporary path already exists: $temporary"
    {
        printf 'target:%s\n' "$target"
        printf 'mode:%s\n' "$mode"
    } > "$temporary"
    chmod 0600 "$temporary"
    mv "$temporary" "$NGINX_SELECTOR_BACKUP"
    NGINX_SELECTOR_BACKUP_CREATED=1
}

reuse_nginx_copy() {
    mode=$1
    copy=$2
    hashes=$3
    original=$4
    if [ ! -e "$copy" ] && [ ! -L "$copy" ] && [ ! -e "$hashes" ] && [ ! -L "$hashes" ]; then
        return 1
    fi
    require_regular_file "$copy"
    set -- $(read_nginx_copy_hashes "$mode")
    recorded_original=$1
    patched=$2
    [ "$recorded_original" = "$original" ] && [ "$(sha256_file "$copy")" = "$patched" ] ||
        die "nginx owned copy changed outside this installer: $copy"
    record_owned "nginx-copy:$mode"
    return 0
}

prepare_nginx_copy() {
    mode=$1
    canonical=$(nginx_canonical_path "$mode")
    copy=$(nginx_copy_path "$mode")
    hashes=$(nginx_hash_path "$mode")
    case $(nginx_include_state "$canonical") in
        original) ;;
        *) die "canonical nginx template is unexpectedly patched: $canonical" ;;
    esac
    original=$(sha256_file "$canonical")
    reuse_nginx_copy "$mode" "$copy" "$hashes" "$original" && return
    temporary=$STATE/.nginx_zoom_$mode.conf.$$
    rendered=$temporary.rendered
    [ ! -e "$temporary" ] && [ ! -L "$temporary" ] &&
        [ ! -e "$rendered" ] && [ ! -L "$rendered" ] ||
        die "nginx copy temporary path already exists: $temporary"
    cp -p "$canonical" "$temporary" || die "cannot copy canonical nginx template: $canonical"
    if ! awk -v include="$NGINX_INCLUDE" '
        {
            print
            line = $0
            sub(/^[[:space:]]*/, "", line)
            sub(/[[:space:]]*$/, "", line)
            if (line == "listen 80;") {
                indent = $0
                sub(/[^[:space:]].*$/, "", indent)
                print indent include
            }
        }
    ' "$canonical" > "$rendered" || ! cat "$rendered" > "$temporary"; then
        rm -f "$temporary" "$rendered"
        die "cannot render nginx owned copy: $copy"
    fi
    rm -f "$rendered"
    [ "$(nginx_include_state "$temporary")" = patched ] || {
        rm -f "$temporary"
        die "rendered nginx owned copy failed validation: $copy"
    }
    patched=$(sha256_file "$temporary")
    mv "$temporary" "$copy" || { rm -f "$temporary"; die "cannot install nginx owned copy: $copy"; }
    [ "$(sha256_file "$copy")" = "$patched" ] || die "installed nginx owned copy hash mismatch: $copy"
    write_nginx_copy_hashes "$mode" "$original" "$patched"
    NGINX_COPIES_CREATED="$NGINX_COPIES_CREATED $mode"
    record_owned "nginx-copy:$mode"
}

repoint_nginx_selector() {
    if [ -e "$NGINX_SELECTOR_BACKUP" ] || [ -L "$NGINX_SELECTOR_BACKUP" ]; then
        set -- $(read_selector_backup)
        original_target=$1
        original_mode=$2
        [ "$(selector_owned_mode)" = "$original_mode" ] ||
            die 'nginx selector mode changed outside this installer'
    else
        original_mode=$(selector_original_mode)
        original_target=$(readlink "$NGINX_SELECTOR")
        write_selector_backup "$original_target" "$original_mode"
    fi
    target=$(nginx_owned_target "$original_mode")
    temporary=$NGINX_SELECTOR_DIR/.nginx.conf.aula-ti8168-sip-endpoint.$$
    [ -d "$NGINX_SELECTOR_DIR" ] && [ ! -L "$NGINX_SELECTOR_DIR" ] ||
        die "nginx selector directory is unsafe: $NGINX_SELECTOR_DIR"
    [ ! -e "$temporary" ] && [ ! -L "$temporary" ] ||
        die "nginx selector temporary path already exists: $temporary"
    if [ "$(readlink "$NGINX_SELECTOR")" != "$target" ]; then
        ln -s "$target" "$temporary" || die 'cannot prepare nginx selector replacement'
        mv "$temporary" "$NGINX_SELECTOR" || { rm -f "$temporary"; die 'cannot replace nginx selector'; }
        NGINX_SELECTOR_REPOINTED=1
    fi
    [ "$(selector_owned_mode)" = "$original_mode" ] || die 'installed nginx selector has unexpected target'
    record_owned 'nginx-selector:/var/lib/cbox/etc/nginx/nginx.conf'
}

validate_live_nginx_copies() {
    validation_include="include $RELEASE/etc/nginx/conf.d/zz-aula-ti8168-sip-endpoint-legacy.conf;"
    result=0
    [ "$ROOT" = / ] || return 0
    if [ -x /usr/sbin/nginx ] && [ ! -L /usr/sbin/nginx ]; then
        nginx_binary=/usr/sbin/nginx
    elif nginx_binary=$(command -v nginx 2>/dev/null) &&
        [ -x "$nginx_binary" ] && [ ! -L "$nginx_binary" ]; then
        :
    else
        return 0
    fi
    for mode in http https; do
        copy=$(nginx_copy_path "$mode")
        validation_config=$STATE/.nginx_zoom_$mode.validate.$$
        [ ! -e "$validation_config" ] && [ ! -L "$validation_config" ] ||
            die "nginx validation path already exists: $validation_config"
        awk -v include="$NGINX_INCLUDE" -v replacement="$validation_include" '
            {
                line = $0
                sub(/^[[:space:]]*/, "", line)
                sub(/[[:space:]]*$/, "", line)
                indent = $0
                sub(/[^[:space:]].*$/, "", indent)
                if (line == include) print indent replacement
                else if (line ~ /^include[[:space:]]+mime[.]types;$/)
                    print indent "include /etc/nginx/mime.types;"
                else print
            }
        ' "$copy" > "$validation_config" ||
            die "cannot prepare nginx validation configuration: $copy"
        "$nginx_binary" -t -p /etc/nginx -c "$validation_config" || result=$?
        rm -f "$validation_config"
    done
    return "$result"
}

prune_nginx_journal_entry() {
    entry=$1
    temporary=$STATE/.owned-files.rollback.$$
    [ ! -e "$temporary" ] && [ ! -L "$temporary" ] || die "journal rollback temporary path already exists: $temporary"
    awk -v entry="$entry" '$0 != entry' "$JOURNAL" > "$temporary" || die 'cannot update nginx rollback journal'
    chmod 0600 "$temporary"
    mv "$temporary" "$JOURNAL"
}

rollback_nginx_after_failed_validation() {
    if [ "$NGINX_SELECTOR_REPOINTED" = 1 ]; then
        set -- $(read_selector_backup)
        original_target=$1
        original_mode=$2
        [ "$(selector_owned_mode)" = "$original_mode" ] ||
            die 'nginx selector changed before failed-validation rollback'
        temporary=$NGINX_SELECTOR_DIR/.nginx.conf.aula-ti8168-sip-endpoint-rollback.$$
        [ ! -e "$temporary" ] && [ ! -L "$temporary" ] ||
            die "nginx selector rollback temporary path already exists: $temporary"
        ln -s "$original_target" "$temporary" || die 'cannot prepare nginx selector rollback'
        mv "$temporary" "$NGINX_SELECTOR" || die 'cannot restore nginx selector after failed validation'
        [ "$(readlink "$NGINX_SELECTOR")" = "$original_target" ] || die 'nginx selector rollback mismatch'
        prune_nginx_journal_entry 'nginx-selector:/var/lib/cbox/etc/nginx/nginx.conf'
    fi
    for mode in $NGINX_COPIES_CREATED; do
        copy=$(nginx_copy_path "$mode")
        hashes=$(nginx_hash_path "$mode")
        set -- $(read_nginx_copy_hashes "$mode")
        [ "$(sha256_file "$copy")" = "$2" ] || die "nginx owned copy changed before validation rollback: $copy"
        rm -f "$copy" "$hashes"
        prune_nginx_journal_entry "nginx-copy:$mode"
    done
    if [ "$NGINX_SELECTOR_BACKUP_CREATED" = 1 ]; then
        rm -f "$NGINX_SELECTOR_BACKUP"
    fi
}

install_link() {
    destination=$(root_path "$1")
    source=$2
    if [ -e "$destination" ] || [ -L "$destination" ]; then
        [ -L "$destination" ] && [ "$(readlink "$destination")" = "$source" ] ||
            die "refusing to replace unowned path: $destination"
    else
        mkdir -p "$(dirname "$destination")"
        ln -s "$source" "$destination"
        record_owned "link:$1"
    fi
}

: > "$JOURNAL.new"
chmod 0600 "$JOURNAL.new"
if [ -e "$JOURNAL" ]; then
    [ -f "$JOURNAL" ] && [ ! -L "$JOURNAL" ] ||
        die "owned-file journal is not a regular file: $JOURNAL"
    cat "$JOURNAL" > "$JOURNAL.new"
fi
mv "$JOURNAL.new" "$JOURNAL"
chmod 0600 "$JOURNAL"
prepare_nginx_copy http
prepare_nginx_copy https
repoint_nginx_selector
if ! validate_live_nginx_copies; then
    rollback_nginx_after_failed_validation
    die 'nginx validation failed after preparing persistent copies'
fi
install_link /etc/init.d/S99aula \
    /opt/aula-ti8168-sip-endpoint/current/etc/init.d/S99aula
install_link /etc/nginx/conf.d/zz-aula-ti8168-sip-endpoint-legacy.conf \
    /opt/aula-ti8168-sip-endpoint/current/etc/nginx/conf.d/zz-aula-ti8168-sip-endpoint-legacy.conf
record_owned "release:$VERSION"
record_owned "release-sha256:$VERSION:$MANIFEST_SHA256"

# Replacing a same-directory symlink is atomic for readers. The old release is
# retained for explicit rollback and is never mutated after activation.
sh "$TRUSTED_VERIFIER" --payload "$RELEASE" \
    --manifest-sha256 "$MANIFEST_SHA256" >/dev/null ||
    die "release changed before activation"
ln -s "releases/$VERSION" "$PREFIX/.current-$VERSION-$$"
atomic_replace "$PREFIX/.current-$VERSION-$$" "$CURRENT" \
    "$RELEASE/bin/aula-atomic-replace"
sh "$TRUSTED_VERIFIER" --payload "$RELEASE" \
    --manifest-sha256 "$MANIFEST_SHA256" >/dev/null ||
    die "active release changed during activation"
record_owned 'link:/opt/aula-ti8168-sip-endpoint/current'
printf '%s\n' "$VERSION" > "$STATE/active-version"
printf '%s\n' "installed Aula Zoom release $VERSION"
