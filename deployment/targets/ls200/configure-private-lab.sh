#!/bin/sh
# Create an explicit, local-only private-lab SIP/media configuration.
set -eu
umask 077

BASE=/var/lib/cbox/ls200-zoom
CONFIG=$BASE/ls200-sipd.conf
TLS=$BASE/tls
TLS_CERT=$TLS/server.crt
TLS_KEY=$TLS/server.key
JOURNAL=$BASE/live-owned-files
PRIVATE_LAB_TX=$BASE/private-lab-transaction
CONFIG_STAGE=$BASE/.ls200-sipd.private-lab.stage
TLS_CERT_STAGE=$TLS/.server.crt.private-lab.stage
TLS_KEY_STAGE=$TLS/.server.key.private-lab.stage
TLS_CERT_RECOVERY=$TLS/.server.crt.private-lab.recovery
TLS_KEY_RECOVERY=$TLS/.server.key.private-lab.recovery
SERVICE_PID_ROOT=/var/run/ls200-zoom
RUNTIME_ROOT=/run/ls200-zoom-state/current
AUTOSTART_ENABLED=$BASE/autostart-enabled
LOCK=/var/run/ls200-zoom-operation.lock

die() { printf '%s\n' "configure-private-lab: $*" >&2; exit 1; }
usage() {
    printf '%s\n' "usage: $0 --sip-peer-ip IPV4 --sip-peer-port PORT --device-ip IPV4 [--tls-cert ABSOLUTE_CERT --tls-key ABSOLUTE_KEY] [--dry-run]" >&2
    printf '%s\n' "       $0 --replace-tls --tls-cert ABSOLUTE_CERT --tls-key ABSOLUTE_KEY [--dry-run]" >&2
    exit 64
}
valid_private_unicast_ipv4() {
    printf '%s\n' "$1" | awk -F. 'NF != 4 { exit 1 } { for (i=1;i<=4;i++) if ($i !~ /^(0|[1-9][0-9]*)$/ || $i > 255) exit 1 } ($1 == 10 && ($2 != 0 || $3 != 0 || $4 != 0) && ($2 != 255 || $3 != 255 || $4 != 255)) || ($1 == 172 && $2 >= 16 && $2 <= 31 && ($2 != 16 || $3 != 0 || $4 != 0) && ($2 != 31 || $3 != 255 || $4 != 255)) || ($1 == 192 && $2 == 168 && ($3 != 0 || $4 != 0) && ($3 != 255 || $4 != 255)) { exit 0 } { exit 1 }'
}
value_once() {
    key=$1
    awk -F= -v key="$key" '
        $1 ~ "^[[:space:]]*" key "[[:space:]]*$" {
            value=$2; sub(/^[[:space:]]*/, "", value); sub(/[[:space:]]*$/, "", value); count++
        }
        END { if (count != 1 || value == "") exit 1; print value }
    ' "$CONFIG"
}
sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk 'NR == 1 { print $1 }'
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk 'NR == 1 { print $1 }'
    else die 'sha256sum or shasum is required'; fi
}
journal_entry_once() { [ "$(grep -Fxc "$1" "$JOURNAL")" -eq 1 ]; }
journal_hash_once() {
    target=$1
    digest=$(awk -v prefix="sha256:$target:" '
        index($0, prefix) == 1 { value=substr($0, length(prefix) + 1); count++ }
        END { if (count != 1 || value == "") exit 1; print value }
    ' "$JOURNAL") || die "managed file hash is absent or ambiguous: $target"
    valid_sha256 "$digest" || die "managed file hash is unsafe: $target"
    printf '%s\n' "$digest"
}
record() {
    if ! grep -Fx "$1" "$JOURNAL" >/dev/null 2>&1; then
        printf '%s\n' "$1" >> "$JOURNAL"
        sync || die 'ownership journal durability barrier failed'
    fi
}
record_sha256() {
    target=$1
    digest=$(sha256_file "$target") || die "cannot hash managed file: $target"
    case $digest in *[!0-9a-f]*|'') die "managed file hash is unsafe: $target" ;; esac
    [ "${#digest}" -eq 64 ] || die "managed file hash has an unsafe length: $target"
    temporary=$(mktemp "$BASE/.live-owned-files.XXXXXX") || die 'cannot create ownership journal temporary'
    while IFS= read -r entry; do
        case $entry in
            "sha256:$target:"*) : ;;
            *) printf '%s\n' "$entry" >> "$temporary" ;;
        esac
    done < "$JOURNAL"
    printf '%s\n' "sha256:$target:$digest" >> "$temporary"
    chown 0:0 "$temporary"
    chmod 0600 "$temporary"
    mv "$temporary" "$JOURNAL" || die 'cannot atomically record managed file hash'
    sync || die 'ownership journal hash durability barrier failed'
}
valid_sha256() {
    case $1 in *[!0-9a-f]*|'') return 1 ;; esac
    [ "${#1}" -eq 64 ]
}
require_journaled_hash() {
    target=$1
    journal_entry_once "file:$target" || die "managed file is not uniquely journaled as owned: $target"
    expected=$(journal_hash_once "$target")
    require_hash "$target" "$expected"
    printf '%s\n' "$expected"
}
require_tls_file_contract() {
    target=$1
    [ -f "$target" ] && [ ! -L "$target" ] || die "managed TLS file is absent or unsafe: $target"
    tls_gid=$(awk -F: '$1 == "ls200-web-tls" { value=$3; count++ } END { if (count != 1 || value !~ /^[0-9]+$/) exit 1; print value }' /etc/group) ||
        die 'managed TLS group is absent or ambiguous'
    metadata=$(LC_ALL=C ls -lnd "$target" | awk 'NR == 1 { print $1 ":" $3 ":" $4 }') ||
        die "cannot inspect managed TLS file: $target"
    [ "$metadata" = "-rw-r-----:0:$tls_gid" ] || die "managed TLS file mode or owner is unsafe: $target"
}
require_root_owned_0600() {
    target=$1
    [ -f "$target" ] && [ ! -L "$target" ] || die "root-owned state file is absent or unsafe: $target"
    metadata=$(LC_ALL=C ls -lnd "$target" | awk 'NR == 1 { print $1 ":" $3 ":" $4 }') ||
        die "cannot inspect root-owned state file: $target"
    [ "$metadata" = '-rw-------:0:0' ] || die "root-owned state file mode or owner is unsafe: $target"
}
require_tls_input() {
    target=$1
    [ -f "$target" ] && [ ! -L "$target" ] || die "TLS input is absent or unsafe: $target"
    canonical=$(readlink -f "$target") || die "TLS input path is unsafe: $target"
    [ "$canonical" = "$target" ] || die "TLS input path is not canonical: $target"
    [ "$(LC_ALL=C ls -lnd "$target" | awk 'NR == 1 { print $3 }')" = 0 ] || die "TLS input is not root-owned: $target"
    find "$target" -prune \( -perm -020 -o -perm -002 \) | grep -q . && die "TLS input is writable by another identity: $target"
    parent=${target%/*}
    [ -d "$parent" ] && [ ! -L "$parent" ] || die "TLS input parent is unsafe: $parent"
    [ "$(LC_ALL=C ls -lnd "$parent" | awk 'NR == 1 { print $3 }')" = 0 ] || die "TLS input parent is not root-owned: $parent"
    find "$parent" -prune \( -perm -020 -o -perm -002 \) | grep -q . && die "TLS input parent is writable by another identity: $parent"
    return 0
}
require_services_stopped() {
    [ ! -e "$AUTOSTART_ENABLED" ] && [ ! -L "$AUTOSTART_ENABLED" ] ||
        die 'autostart must be disabled through live-start.sh before TLS replacement'
    for pidfile in "$SERVICE_PID_ROOT/device.pid" "$SERVICE_PID_ROOT/sip.pid" "$SERVICE_PID_ROOT/gateway.pid" "$SERVICE_PID_ROOT/web.pid"; do
        [ ! -e "$pidfile" ] && [ ! -L "$pidfile" ] || die "managed service must be stopped before TLS replacement: $pidfile"
    done
    for process_link in /proc/[0-9]*/exe; do
        [ -e "$process_link" ] || continue
        executable=$(readlink "$process_link" 2>/dev/null || true)
        case $executable in
            "$RUNTIME_ROOT/bin/ls200-device-control"|"$RUNTIME_ROOT/bin/ls200-sipd"|"$RUNTIME_ROOT/bin/ls200-gateway-fcgi"|"$RUNTIME_ROOT/bin/nginx"|\
            /run/ls200-zoom-state/releases/*/bin/ls200-device-control|/run/ls200-zoom-state/releases/*/bin/ls200-sipd|/run/ls200-zoom-state/releases/*/bin/ls200-gateway-fcgi|/run/ls200-zoom-state/releases/*/bin/nginx|\
            "$BASE"/releases/*/bin/ls200-device-control|"$BASE"/releases/*/bin/ls200-sipd|"$BASE"/releases/*/bin/ls200-gateway-fcgi|"$BASE"/releases/*/bin/nginx)
                die "managed service executable is still running before TLS replacement: $executable"
                ;;
        esac
    done
    for socket_path in /run/ls200-sipd/control.sock /run/ls200-console/gateway.fcgi.sock; do
        [ ! -e "$socket_path" ] && [ ! -L "$socket_path" ] || die "managed runtime socket remains before TLS replacement: $socket_path"
    done
}
acquire_lock() {
    mkdir "$LOCK" 2>/dev/null || die 'another live operation is active or its lock requires reconciliation'
    printf '%s\n' "$$" > "$LOCK/pid" || { rmdir "$LOCK" 2>/dev/null || true; die 'cannot record live operation lock owner'; }
}
release_lock() { rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true; }
transaction_value_once() {
    key=$1
    awk -F= -v key="$key" '
        $1 == key { value=$2; count++ }
        END { if (count != 1 || value == "") exit 1; print value }
    ' "$PRIVATE_LAB_TX"
}
require_hash() {
    target=$1
    expected=$2
    [ -f "$target" ] && [ ! -L "$target" ] || die "private-lab transaction file is absent or unsafe: $target"
    actual=$(sha256_file "$target") || die "cannot hash private-lab transaction file: $target"
    [ "$actual" = "$expected" ] || die "private-lab transaction file changed outside the transaction: $target"
}
require_absent_or_hash() {
    target=$1
    expected=$2
    [ ! -e "$target" ] && [ ! -L "$target" ] && return
    require_hash "$target" "$expected"
}
require_staged_or_final_hash() {
    stage=$1
    target=$2
    expected=$3
    if [ -e "$stage" ] || [ -L "$stage" ]; then
        require_hash "$stage" "$expected"
    else
        require_hash "$target" "$expected"
    fi
}
write_private_lab_transaction() {
    previous=$1
    config_digest=$2
    tls_mode=$3
    cert_digest=${4:-}
    key_digest=${5:-}
    temporary=$BASE/.private-lab-transaction.$$
    [ ! -e "$temporary" ] && [ ! -L "$temporary" ] || die 'private-lab transaction temporary path already exists'
    {
        printf '%s\n' 'schema=private-lab-v1'
        printf '%s\n' 'phase=prepared'
        printf '%s\n' "config_previous_sha256=$previous"
        printf '%s\n' "config_sha256=$config_digest"
        printf '%s\n' "tls_mode=$tls_mode"
        if [ "$tls_mode" = pair ]; then
            printf '%s\n' "tls_cert_sha256=$cert_digest"
            printf '%s\n' "tls_key_sha256=$key_digest"
        fi
    } > "$temporary"
    chown 0:0 "$temporary"
    chmod 0600 "$temporary"
    mv "$temporary" "$PRIVATE_LAB_TX" || die 'cannot atomically record private-lab transaction'
    sync || die 'private-lab transaction durability barrier failed'
}
write_tls_renew_transaction() {
    temporary=$BASE/.private-lab-transaction.$$
    [ ! -e "$temporary" ] && [ ! -L "$temporary" ] || die 'private-lab transaction temporary path already exists'
    {
        printf '%s\n' 'schema=private-lab-tls-renew-v1'
        printf '%s\n' 'phase=prepared'
        printf '%s\n' "tls_cert_previous_sha256=$1"
        printf '%s\n' "tls_key_previous_sha256=$2"
        printf '%s\n' "tls_cert_sha256=$3"
        printf '%s\n' "tls_key_sha256=$4"
    } > "$temporary"
    chown 0:0 "$temporary"
    chmod 0600 "$temporary"
    mv "$temporary" "$PRIVATE_LAB_TX" || die 'cannot atomically record TLS replacement transaction'
    sync || die 'TLS replacement transaction durability barrier failed'
}
clear_private_lab_transaction() {
    rm -f "$CONFIG_STAGE" "$TLS_CERT_STAGE" "$TLS_KEY_STAGE" "$PRIVATE_LAB_TX"
    sync || die 'private-lab transaction cleanup durability barrier failed'
}
clear_tls_renew_transaction() {
    for cleanup_path in "$TLS_CERT_STAGE" "$TLS_KEY_STAGE" "$TLS_CERT_RECOVERY" "$TLS_KEY_RECOVERY"; do
        rm -f "$cleanup_path" || die "cannot remove completed TLS replacement state: $cleanup_path"
    done
    sync || die 'TLS replacement state cleanup durability barrier failed'
    rm -f "$PRIVATE_LAB_TX" || die 'cannot remove completed TLS replacement transaction'
    sync || die 'TLS replacement transaction cleanup durability barrier failed'
}

validate_private_lab_tls_transaction() {
    tls_mode=$1
    case $tls_mode in
        none)
            [ "$(wc -l < "$PRIVATE_LAB_TX" | tr -d ' ')" = 5 ] || die 'private-lab transaction contains unexpected TLS evidence'
            ;;
        pair)
            [ "$(wc -l < "$PRIVATE_LAB_TX" | tr -d ' ')" = 7 ] || die 'private-lab TLS transaction is malformed'
            cert_digest=$(transaction_value_once tls_cert_sha256) || die 'private-lab transaction TLS certificate hash is unsafe'
            key_digest=$(transaction_value_once tls_key_sha256) || die 'private-lab transaction TLS key hash is unsafe'
            valid_sha256 "$cert_digest" && valid_sha256 "$key_digest" || die 'private-lab transaction TLS hashes are unsafe'
            require_absent_or_hash "$TLS_CERT" "$cert_digest"
            require_absent_or_hash "$TLS_KEY" "$key_digest"
            require_staged_or_final_hash "$TLS_CERT_STAGE" "$TLS_CERT" "$cert_digest"
            require_staged_or_final_hash "$TLS_KEY_STAGE" "$TLS_KEY" "$key_digest"
            ;;
        *) die 'private-lab transaction TLS mode is unsafe' ;;
    esac
}

publish_private_lab_transaction() {
    tls_mode=$1
    config_digest=$2
    [ "$current_config" = "$config_digest" ] || mv "$CONFIG_STAGE" "$CONFIG"
    if [ "$tls_mode" = pair ]; then
        [ -e "$TLS_KEY" ] || [ -L "$TLS_KEY" ] || mv "$TLS_KEY_STAGE" "$TLS_KEY"
        [ -e "$TLS_CERT" ] || [ -L "$TLS_CERT" ] || mv "$TLS_CERT_STAGE" "$TLS_CERT"
    fi
    sync || die 'private-lab publication durability barrier failed'
}

validate_private_lab_config_transaction() {
    previous=$1
    config_digest=$2
    [ -f "$CONFIG" ] && [ ! -L "$CONFIG" ] || die 'installed SIP configuration is absent or unsafe during transaction recovery'
    current_config=$(sha256_file "$CONFIG") || die 'cannot hash installed SIP configuration'
    case $current_config in
        "$previous") require_hash "$CONFIG_STAGE" "$config_digest" ;;
        "$config_digest") [ ! -e "$CONFIG_STAGE" ] && [ ! -L "$CONFIG_STAGE" ] || require_hash "$CONFIG_STAGE" "$config_digest" ;;
        *) die 'installed SIP configuration changed outside the private-lab transaction' ;;
    esac
}

complete_private_lab_v1_transaction() {
    [ ! -e "$TLS_CERT_RECOVERY" ] && [ ! -L "$TLS_CERT_RECOVERY" ] && [ ! -e "$TLS_KEY_RECOVERY" ] && [ ! -L "$TLS_KEY_RECOVERY" ] ||
        die 'private-lab v1 transaction contains unexpected TLS replacement state'
    [ "$(wc -l < "$PRIVATE_LAB_TX" | tr -d ' ')" -ge 5 ] || die 'private-lab transaction is malformed'
    [ "$(transaction_value_once phase)" = prepared ] || die 'private-lab transaction phase is unsafe'
    previous=$(transaction_value_once config_previous_sha256) || die 'private-lab transaction previous config hash is unsafe'
    config_digest=$(transaction_value_once config_sha256) || die 'private-lab transaction config hash is unsafe'
    tls_mode=$(transaction_value_once tls_mode) || die 'private-lab transaction TLS mode is unsafe'
    valid_sha256 "$previous" && valid_sha256 "$config_digest" || die 'private-lab transaction config hashes are unsafe'
    validate_private_lab_config_transaction "$previous" "$config_digest"
    validate_private_lab_tls_transaction "$tls_mode"
    publish_private_lab_transaction "$tls_mode" "$config_digest"
    journal_entry_once "file:$CONFIG" || die 'installed SIP configuration is not uniquely journaled as owned'
    record_sha256 "$CONFIG"
    if [ "$tls_mode" = pair ]; then
        record "file:$TLS_KEY"
        record "file:$TLS_CERT"
        record_sha256 "$TLS_KEY"
        record_sha256 "$TLS_CERT"
    fi
    clear_private_lab_transaction
}

complete_tls_renew_transaction() {
    require_services_stopped
    [ ! -e "$CONFIG_STAGE" ] && [ ! -L "$CONFIG_STAGE" ] || die 'TLS replacement transaction contains unexpected configuration state'
    [ "$(wc -l < "$PRIVATE_LAB_TX" | tr -d ' ')" = 6 ] || die 'TLS replacement transaction is malformed'
    [ "$(transaction_value_once phase)" = prepared ] || die 'TLS replacement transaction phase is unsafe'
    old_cert_digest=$(transaction_value_once tls_cert_previous_sha256) || die 'TLS replacement previous certificate hash is unsafe'
    old_key_digest=$(transaction_value_once tls_key_previous_sha256) || die 'TLS replacement previous key hash is unsafe'
    cert_digest=$(transaction_value_once tls_cert_sha256) || die 'TLS replacement certificate hash is unsafe'
    key_digest=$(transaction_value_once tls_key_sha256) || die 'TLS replacement key hash is unsafe'
    for digest in "$old_cert_digest" "$old_key_digest" "$cert_digest" "$key_digest"; do
        valid_sha256 "$digest" || die 'TLS replacement transaction contains an unsafe hash'
    done
    require_journaled_hash "$CONFIG" >/dev/null
    validate_tls_member certificate "$TLS_CERT" "$old_cert_digest" "$cert_digest"
    cert_state=$tls_member_state journal_cert_digest=$tls_member_journal
    validate_tls_member key "$TLS_KEY" "$old_key_digest" "$key_digest"
    key_state=$tls_member_state journal_key_digest=$tls_member_journal
    if [ "$cert_state:$key_state:$journal_cert_digest:$journal_key_digest" = "replacement:replacement:$cert_digest:$key_digest" ]; then
        require_absent_or_hash "$TLS_CERT_RECOVERY" "$old_cert_digest"
        require_absent_or_hash "$TLS_KEY_RECOVERY" "$old_key_digest"
    else
        require_hash "$TLS_CERT_RECOVERY" "$old_cert_digest"
        require_hash "$TLS_KEY_RECOVERY" "$old_key_digest"
    fi
    case $cert_state in previous) require_hash "$TLS_CERT_STAGE" "$cert_digest" ;; replacement) require_absent_or_hash "$TLS_CERT_STAGE" "$cert_digest" ;; esac
    case $key_state in previous) require_hash "$TLS_KEY_STAGE" "$key_digest" ;; replacement) require_absent_or_hash "$TLS_KEY_STAGE" "$key_digest" ;; esac
    [ "$key_state" = replacement ] || mv "$TLS_KEY_STAGE" "$TLS_KEY"
    [ "$cert_state" = replacement ] || mv "$TLS_CERT_STAGE" "$TLS_CERT"
    sync || die 'TLS replacement publication durability barrier failed'
    require_hash "$TLS_CERT" "$cert_digest"
    require_hash "$TLS_KEY" "$key_digest"
    record_sha256 "$TLS_KEY"
    record_sha256 "$TLS_CERT"
    [ "$(journal_hash_once "$TLS_CERT")" = "$cert_digest" ] || die 'certificate replacement hash was not journaled'
    [ "$(journal_hash_once "$TLS_KEY")" = "$key_digest" ] || die 'key replacement hash was not journaled'
    clear_tls_renew_transaction
}

validate_tls_member() {
    label=$1 target=$2 previous=$3 replacement=$4
    require_tls_file_contract "$target"
    journal_entry_once "file:$target" || die "managed TLS $label is not uniquely journaled as owned"
    actual=$(sha256_file "$target") || die "cannot hash managed TLS file: $target"
    case $actual in "$previous") tls_member_state=previous ;; "$replacement") tls_member_state=replacement ;;
        *) die "managed TLS file changed outside the replacement transaction: $target" ;; esac
    tls_member_journal=$(journal_hash_once "$target")
    case $tls_member_state:$tls_member_journal in
        previous:"$previous"|replacement:"$previous"|replacement:"$replacement") ;;
        *) die "$label and ownership journal are inconsistent during TLS replacement" ;;
    esac
}

complete_private_lab_transaction() {
    require_root_owned_0600 "$PRIVATE_LAB_TX"
    schema=$(transaction_value_once schema) || die 'private-lab transaction schema is unsafe'
    case $schema in
        private-lab-v1) complete_private_lab_v1_transaction ;;
        private-lab-tls-renew-v1) complete_tls_renew_transaction ;;
        *) die 'private-lab transaction schema is unsafe' ;;
    esac
}

peer_ip= peer_port= device_ip= tls_cert= tls_key= dry_run=0 replace_tls=0
while [ "$#" -gt 0 ]; do
    case $1 in
        --sip-peer-ip) [ "$#" -ge 2 ] || usage; peer_ip=$2; shift 2 ;;
        --sip-peer-port) [ "$#" -ge 2 ] || usage; peer_port=$2; shift 2 ;;
        --device-ip) [ "$#" -ge 2 ] || usage; device_ip=$2; shift 2 ;;
        --tls-cert) [ "$#" -ge 2 ] || usage; tls_cert=$2; shift 2 ;;
        --tls-key) [ "$#" -ge 2 ] || usage; tls_key=$2; shift 2 ;;
        --replace-tls) replace_tls=1; shift ;;
        --dry-run) dry_run=1; shift ;;
        *) usage ;;
    esac
done
case $tls_cert:$tls_key in :|/*:/*) ;; *) die 'TLS certificate and key must be supplied together as absolute paths' ;; esac
if [ "$replace_tls" = 1 ]; then
    [ -z "$peer_ip$peer_port$device_ip" ] || die 'TLS replacement does not accept SIP or device configuration arguments'
else
    valid_private_unicast_ipv4 "$peer_ip" || die 'SIP peer must be a private unicast IPv4 literal'
    valid_private_unicast_ipv4 "$device_ip" || die 'LS-200 address must be a private unicast IPv4 literal'
    [ "$peer_ip" != "$device_ip" ] || die 'SIP peer and LS-200 addresses must be different'
    case $peer_port in ''|*[!0-9]*) die 'SIP peer port must be numeric' ;; esac
    [ "$peer_port" -ge 1 ] && [ "$peer_port" -le 65535 ] || die 'SIP peer port must be between 1 and 65535'
fi

if [ "$dry_run" = 1 ]; then
    if [ "$replace_tls" = 1 ]; then
        [ -n "$tls_cert" ] || die 'TLS replacement requires a certificate and key pair'
        printf '%s\n' 'configure-private-lab dry-run: replace managed TLS certificate and key while services are stopped'
    else
        printf '%s\n' "configure-private-lab dry-run: peer=$peer_ip:$peer_port device=$device_ip rtsp=rtsp://127.0.0.1/movie"
    fi
    exit 0
fi
[ "$(id -u)" = 0 ] || die 'must run as root on the target'
[ -f "$CONFIG" ] && [ ! -L "$CONFIG" ] || die 'installed SIP configuration is absent or unsafe'
require_root_owned_0600 "$JOURNAL"
journal_entry_once "file:$CONFIG" || die 'installed SIP configuration is not uniquely journaled as owned'
acquire_lock
trap release_lock 0
trap 'exit 1' 1 2 15
if [ -e "$PRIVATE_LAB_TX" ] || [ -L "$PRIVATE_LAB_TX" ]; then
    complete_private_lab_transaction
    printf '%s\n' 'configure-private-lab: recovered and completed the interrupted private-lab transaction'
    exit 0
fi
[ ! -e "$CONFIG_STAGE" ] && [ ! -L "$CONFIG_STAGE" ] && [ ! -e "$TLS_CERT_STAGE" ] && [ ! -L "$TLS_CERT_STAGE" ] && [ ! -e "$TLS_KEY_STAGE" ] && [ ! -L "$TLS_KEY_STAGE" ] && [ ! -e "$TLS_CERT_RECOVERY" ] && [ ! -L "$TLS_CERT_RECOVERY" ] && [ ! -e "$TLS_KEY_RECOVERY" ] && [ ! -L "$TLS_KEY_RECOVERY" ] ||
    die 'private-lab staging state exists without a recoverable transaction'
if [ "$replace_tls" = 1 ]; then
    [ -n "$tls_cert" ] || die 'TLS replacement requires a certificate and key pair'
    require_tls_input "$tls_cert"
    require_tls_input "$tls_key"
    [ ! "$tls_cert" -ef "$tls_key" ] || die 'TLS certificate and key inputs must be distinct files'
    [ -d "$TLS" ] && [ ! -L "$TLS" ] || die 'managed TLS directory is absent or unsafe'
    require_services_stopped
    require_journaled_hash "$CONFIG" >/dev/null
    require_tls_file_contract "$TLS_CERT"
    require_tls_file_contract "$TLS_KEY"
    old_cert_digest=$(require_journaled_hash "$TLS_CERT")
    old_key_digest=$(require_journaled_hash "$TLS_KEY")
    cp "$TLS_CERT" "$TLS_CERT_RECOVERY"
    cp "$TLS_KEY" "$TLS_KEY_RECOVERY"
    cp "$tls_cert" "$TLS_CERT_STAGE"
    cp "$tls_key" "$TLS_KEY_STAGE"
    chown root:ls200-web-tls "$TLS_CERT_RECOVERY" "$TLS_KEY_RECOVERY" "$TLS_CERT_STAGE" "$TLS_KEY_STAGE"
    chmod 0640 "$TLS_CERT_RECOVERY" "$TLS_KEY_RECOVERY" "$TLS_CERT_STAGE" "$TLS_KEY_STAGE"
    require_tls_file_contract "$TLS_CERT_RECOVERY"
    require_tls_file_contract "$TLS_KEY_RECOVERY"
    require_tls_file_contract "$TLS_CERT_STAGE"
    require_tls_file_contract "$TLS_KEY_STAGE"
    cert_digest=$(sha256_file "$TLS_CERT_STAGE") || die 'cannot hash staged TLS certificate'
    key_digest=$(sha256_file "$TLS_KEY_STAGE") || die 'cannot hash staged TLS key'
    valid_sha256 "$cert_digest" && valid_sha256 "$key_digest" || die 'TLS replacement hash is unsafe'
    [ "$cert_digest:$key_digest" != "$old_cert_digest:$old_key_digest" ] || die 'TLS replacement pair is unchanged'
    sync || die 'TLS replacement staging durability barrier failed'
    write_tls_renew_transaction "$old_cert_digest" "$old_key_digest" "$cert_digest" "$key_digest"
    complete_private_lab_transaction
    printf '%s\n' 'configure-private-lab: managed TLS certificate and key replaced while services remained stopped'
    exit 0
fi
gateway_uid=$(value_once gateway_uid) || die 'installed gateway UID is absent or ambiguous'
settings_file=$(value_once settings_file) || die 'installed runtime settings path is absent or ambiguous'
case $gateway_uid in ''|*[!0-9]*) die 'installed gateway UID is unsafe' ;; esac
case $settings_file in /run/ls200-zoom-state/sip/settings.json) ;; *) die 'installed runtime settings path is not the managed path' ;; esac
if [ -n "$tls_cert" ]; then
    [ -f "$tls_cert" ] && [ ! -L "$tls_cert" ] && [ -f "$tls_key" ] && [ ! -L "$tls_key" ] ||
        die 'TLS inputs must be regular non-symlink files'
    [ -d "$TLS" ] && [ ! -L "$TLS" ] || die 'managed TLS directory is absent or unsafe'
    [ ! -e "$TLS_CERT" ] && [ ! -L "$TLS_CERT" ] && [ ! -e "$TLS_KEY" ] && [ ! -L "$TLS_KEY" ] ||
        die 'managed TLS material already exists'
fi

cat > "$CONFIG_STAGE" <<EOF
[runtime]
foreground = true
pid_file = /run/ls200-sipd/daemon.pid
settings_file = $settings_file
log_sink = stderr
log_level = warning
log_summary_interval_seconds = 60

[sip]
profile = private_lab
uri = sip:room@$peer_ip:$peer_port
registrar_uri =
transport = udp
auth_username =
auth_secret_file =
tls_ca_file =
enable_tls = false
enable_public_network = true
transaction_timeout_ms = 32000
max_retransmissions = 6
max_reconnect_attempts = 3

[media]
backend = rtsp_native
video_source = rtsp://127.0.0.1/movie
audio_source =
authorized_rtsp_ipv4 = 127.0.0.1
bind_address = $device_ip
advertised_address = $device_ip
preferred_audio_codec = pcmu
security = plain_compat
rtsp_use_udp = true
rtsp_jitter_max_ms = 120
rtsp_reconnect_limit = 3
local_rtp_port_min = 40000
local_rtp_port_max = 40100
rtp_mtu = 1200
video_queue_frames = 8
audio_queue_frames = 10
child_uid = 65534
child_gid = 65534

[control]
enable_local_control = true
unix_socket_path = /run/ls200-sipd/control.sock
unix_socket_mode = 0660
gateway_uid = $gateway_uid

[limits]
sip_message_bytes = 16384
sdp_bytes = 8192
rtp_packet_bytes = 1500
video_access_unit_bytes = 2097152
audio_frame_bytes = 4096
control_message_bytes = 2048
log_events_per_interval = 120
EOF
chown ls200-sip "$CONFIG_STAGE"
chmod 0600 "$CONFIG_STAGE"
previous_config=$(sha256_file "$CONFIG") || die 'cannot hash installed SIP configuration before staging'
config_digest=$(sha256_file "$CONFIG_STAGE") || die 'cannot hash staged SIP configuration'
valid_sha256 "$previous_config" && valid_sha256 "$config_digest" || die 'private-lab SIP configuration hash is unsafe'
if [ -n "$tls_cert" ]; then
    cp "$tls_cert" "$TLS_CERT_STAGE"
    cp "$tls_key" "$TLS_KEY_STAGE"
    chown root:ls200-web-tls "$TLS_CERT_STAGE" "$TLS_KEY_STAGE"
    chmod 0640 "$TLS_CERT_STAGE" "$TLS_KEY_STAGE"
    cert_digest=$(sha256_file "$TLS_CERT_STAGE") || die 'cannot hash staged TLS certificate'
    key_digest=$(sha256_file "$TLS_KEY_STAGE") || die 'cannot hash staged TLS key'
    valid_sha256 "$cert_digest" && valid_sha256 "$key_digest" || die 'private-lab TLS hash is unsafe'
    sync || die 'private-lab staged TLS durability barrier failed'
    write_private_lab_transaction "$previous_config" "$config_digest" pair "$cert_digest" "$key_digest"
else
    sync || die 'private-lab staged configuration durability barrier failed'
    write_private_lab_transaction "$previous_config" "$config_digest" none
fi
complete_private_lab_transaction
printf '%s\n' 'configure-private-lab: numeric private-lab configuration and supplied ephemeral TLS material installed'
