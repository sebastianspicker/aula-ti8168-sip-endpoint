#!/bin/sh
# Build the dependency-lock-pinned native nginx for the loopback console demo.
set -eu
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
root=$(CDPATH='' cd -- "$script_dir/../.." && pwd -P)
cache=$root/.work/cache/dependencies
build=$root/.work/build/nginx-host
prefix=$build/prefix
for tool in python3 curl tar shasum make cc pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || { echo "nginx host build requires $tool" >&2; exit 1; }
done
pin=$(python3 - "$root/dependencies/dependency-lock.json" <<'PY'
import json, sys
item = next(x for x in json.load(open(sys.argv[1]))['packages'] if x['name'] == 'nginx')
if not item.get('archive_sha256') or len(item['archive_sha256']) != 64:
    raise SystemExit('nginx archive hash is missing')
print(item['version'], item['immutable_url'], item['archive_sha256'])
PY
)
set -- $pin
version=$1
url=$2
digest=$3
mkdir -p "$cache" "$build" "$prefix"
archive=$cache/nginx-$version.tar.gz
if [ ! -f "$archive" ]; then
    curl --proto '=https' --tlsv1.2 --fail --silent --show-error --location --output "$archive.part" "$url"
    mv "$archive.part" "$archive"
fi
actual=$(shasum -a 256 "$archive" | awk '{print $1}')
[ "$actual" = "$digest" ] || { echo "nginx archive digest mismatch" >&2; exit 1; }
if python3 - "$build/build.json" "$prefix/sbin/nginx" "$digest" <<'PY'
from pathlib import Path
import hashlib, json, sys
try:
    record=json.loads(Path(sys.argv[1]).read_text())
    assert record['source_sha256']==sys.argv[3]
    assert record['binary_sha256']==hashlib.sha256(Path(sys.argv[2]).read_bytes()).hexdigest()
except (OSError, ValueError, KeyError, AssertionError):
    raise SystemExit(1)
PY
then
    echo "nginx host build ready: $prefix/sbin/nginx"
    exit 0
fi
source_dir=$(mktemp -d "$build/source.XXXXXX")
tar -xzf "$archive" -C "$source_dir"
cc_opts=$(pkg-config --cflags openssl)
ld_opts=$(pkg-config --libs openssl)
(
    cd "$source_dir/nginx-$version"
    ./configure --prefix="$prefix" --with-http_ssl_module \
        --with-cc-opt="$cc_opts" --with-ld-opt="$ld_opts" \
        --without-http_rewrite_module --without-http_gzip_module \
        --without-http_proxy_module --without-http_uwsgi_module \
        --without-http_scgi_module --without-http_memcached_module \
        --without-http_autoindex_module --without-http_ssi_module
    make -j "${NGINX_JOBS:-4}"
    make install
)
python3 - "$build/build.json" "$prefix/sbin/nginx" "$version" "$digest" "$source_dir" <<'PY'
from pathlib import Path
import hashlib, json, subprocess, sys
path=Path(sys.argv[2])
record={'version':sys.argv[3], 'source_sha256':sys.argv[4], 'source_directory':sys.argv[5],
        'binary_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
        'configuration':subprocess.run([str(path),'-V'],capture_output=True,text=True,check=True).stderr}
Path(sys.argv[1]).write_text(json.dumps(record,indent=2)+'\n')
PY
echo "nginx host build ready: $prefix/sbin/nginx"
