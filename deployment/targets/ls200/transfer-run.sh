#!/bin/sh
# macOS operator helper.  It has one fixed target and never stores credentials.
set -eu
umask 077

TARGET=10.230.61.171
SCRIPT_DIR=$(CDPATH='' cd "$(dirname "$0")" && pwd)
die() { printf '%s\n' "live-transfer: $*" >&2; exit 1; }
usage() { printf '%s\n' "usage: $0 --payload ABSOLUTE_PATH --manifest-sha256 SHA256 --version VERSION --user OPERATOR --interface IFACE --expected-mac MAC --host-key SHA256:FINGERPRINT [--port 5100] [--start] [--apply]" >&2; exit 64; }
normalize_mac() { printf '%s' "$1" | tr 'A-F:-' 'a-f  ' | tr -cd '0-9a-f'; }

payload= version= user= interface= expected_mac= host_key= manifest_sha= port=5100 start=0 apply=0
while [ "$#" -gt 0 ]; do
    case $1 in
        --payload) [ "$#" -ge 2 ] || usage; payload=$2; shift 2 ;;
        --manifest-sha256) [ "$#" -ge 2 ] || usage; manifest_sha=$2; shift 2 ;;
        --version) [ "$#" -ge 2 ] || usage; version=$2; shift 2 ;;
        --user) [ "$#" -ge 2 ] || usage; user=$2; shift 2 ;;
        --interface) [ "$#" -ge 2 ] || usage; interface=$2; shift 2 ;;
        --expected-mac) [ "$#" -ge 2 ] || usage; expected_mac=$(normalize_mac "$2"); shift 2 ;;
        --host-key) [ "$#" -ge 2 ] || usage; host_key=$2; shift 2 ;;
        --port) [ "$#" -ge 2 ] || usage; port=$2; shift 2 ;;
        --start) start=1; shift ;;
        --apply) apply=1; shift ;;
        *) usage ;;
    esac
done
case $payload in /*) ;; *) die 'payload must be an absolute path' ;; esac
case $version in ''|.|..|*[!A-Za-z0-9._-]*) die 'version is unsafe' ;; esac
case $manifest_sha in ''|*[!0-9a-f]*) die 'independent manifest digest is required' ;; esac
[ "${#manifest_sha}" -eq 64 ] || die 'manifest digest must be SHA-256'
case $user in ''|*[!A-Za-z0-9_.-]*|[!A-Za-z_]* ) die 'operator username is unsafe' ;; esac
case $port in ''|*[!0-9]*) die 'SSH port must be numeric' ;; esac
[ "$port" -ge 1 ] && [ "$port" -le 65535 ] || die 'SSH port must be between 1 and 65535'
case $expected_mac in [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;; *) die 'expected MAC must have 12 hexadecimal digits' ;; esac
case $host_key in SHA256:*) host_key_value=${host_key#SHA256:} ;; *) die 'host key must be an SSH SHA256 fingerprint' ;; esac
case $host_key_value in ''|*[!A-Za-z0-9+/=]*) die 'host key must be an SSH SHA256 fingerprint' ;; esac
[ -d "$payload" ] && [ ! -L "$payload" ] || die 'payload is not a safe directory'
[ -f "$payload/payload-manifest.tsv" ] && [ ! -L "$payload/payload-manifest.tsv" ] || die 'payload manifest is absent or unsafe'
[ -x "$SCRIPT_DIR/install.sh" ] && [ -x "$SCRIPT_DIR/start.sh" ] && [ -x "$SCRIPT_DIR/remove.sh" ] &&
    [ -f "$SCRIPT_DIR/bootstrap-transaction.sh" ] && [ ! -L "$SCRIPT_DIR/bootstrap-transaction.sh" ] ||
    die 'live profile scripts must be executable and the bootstrap helper must be a regular file'
payload_name=$(basename "$payload")
sh "$SCRIPT_DIR/verify-payload.sh" --payload "$payload" --manifest-sha256 "$manifest_sha" >/dev/null || die 'trusted payload verification failed'
case $payload_name in ''|*[!A-Za-z0-9._-]*) die 'payload directory basename is unsafe for remote transfer' ;; esac

if [ "$apply" = 0 ]; then
    printf '%s\n' "live-transfer dry-run: exact target=$TARGET user=$user port=$port interface=$interface payload=$payload version=$version start=$start"
    printf '%s\n' 'live-transfer dry-run: no network or device contact occurs; --apply performs one ARP, one SSH host-key scan, then one authenticated SSH transfer'
    exit 0
fi
[ "$(uname -s)" = Darwin ] || die 'this transfer helper supports macOS only'
route_interface=$(route -n get "$TARGET" | awk '/interface:/{ print $2; exit }')
[ "$route_interface" = "$interface" ] || die "target route is not pinned to requested interface $interface"
ping -c 1 -W 1000 "$TARGET" >/dev/null 2>&1 || die 'target did not answer the single exact-target ARP/ping gate'
neighbor=$(arp -an "$TARGET" | awk '/ at / { print $4; exit }')
[ "$(normalize_mac "$neighbor")" = "$expected_mac" ] || die 'neighbor MAC does not match the authorized LS-200'

scan=$(mktemp -t ls200-live-ecdsa-scan.XXXXXX) || die 'cannot create temporary SSH scan file'
known=$(mktemp -t ls200-live-known-hosts.XXXXXX) || { rm -f "$scan"; die 'cannot create temporary known-hosts file'; }
trap 'rm -f "$scan" "$known"' EXIT HUP INT TERM
ssh-keyscan -p "$port" -T 5 -t ecdsa "$TARGET" > "$scan" 2>/dev/null || die 'cannot obtain current ECDSA SSH host key'
matches=0
while IFS= read -r scanned_line; do
    case $scanned_line in ''|'#'*) continue ;; esac
    scanned_fingerprint=$(printf '%s\n' "$scanned_line" | ssh-keygen -lf - -E sha256 2>/dev/null | awk 'NR == 1 { print $2 }')
    if [ "$scanned_fingerprint" = "$host_key" ]; then
        printf '%s\n' "$scanned_line" >> "$known"
        matches=$((matches + 1))
    fi
done < "$scan"
[ "$matches" -eq 1 ] || die 'current ECDSA SSH scan has zero or multiple approved fingerprint lines'
chmod 0600 "$known"

# SSH reads any password from the controlling terminal.  No password is accepted
# by this script, placed in an argument, environment variable, temporary file, or log.
start_arg=
[ "$start" = 0 ] || start_arg=' --start'
tar -C "$(dirname "$payload")" -cf - "$payload_name" \
    -C "$SCRIPT_DIR/.." ls200 | \
    ssh -p "$port" -o HostKeyAlgorithms=ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521 -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$known" -o GlobalKnownHostsFile=/dev/null \
        "$user@$TARGET" "umask 077; base=/var/lib/cbox/ls200-zoom; incoming=\$base/.live-incoming-\$\$; mkdir -p \$incoming; chmod 0700 \$incoming; trap 'rm -rf \"\$incoming\"' EXIT HUP INT TERM; tar -xf - -C \$incoming; sh \$incoming/ls200/install.sh --payload \$incoming/$payload_name --version $version --manifest-sha256 $manifest_sha$start_arg"
