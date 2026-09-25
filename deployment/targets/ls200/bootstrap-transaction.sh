# shellcheck shell=sh
# Recover one generated gateway configuration and its exact first-use token.

BOOTSTRAP_TX=$BOOTSTRAP_ROOT/bootstrap-transaction
BOOTSTRAP_CONFIG_STAGE=$BOOTSTRAP_ROOT/.gateway.conf.bootstrap
BOOTSTRAP_TOKEN_STAGE=$BOOTSTRAP_ROOT/.bootstrap-token.bootstrap

bootstrap_checkpoint() { :; }
bootstrap_value_once() {
    awk -F: -v key="$1" '
        $1 == key { count++; value=substr($0, length(key) + 2) }
        END { if (count == 1) print value; else exit 1 }
    ' "$BOOTSTRAP_TX"
}
bootstrap_require_file() {
    file=$1 owner=$2
    [ -f "$file" ] && [ ! -L "$file" ] || die "bootstrap state is absent or unsafe: $file"
    [ "$(ls -nd "$file" | awk 'NR == 1 { print $3 }')" = "$owner" ] ||
        die "bootstrap state has an unsafe owner: $file"
    find "$file" -prune \( -perm -020 -o -perm -002 \) | grep -q . &&
        die "bootstrap state is writable by others: $file"
    return 0
}
bootstrap_require_stage() {
    [ -f "$1" ] && [ ! -L "$1" ] || die "bootstrap staging state is unsafe: $1"
    find "$1" -prune \( -perm -020 -o -perm -002 \) | grep -q . &&
        die "bootstrap staging state is writable by others: $1"
    return 0
}
bootstrap_read_transaction() {
    bootstrap_require_file "$BOOTSTRAP_TX" "$BOOTSTRAP_ROOT_UID"
    [ "$(wc -l < "$BOOTSTRAP_TX" | tr -d ' ')" = 5 ] || die 'bootstrap transaction is malformed'
    [ "$(grep -Fxc bootstrap-v1 "$BOOTSTRAP_TX")" = 1 ] || die 'bootstrap transaction schema is unsafe'
    bootstrap_version=$(bootstrap_value_once version) || die 'bootstrap transaction version is unsafe'
    bootstrap_manifest=$(bootstrap_value_once manifest_sha256) || die 'bootstrap transaction manifest is unsafe'
    bootstrap_sip_uid=$(bootstrap_value_once expected_sipd_uid) || die 'bootstrap transaction SIP UID is unsafe'
    bootstrap_code=$(bootstrap_value_once bootstrap_code) || die 'bootstrap transaction token is unsafe'
    case $bootstrap_version in ''|.|..|*[!A-Za-z0-9._-]*) die 'bootstrap transaction version is unsafe' ;; esac
    case $bootstrap_manifest in ''|*[!0-9a-f]*) die 'bootstrap transaction manifest is unsafe' ;; esac
    [ "${#bootstrap_manifest}" = 64 ] || die 'bootstrap transaction manifest is unsafe'
    case $bootstrap_sip_uid in ''|*[!0-9]*) die 'bootstrap transaction SIP UID is unsafe' ;; esac
    case $bootstrap_code in *[!0-9a-f]*|'') die 'bootstrap transaction token is unsafe' ;; esac
    [ "${#bootstrap_code}" = 32 ] || die 'bootstrap transaction token is unsafe'
    bootstrap_release=$BOOTSTRAP_RELEASES/$bootstrap_version
    bootstrap_release_identity "$bootstrap_version" "$bootstrap_manifest"
}
write_bootstrap_transaction() {
    [ ! -e "$BOOTSTRAP_TX" ] && [ ! -L "$BOOTSTRAP_TX" ] || die 'bootstrap transaction already exists'
    bootstrap_temporary=$(mktemp "$BOOTSTRAP_ROOT/.bootstrap-transaction.XXXXXX") ||
        die 'cannot create bootstrap transaction temporary'
    {
        printf '%s\n' bootstrap-v1
        printf 'version:%s\n' "$1"
        printf 'manifest_sha256:%s\n' "$2"
        printf 'expected_sipd_uid:%s\n' "$3"
        printf 'bootstrap_code:%s\n' "$4"
    } > "$bootstrap_temporary"
    chown "$BOOTSTRAP_ROOT_UID:$BOOTSTRAP_ROOT_GID" "$bootstrap_temporary"
    chmod 0600 "$bootstrap_temporary"
    mv "$bootstrap_temporary" "$BOOTSTRAP_TX" || die 'cannot publish bootstrap transaction'
    durability_barrier
}
bootstrap_prepare_stage() {
    stage=$1
    if [ -e "$stage" ] || [ -L "$stage" ]; then
        bootstrap_require_stage "$stage"
        : > "$stage" || die "cannot refresh bootstrap staging state: $stage"
    else
        (set -C; : > "$stage") 2>/dev/null || die "cannot create bootstrap staging state: $stage"
    fi
}
bootstrap_prepare_stages() {
    bootstrap_prepare_stage "$BOOTSTRAP_CONFIG_STAGE"
    sed -e "s/\"expected_sipd_uid\": [0-9][0-9]*/\"expected_sipd_uid\": $bootstrap_sip_uid/" \
        -e 's/^\([[:space:]]*"preview_rtsp_port":[[:space:]]*[0-9][0-9]*\)$/\1,/' -e '$d' \
        "$bootstrap_release/etc/ls200-zoom/gateway.conf.example" > "$BOOTSTRAP_CONFIG_STAGE"
    printf '  "bootstrap_code": "%s"\n}\n' "$bootstrap_code" >> "$BOOTSTRAP_CONFIG_STAGE"
    chown "$BOOTSTRAP_GATEWAY_OWNER" "$BOOTSTRAP_CONFIG_STAGE"; chmod 0600 "$BOOTSTRAP_CONFIG_STAGE"
    bootstrap_require_file "$BOOTSTRAP_CONFIG_STAGE" "$bootstrap_gateway_uid"
    bootstrap_prepare_stage "$BOOTSTRAP_TOKEN_STAGE"
    printf '%s\n' "$bootstrap_code" > "$BOOTSTRAP_TOKEN_STAGE"
    chown "$BOOTSTRAP_ROOT_UID:$BOOTSTRAP_ROOT_GID" "$BOOTSTRAP_TOKEN_STAGE"
    chmod 0600 "$BOOTSTRAP_TOKEN_STAGE"
    bootstrap_require_file "$BOOTSTRAP_TOKEN_STAGE" "$BOOTSTRAP_ROOT_UID"
}
bootstrap_inspect_publication() {
    bootstrap_config_present=0 bootstrap_token_present=0
    if [ -e "$BOOTSTRAP_ROOT/gateway.conf" ] || [ -L "$BOOTSTRAP_ROOT/gateway.conf" ]; then
        bootstrap_require_file "$BOOTSTRAP_ROOT/gateway.conf" "$bootstrap_gateway_uid"
        bootstrap_config_present=1
    fi
    if [ -e "$BOOTSTRAP_ROOT/bootstrap-token" ] || [ -L "$BOOTSTRAP_ROOT/bootstrap-token" ]; then
        bootstrap_require_file "$BOOTSTRAP_ROOT/bootstrap-token" "$BOOTSTRAP_ROOT_UID"
        bootstrap_token_present=1
    fi
    [ "$bootstrap_config_present:$bootstrap_token_present" != 0:1 ] ||
        die 'bootstrap transaction published its token before its configuration'
}
bootstrap_validate_journal() {
    [ -n "$BOOTSTRAP_JOURNAL" ] || return 0
    bootstrap_config_record=$(grep -Fxc "file:$BOOTSTRAP_ROOT/gateway.conf" "$BOOTSTRAP_JOURNAL" || true)
    bootstrap_token_record=$(grep -Fxc "file:$BOOTSTRAP_ROOT/bootstrap-token" "$BOOTSTRAP_JOURNAL" || true)
    case $bootstrap_config_record:$bootstrap_token_record in 0:0|1:0|1:1) ;; *) die 'bootstrap ownership journal is ambiguous' ;; esac
    if [ "$bootstrap_token_present" = 0 ] && [ "$bootstrap_config_record:$bootstrap_token_record" != 0:0 ]; then
        die 'bootstrap ownership journal is ahead of published state'
    fi
}
bootstrap_publish_file() {
    stage=$1 destination=$2 present=$3 label=$4 checkpoint=$5
    if [ "$present" = 1 ]; then
        cmp -s "$stage" "$destination" || die "$label changed outside the bootstrap transaction"
        rm -f "$stage"
    else
        mv "$stage" "$destination" || die "cannot publish $label"
        durability_barrier
        bootstrap_checkpoint "$checkpoint"
    fi
}
bootstrap_reconcile_journal() {
    [ -n "$BOOTSTRAP_JOURNAL" ] || return 0
    if [ "$bootstrap_config_record" != 1 ]; then
        bootstrap_record "file:$BOOTSTRAP_ROOT/gateway.conf"
        bootstrap_checkpoint config-journaled
    fi
    if [ "$bootstrap_token_record" != 1 ]; then
        bootstrap_record "file:$BOOTSTRAP_ROOT/bootstrap-token"
        bootstrap_checkpoint token-journaled
    fi
    [ "$(grep -Fxc "file:$BOOTSTRAP_ROOT/gateway.conf" "$BOOTSTRAP_JOURNAL")" = 1 ] &&
        [ "$(grep -Fxc "file:$BOOTSTRAP_ROOT/bootstrap-token" "$BOOTSTRAP_JOURNAL")" = 1 ] ||
        die 'bootstrap ownership journal is incomplete'
}
complete_bootstrap_transaction() {
    bootstrap_read_transaction
    bootstrap_gateway_uid=$(bootstrap_user_id ls200-gateway) || die 'cannot resolve gateway service UID during bootstrap recovery'
    [ "$(bootstrap_user_id ls200-sip)" = "$bootstrap_sip_uid" ] || die 'bootstrap transaction SIP UID changed'
    bootstrap_inspect_publication
    bootstrap_validate_journal
    bootstrap_prepare_stages
    bootstrap_publish_file "$BOOTSTRAP_CONFIG_STAGE" "$BOOTSTRAP_ROOT/gateway.conf" \
        "$bootstrap_config_present" 'gateway configuration' config-published
    bootstrap_publish_file "$BOOTSTRAP_TOKEN_STAGE" "$BOOTSTRAP_ROOT/bootstrap-token" \
        "$bootstrap_token_present" 'bootstrap token' token-published
    bootstrap_reconcile_journal
    rm -f "$BOOTSTRAP_TX"; durability_barrier
    unset bootstrap_code
}
reconcile_bootstrap_transaction() {
    if [ -e "$BOOTSTRAP_TX" ] || [ -L "$BOOTSTRAP_TX" ]; then
        complete_bootstrap_transaction
    elif [ -e "$BOOTSTRAP_CONFIG_STAGE" ] || [ -L "$BOOTSTRAP_CONFIG_STAGE" ] ||
         [ -e "$BOOTSTRAP_TOKEN_STAGE" ] || [ -L "$BOOTSTRAP_TOKEN_STAGE" ]; then
        die 'bootstrap staging state exists without a recoverable transaction'
    fi
}
