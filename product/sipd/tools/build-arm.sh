#!/bin/sh
# Cross-build only. This script never writes to extracted firmware evidence.
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
repository_root=$(CDPATH='' cd -- "$project_dir/../.." && pwd -P)
path_guard="$repository_root/tooling/workspace/protected_output.py"
cross_prefix=${AULA_CROSS_PREFIX:?set AULA_CROSS_PREFIX to an ARM EABI5 toolchain prefix}
sysroot=${AULA_SYSROOT:?set AULA_SYSROOT to a separately generated development sysroot}
build_dir=${AULA_ARM_BUILD_DIR:-"$repository_root/.work/build/sipd-arm"}
pjsip_include=${AULA_SIPD_PJSIP_INCLUDE_DIR:?set AULA_SIPD_PJSIP_INCLUDE_DIR to the reviewed pjproject include root in the sysroot}
pjsip_libraries=${AULA_SIPD_PJSIP_LIBRARIES:?set AULA_SIPD_PJSIP_LIBRARIES to a semicolon-separated list of reviewed ARM libraries in the sysroot}
pjsip_ua_library=${AULA_SIPD_PJSIP_UA_LIBRARY:?set AULA_SIPD_PJSIP_UA_LIBRARY to the reviewed ARM libpjsip-ua archive in the sysroot}
pjmedia_library=${AULA_SIPD_PJMEDIA_LIBRARY:?set AULA_SIPD_PJMEDIA_LIBRARY to the reviewed ARM libpjmedia archive in the sysroot}
faad2_include=${AULA_SIPD_FAAD2_INCLUDE_DIR:?set AULA_SIPD_FAAD2_INCLUDE_DIR to the reviewed FAAD2 2.11.2 include root in the sysroot}
faad2_library=${AULA_SIPD_FAAD2_LIBRARY:?set AULA_SIPD_FAAD2_LIBRARY to the reviewed FAAD2 2.11.2 ARM library in the sysroot}
speexdsp_include=${AULA_SIPD_SPEEXDSP_INCLUDE_DIR:?set AULA_SIPD_SPEEXDSP_INCLUDE_DIR to the reviewed SpeexDSP 1.2.1 include root in the sysroot}
speexdsp_library=${AULA_SIPD_SPEEXDSP_LIBRARY:?set AULA_SIPD_SPEEXDSP_LIBRARY to the reviewed SpeexDSP 1.2.1 ARM library in the sysroot}
srtp_include=${AULA_SIPD_SRTP_INCLUDE_DIR:?set AULA_SIPD_SRTP_INCLUDE_DIR to the reviewed libsrtp 2.8 include root in the sysroot}
srtp_library=${AULA_SIPD_SRTP_LIBRARY:?set AULA_SIPD_SRTP_LIBRARY to the reviewed libsrtp 2.8 ARM library in the sysroot}

canonical_dir() {
    (CDPATH='' cd -- "$1" && pwd -P)
}

canonical_file() {
    file_dir=$(canonical_dir "$(dirname -- "$1")")
    printf '%s/%s\n' "$file_dir" "$(basename -- "$1")"
}

require_sysroot_dir() {
    dependency_dir=$1
    required_header=$2
    label=$3
    [ -d "$dependency_dir" ] || {
        echo "build-arm: $label include root is not a directory" >&2; exit 2;
    }
    canonical_dependency_dir=$(canonical_dir "$dependency_dir")
    case "$canonical_dependency_dir" in
        "$canonical_sysroot"|"$canonical_sysroot"/*) ;;
        *) echo "build-arm: $label include root must be inside the development sysroot" >&2; exit 2 ;;
    esac
    [ -f "$canonical_dependency_dir/$required_header" ] &&
        [ ! -L "$canonical_dependency_dir/$required_header" ] || {
        echo "build-arm: $label required header is missing or a symlink" >&2; exit 2;
    }
    printf '%s\n' "$canonical_dependency_dir"
}

require_sysroot_library() {
    dependency_library=$1
    label=$2
    [ -f "$dependency_library" ] && [ ! -L "$dependency_library" ] || {
        echo "build-arm: $label library must be a regular non-symlink file" >&2; exit 2;
    }
    canonical_dependency_library=$(canonical_file "$dependency_library")
    case "$canonical_dependency_library" in
        "$canonical_sysroot"/*) ;;
        *) echo "build-arm: $label library must be inside the development sysroot" >&2; exit 2 ;;
    esac
    printf '%s\n' "$canonical_dependency_library"
}

[ -d "$sysroot" ] || { echo "build-arm: sysroot is not a directory" >&2; exit 2; }
[ -d "$build_dir" ] || { echo "build-arm: build directory must exist before invocation" >&2; exit 2; }
canonical_sysroot=$(python3 "$path_guard" --input "$repository_root" "$sysroot") || exit 2
canonical_build=$(python3 "$path_guard" "$repository_root" "$build_dir") || exit 2

canonical_pjsip_include=$(require_sysroot_dir "$pjsip_include" pjsip.h pjproject)
canonical_pjsip_ua_library=$(require_sysroot_library "$pjsip_ua_library" "libpjsip-ua")
canonical_pjmedia_library=$(require_sysroot_library "$pjmedia_library" libpjmedia)
canonical_faad2_include=$(require_sysroot_dir "$faad2_include" neaacdec.h "FAAD2 2.11.2")
canonical_speexdsp_include=$(require_sysroot_dir "$speexdsp_include" speex/speex_resampler.h "SpeexDSP 1.2.1")
canonical_srtp_include=$(require_sysroot_dir "$srtp_include" srtp2/srtp.h "libsrtp 2.8")
canonical_faad2_library=$(require_sysroot_library "$faad2_library" "FAAD2 2.11.2")
canonical_speexdsp_library=$(require_sysroot_library "$speexdsp_library" "SpeexDSP 1.2.1")
canonical_srtp_library=$(require_sysroot_library "$srtp_library" "libsrtp 2.8")

old_ifs=$IFS
IFS=';'
set -f
# shellcheck disable=SC2086 # Intentional split on the semicolon-only IFS.
set -- $pjsip_libraries
set +f
IFS=$old_ifs
[ "$#" -gt 0 ] || { echo "build-arm: pjproject library list is empty" >&2; exit 2; }
canonical_pjsip_libraries=
for pjsip_library do
    canonical_pjsip_library=$(require_sysroot_library "$pjsip_library" pjproject)
    if [ -z "$canonical_pjsip_libraries" ]; then
        canonical_pjsip_libraries=$canonical_pjsip_library
    else
        canonical_pjsip_libraries="$canonical_pjsip_libraries;$canonical_pjsip_library"
    fi
done

export SYSROOT="$canonical_sysroot"
export AULA_ARM_BUILD_DIR="$canonical_build"

command -v cmake >/dev/null 2>&1 || {
    echo "build-arm: cmake is required" >&2
    exit 2
}
CC=$(command -v "${cross_prefix}gcc") || { echo "build-arm: cross compiler is unavailable" >&2; exit 2; }
AR=$(command -v "${cross_prefix}ar") || { echo "build-arm: cross archiver is unavailable" >&2; exit 2; }
RANLIB=$(command -v "${cross_prefix}ranlib") || { echo "build-arm: cross ranlib is unavailable" >&2; exit 2; }
CC=$(python3 "$path_guard" --input "$repository_root" "$CC") || exit 2
AR=$(python3 "$path_guard" --input "$repository_root" "$AR") || exit 2
RANLIB=$(python3 "$path_guard" --input "$repository_root" "$RANLIB") || exit 2
export CC AR RANLIB

# This explicit CMake invocation cannot fetch a toolchain or execute a target
# binary.  The build directory is caller-created so an accidental typo cannot
# create output beneath the recovered firmware evidence.
cmake -S "$project_dir" -B "$canonical_build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=armv7 \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_AR="$AR" \
    -DCMAKE_RANLIB="$RANLIB" \
    -DCMAKE_SYSROOT="$SYSROOT" \
    -DCMAKE_FIND_ROOT_PATH="$SYSROOT" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DAULA_SIPD_TARGET_PROFILE=aula-arm-eabi5 \
    -DAULA_SIPD_ENABLE_PJSIP=ON \
    -DAULA_SIPD_PJSIP_INCLUDE_DIR="$canonical_pjsip_include" \
    -DAULA_SIPD_PJSIP_LIBRARIES="$canonical_pjsip_libraries" \
    -DAULA_SIPD_PJSIP_UA_LIBRARY="$canonical_pjsip_ua_library" \
    -DAULA_SIPD_PJMEDIA_LIBRARY="$canonical_pjmedia_library" \
    -DAULA_SIPD_ENABLE_FAAD2=ON \
    -DAULA_SIPD_FAAD2_INCLUDE_DIR="$canonical_faad2_include" \
    -DAULA_SIPD_FAAD2_LIBRARY="$canonical_faad2_library" \
    -DAULA_SIPD_ENABLE_SPEEXDSP=ON \
    -DAULA_SIPD_SPEEXDSP_INCLUDE_DIR="$canonical_speexdsp_include" \
    -DAULA_SIPD_SPEEXDSP_LIBRARY="$canonical_speexdsp_library" \
    -DAULA_SIPD_ENABLE_SRTP=ON \
    -DAULA_SIPD_SRTP_INCLUDE_DIR="$canonical_srtp_include" \
    -DAULA_SIPD_SRTP_LIBRARY="$canonical_srtp_library"
cmake --build "$canonical_build" --target aula-sipd --parallel "${AULA_BUILD_JOBS:-1}"
