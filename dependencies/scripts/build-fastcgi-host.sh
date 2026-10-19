#!/bin/sh
# Build the pinned FastCGI 2.4.7 client library (libfcgi) for the host into
# .work/build/fastcgi/prefix, for `make console-fastcgi-host`.
#
# Like the PJSIP host lane, this downloads only when invoked explicitly. The
# archive URL and SHA-256 come from dependencies/dependency-lock.json and the
# archive is refused unless its digest matches. The release tag ships without
# a generated configure script, so the two library sources the gateway needs
# (fcgiapp.c and os_unix.c) are compiled directly against a POSIX
# fcgi_config.h written here; nothing else from the archive is built.
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
root=$(CDPATH='' cd -- "$script_dir/../.." && pwd -P)
work=$root/.work
cache_dir=$work/cache/dependencies
build_root=$work/build/fastcgi
prefix=$build_root/prefix

fail() {
    echo "build-fastcgi-host: $*" >&2
    exit 1
}

for tool in curl tar shasum python3 cc; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool is required"
done

pin=$(python3 -B - "$root/dependencies/dependency-lock.json" <<'EOF'
import json, sys
packages = json.load(open(sys.argv[1], encoding="utf-8"))["packages"]
entry = next(item for item in packages if item["name"] == "FastCGI")
print(entry["version"], entry["immutable_url"], entry["archive_sha256"])
EOF
)
set -- $pin
version=$1
url=$2
digest=$3
archive=$cache_dir/fastcgi-$version.tar.gz

mkdir -p "$cache_dir" "$build_root"
if [ ! -f "$archive" ]; then
    curl --fail --silent --show-error --location --output "$archive.part" "$url"
    mv -f "$archive.part" "$archive"
fi
actual=$(shasum -a 256 "$archive" | awk '{print $1}')
[ "$actual" = "$digest" ] || fail "archive digest $actual does not match the pinned $digest"

# Always build from a fresh extraction of the verified archive, never from a
# tree left by an earlier run. The scratch tree lives in the system temporary
# directory; only the library and headers are kept below .work.
extract_dir=$(mktemp -d "${TMPDIR:-/tmp}/aula-fastcgi.XXXXXX")
tar -xzf "$archive" -C "$extract_dir"
source_dir=$extract_dir/fcgi2-$version
[ -f "$source_dir/libfcgi/fcgiapp.c" ] || fail "archive did not contain fcgi2-$version"

case $(uname -s) in
    Darwin)
        # GNU binutils' ar writes archives that Apple's linker rejects.
        ar=/usr/bin/ar
        sun_len='#define HAVE_SOCKADDR_UN_SUN_LEN 1'
        ;;
    *)
        ar='ar'
        sun_len=
        ;;
esac
cat > "$source_dir/include/fcgi_config.h" <<EOF
/* Written by build-fastcgi-host.sh for a POSIX host. */
#define HAVE_ARPA_INET_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_SOCKLEN 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRERROR 1
#define HAVE_STRING_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_LIBPTHREAD 1
#define USE_LOCKING 1
#define STDC_HEADERS 1
#define PACKAGE "fcgi"
#define VERSION "$version"
$sun_len
EOF

mkdir -p "$prefix/include" "$prefix/lib" "$build_root/obj"
# Third-party code: built without the project's -Werror policy.
for unit in fcgiapp os_unix; do
    cc -O2 -w -I"$source_dir/include" -c "$source_dir/libfcgi/$unit.c" \
        -o "$build_root/obj/$unit.o" || fail "compiling $unit.c failed"
done
"$ar" rcs "$prefix/lib/libfcgi.a.part" "$build_root/obj/fcgiapp.o" "$build_root/obj/os_unix.o"
mv -f "$prefix/lib/libfcgi.a.part" "$prefix/lib/libfcgi.a"
for header in fastcgi.h fcgi_config.h fcgiapp.h fcgimisc.h; do
    cp "$source_dir/include/$header" "$prefix/include/$header"
done
echo "build-fastcgi-host: libfcgi $version in $prefix"
