#!/bin/sh
# Cross-build the production FastCGI gateway from reviewed static inputs.
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
console_dir=$(CDPATH='' cd -- "$script_dir/../.." && pwd -P)
repository_dir=$(CDPATH='' cd -- "$console_dir/../.." && pwd -P)
sipd_dir=$repository_dir/product/sipd
path_guard=$repository_dir/tooling/workspace/protected_output.py
cross_prefix=${LS200_CROSS_PREFIX:?set LS200_CROSS_PREFIX to an ARM EABI5 toolchain prefix}
sysroot=${LS200_SYSROOT:?set LS200_SYSROOT to the reviewed development sysroot}
build_dir=${LS200_GATEWAY_ARM_BUILD_DIR:?set LS200_GATEWAY_ARM_BUILD_DIR to an existing output directory}

[ -d "$sysroot" ] && [ -d "$build_dir" ] || {
    echo "build-gateway-arm: sysroot and output directory must already exist" >&2
    exit 2
}
canonical_sysroot=$(python3 "$path_guard" --input "$repository_dir" "$sysroot") || exit 2
canonical_build=$(python3 "$path_guard" "$repository_dir" "$build_dir") || exit 2
compiler=$(command -v "${cross_prefix}gcc") || {
    echo "build-gateway-arm: ARM cross compiler is unavailable" >&2
    exit 2
}
compiler=$(python3 "$path_guard" --input "$repository_dir" "$compiler") || exit 2

for dependency in libjansson.a libssl.a libcrypto.a libfcgi.a; do
    candidate=$canonical_sysroot/usr/lib/$dependency
    [ -f "$candidate" ] && [ ! -L "$candidate" ] || {
        echo "build-gateway-arm: reviewed dependency missing or symlinked: $dependency" >&2
        exit 2
    }
done
for header in jansson.h openssl/ssl.h fcgiapp.h; do
    candidate=$canonical_sysroot/usr/include/$header
    [ -f "$candidate" ] && [ ! -L "$candidate" ] || {
        echo "build-gateway-arm: reviewed header missing or symlinked: $header" >&2
        exit 2
    }
done

"$compiler" \
    -std=c17 -Wall -Wextra -Werror -pedantic -O2 \
    -I"$console_dir/gateway" -I"$console_dir/gateway/preview" \
    -I"$sipd_dir/include" -I"$sipd_dir/src/backends" \
    -I"$canonical_sysroot/usr/include" \
    -DLS200_GATEWAY_WITH_FCGI \
    "$console_dir/gateway/gateway.c" \
    "$console_dir/device/transport.c" \
    "$console_dir/device/wire.c" \
    "$console_dir/device/client.c" \
    "$console_dir/device/projection.c" \
    "$console_dir/device/credentials.c" \
    "$console_dir/gateway/fastcgi_main.c" \
    "$console_dir/gateway/fastcgi_request.c" \
    "$console_dir/gateway/fastcgi_preview.c" \
    "$console_dir/gateway/preview/preview_reader.c" \
    "$console_dir/gateway/preview/preview_reader_protocol.c" \
    "$console_dir/gateway/preview/preview_reader_rtp.c" \
    "$console_dir/gateway/preview/preview_flv.c" \
    "$sipd_dir/src/backends/rtsp_parser.c" \
    "$sipd_dir/src/media/h264.c" \
    "$sipd_dir/src/media/h264_depacketizer.c" \
    "$canonical_sysroot/usr/lib/libjansson.a" \
    "$canonical_sysroot/usr/lib/libssl.a" \
    "$canonical_sysroot/usr/lib/libcrypto.a" \
    "$canonical_sysroot/usr/lib/libfcgi.a" \
    -lm -lpthread -lrt -ldl \
    -o "$canonical_build/ls200-gateway-fcgi"
