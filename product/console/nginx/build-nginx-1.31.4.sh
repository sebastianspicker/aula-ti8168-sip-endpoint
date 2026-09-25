#!/bin/sh
set -eu

# Run from an exact nginx-1.31.4 source directory. The preparer admits only the
# reviewed source hashes and supplies the recovered LS200 cross-probe facts.
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
repo_dir=$(CDPATH='' cd -- "$script_dir/../../.." && pwd -P)
path_guard=$repo_dir/tooling/workspace/protected_output.py
cross_prefix=${LS200_CROSS_PREFIX:?set LS200_CROSS_PREFIX to an ARM EABI5 toolchain prefix}
sysroot=${LS200_SYSROOT:?set LS200_SYSROOT to the reviewed development sysroot}
build_dir=${LS200_NGINX_ARM_BUILD_DIR:?set LS200_NGINX_ARM_BUILD_DIR to the staged nginx source}
canonical_build=$(python3 "$path_guard" "$repo_dir" "$build_dir") || exit 2
physical_pwd=$(pwd -P)
[ "$build_dir" = "$canonical_build" ] && [ "$PWD" = "$canonical_build" ] &&
    [ "$physical_pwd" = "$canonical_build" ] || {
    echo "build-nginx: working directory must be the canonical staged .work source" >&2
    exit 2
}
canonical_sysroot=$(python3 "$path_guard" --input "$repo_dir" "$sysroot") || exit 2
[ "$sysroot" = "$canonical_sysroot" ] || {
    echo "build-nginx: sysroot path must be canonical" >&2
    exit 2
}
[ -d "$canonical_sysroot/usr/include/openssl" ] &&
    [ -f "$canonical_sysroot/usr/lib/libssl.a" ] &&
    [ -f "$canonical_sysroot/usr/lib/libcrypto.a" ] || {
    echo "build-nginx: reviewed static OpenSSL is missing from the sysroot" >&2
    exit 2
}
compiler=$(command -v "${cross_prefix}gcc") || {
    echo "build-nginx: ARM cross compiler is unavailable" >&2
    exit 2
}
canonical_compiler=$(python3 "$path_guard" --input "$repo_dir" "$compiler") || exit 2
[ "$compiler" = "$canonical_compiler" ] || {
    echo "build-nginx: compiler path must be canonical" >&2
    exit 2
}
"$repo_dir/dependencies/scripts/prepare-nginx-ls200-cross.sh" "$canonical_build"

./configure \
  --crossbuild=Linux:2.6.37:arm \
  --with-cc="$canonical_compiler" \
  --with-cc-opt="-I$canonical_sysroot/usr/include" \
  --with-ld-opt="-L$canonical_sysroot/usr/lib -L$canonical_sysroot/lib -Wl,-rpath-link,$canonical_sysroot/lib -Wl,-rpath-link,$canonical_sysroot/usr/lib" \
  --prefix=/opt/ls200-nginx-1.31.4 \
  --sbin-path=/usr/sbin/ls200-nginx \
  --conf-path=/etc/ls200-console/nginx.conf \
  --pid-path=/run/ls200-nginx.pid \
  --error-log-path=/var/log/ls200-console/nginx-error.log \
  --http-log-path=/var/log/ls200-console/nginx-access.log \
  --user=ls200-web \
  --group=ls200-web \
  --with-http_ssl_module \
  --without-quic_bpf_module \
  --without-http-cache \
  --without-http_gzip_module \
  --without-http_rewrite_module \
  --without-pcre \
  --without-http_charset_module \
  --without-http_autoindex_module \
  --without-http_browser_module \
  --without-http_empty_gif_module \
  --without-http_geo_module \
  --without-http_grpc_module \
  --without-http_access_module \
  --without-http_auth_basic_module \
  --without-http_mirror_module \
  --without-http_limit_conn_module \
  --without-http_map_module \
  --without-http_memcached_module \
  --without-http_proxy_module \
  --without-http_referer_module \
  --without-http_scgi_module \
  --without-http_split_clients_module \
  --without-http_ssi_module \
  --without-http_upstream_hash_module \
  --without-http_upstream_ip_hash_module \
  --without-http_upstream_keepalive_module \
  --without-http_upstream_least_conn_module \
  --without-http_upstream_least_time_module \
  --without-http_upstream_random_module \
  --without-http_upstream_zone_module \
  --without-http_upstream_sticky_module \
  --without-http_tunnel_module \
  --without-http_userid_module \
  --without-http_uwsgi_module
make -j"${LS200_BUILD_JOBS:-1}"
