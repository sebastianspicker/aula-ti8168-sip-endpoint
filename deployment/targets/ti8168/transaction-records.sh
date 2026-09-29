# shellcheck shell=sh
# Transaction records; callers retain ownership checks and recovery policy.

release_manifest_digest() {
    trust_version=$1
    case $trust_version in ''|.|..|*[!A-Za-z0-9._-]*) die 'release trust version is unsafe' ;; esac
    [ "$(grep -Fxc "release:$trust_version" "$JOURNAL")" = 1 ] || die 'release is not uniquely owned'
    trust_digest=$(awk -F: -v version="$trust_version" '
        $1 == "release-sha256" && $2 == version { count++; digest=$3; if (NF != 3) bad=1 }
        END { if (count != 1 || bad) exit 1; print digest }
    ' "$JOURNAL") || die 'independent release digest is absent or ambiguous'
    case $trust_digest in ''|*[!0-9a-f]*) die 'release digest is unsafe' ;; esac
    [ "${#trust_digest}" = 64 ] || die 'release digest is not SHA-256'
    printf '%s\n' "$trust_digest"
}

validate_sha256_entry() {
    digest=${1##*:}
    case $digest in *[!0-9a-f]*|'') die "unsafe SHA-256 journal entry: $1" ;; esac
    [ "${#digest}" -eq 64 ] || die "unsafe SHA-256 journal entry: $1"
}
validate_release_digest_entry() {
    digest_version=${1#release-sha256:}; digest_version=${digest_version%:*}
    [ "$(release_manifest_digest "$digest_version")" = "${1##*:}" ] || die 'release digest journal mismatch'
}

verify_installed_release() {
    trust_version=$1
    trust_digest=$(release_manifest_digest "$trust_version") || return 1
    trust_verifier=$BASE/live-verify-payload.sh
    [ "$(grep -Fxc "file:$trust_verifier" "$JOURNAL")" = 1 ] || die 'trusted verifier is not uniquely owned'
    [ -f "$trust_verifier" ] && [ ! -L "$trust_verifier" ] || die 'trusted verifier is absent or unsafe'
    [ "$(LC_ALL=C ls -lnd "$trust_verifier" | awk 'NR == 1 {print $3 ":" $4}')" = 0:0 ] || die 'trusted verifier must be root-owned'
    find "$trust_verifier" -prune \( -perm -020 -o -perm -002 \) | grep -q . && die 'trusted verifier is writable by others'
    sh "$trust_verifier" --payload "$BASE/releases/$trust_version" \
        --manifest-sha256 "$trust_digest" >/dev/null || die 'installed release verification failed'
}

write_autostart_tx() {
    temporary=$(mktemp "$BASE/.autostart-transaction.XXXXXX") || die 'cannot create autostart transaction temporary'
    printf '%s\n' "$1" > "$temporary"; chown 0:0 "$temporary"; chmod 0600 "$temporary"
    mv "$temporary" "$AUTOSTART_TX" || die 'cannot atomically record autostart transaction'
    durability_barrier
}

read_rollback_values() {
    awk '
        $0 == "rollback-v1" { header++; next }
        /^old:[A-Za-z0-9._-]+$/ { old++; old_value=substr($0, 5); next }
        /^new:[A-Za-z0-9._-]+$/ { new++; new_value=substr($0, 5); next }
        /^phase:(prepared|applied|healthy)$/ { phase++; phase_value=substr($0, 7); next }
        { exit 1 }
        END { if (header == 1 && old == 1 && new == 1 && phase == 1) print old_value, new_value, phase_value; else exit 1 }
    ' "$ROLLBACK_TX"
}

reconcile_pending_autostart() {
    case $phase in enable-v1:prepared|enable-v1:applied) ;; *) die 'autostart transaction is unsafe' ;; esac
    if [ -e "$AUTOSTART_ENABLED" ] || [ -L "$AUTOSTART_ENABLED" ]; then
        "$1"
        record "file:$AUTOSTART_ENABLED"
    elif [ "$phase" = enable-v1:applied ]; then
        die 'autostart transaction lost its enabled marker'
    fi
    clear_transaction "$AUTOSTART_TX"
}

require_secure_autostart_marker() {
    require_secure_file "$AUTOSTART_ENABLED"
    [ "$(cat "$AUTOSTART_ENABLED")" = enabled-v1 ] || die 'autostart marker is unsafe'
}
