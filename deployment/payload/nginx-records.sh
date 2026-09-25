# shellcheck shell=sh
# Strict nginx ownership records shared by the installer and remover.

nginx_hash_path() {
    printf '%s.sha256\n' "$(nginx_copy_path "$1")"
}

read_nginx_copy_hashes() {
    hashes=$(nginx_hash_path "$1")
    require_regular_file "$hashes"
    awk -F: '
        $1 == "original" || $1 == "patched" {
            if (seen[$1]++ || NF != 2 || length($2) != 64 || $2 ~ /[^0-9a-f]/) exit 1
            value[$1] = $2
            next
        }
        { exit 1 }
        END {
            if (NR != 2 || !("original" in value) || !("patched" in value)) exit 1
            print value["original"], value["patched"]
        }
    ' "$hashes" || die "nginx copy hash record is invalid: $hashes"
}

read_selector_backup() {
    require_regular_file "$NGINX_SELECTOR_BACKUP"
    awk -F: '
        $1 == "target" || $1 == "mode" {
            if (seen[$1]++ || NF != 2 || $2 == "") exit 1
            value[$1] = $2
            next
        }
        { exit 1 }
        END {
            if (NR != 2 || !("target" in value) || !("mode" in value) ||
                (value["mode"] != "http" && value["mode"] != "https")) exit 1
            print value["target"], value["mode"]
        }
    ' "$NGINX_SELECTOR_BACKUP" || die "nginx selector backup is invalid: $NGINX_SELECTOR_BACKUP"
}


nginx_copy_path() {
    case $1 in
        http) printf '%s\n' "$NGINX_COPY_HTTP" ;;
        https) printf '%s\n' "$NGINX_COPY_HTTPS" ;;
        *) die "unsupported nginx mode: $1" ;;
    esac
}

nginx_owned_target() {
    case $1 in
        http) printf '%s\n' "$NGINX_OWNED_HTTP_TARGET" ;;
        https) printf '%s\n' "$NGINX_OWNED_HTTPS_TARGET" ;;
        *) die "unsupported nginx mode: $1" ;;
    esac
}
