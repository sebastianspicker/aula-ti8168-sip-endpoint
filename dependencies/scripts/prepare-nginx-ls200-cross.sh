#!/bin/sh
# Prepare the exact nginx 1.31.4 source for compile-tested LS200 cross probes.
set -eu

usage() {
    echo "usage: $0 NGINX_SOURCE_DIR" >&2
    exit 2
}

[ "$#" -eq 1 ] || usage
source_dir=$1
[ -d "$source_dir/auto" ] || usage
patch_file=$(CDPATH='' cd -- "$(dirname -- "$0")/../patches" && pwd -P)/nginx-ls200-cross.patch
command -v shasum >/dev/null 2>&1 || {
    echo "prepare-nginx-ls200-cross: shasum is required" >&2
    exit 2
}
command -v patch >/dev/null 2>&1 || {
    echo "prepare-nginx-ls200-cross: patch is required" >&2
    exit 2
}

state=
check_file() {
    relative=$1
    original=$2
    patched=$3
    candidate=$source_dir/$relative
    [ -f "$candidate" ] && [ ! -L "$candidate" ] || {
        echo "prepare-nginx-ls200-cross: unsafe or missing source file: $relative" >&2
        exit 2
    }
    actual=$(shasum -a 256 "$candidate" | awk '{print $1}')
    case "$actual" in
        "$original") file_state=original ;;
        "$patched") file_state=patched ;;
        *)
            echo "prepare-nginx-ls200-cross: source is not the reviewed nginx 1.31.4 pin: $relative" >&2
            exit 1
            ;;
    esac
    if [ -n "$state" ] && [ "$state" != "$file_state" ]; then
        echo "prepare-nginx-ls200-cross: mixed original and patched source state" >&2
        exit 1
    fi
    state=$file_state
}

check_file configure df485655b589ead56ed03f38792445c060bdf3cb4026486b434a2a912cbb65e5 276bd67c9e1d0098446925a73164b136f658abeeab75a1c44b385da21c90971c
check_file auto/cc/name 18e073edd69fc128382d72836623c1ad885f5efba45ec94e3dc50b657a8fa7f1 8bb93f298aeeb19d6d9730905602222f3fb7aaa7dd1fa53eff5dd3bc905afef6
check_file auto/feature 26c2c1025d08d30caef8425e8f314e05cbe19a4bd03a7e9811b00fb73498b3dd 77c1e12b2c9003c31a1d95d18fb2bbd9ad5845ca6f6ecc79b96a17218704d37a
check_file auto/types/sizeof 6d448e631c9d25e4c67d941ac8ae481f28434ef3dbea58a21bfb2513cfb30ead ff2507d37951806164e98f82112940493dacf077b23aa7bf71690b13a360aba9
check_file auto/endianness 744b6b6b497e310cae231997243327dfe77b249625af1ecb191264a802967fae 1c9827ef15a41770c5a4f8623eef287cec18e57aa335f43076a82c172fa8b009
check_file auto/os/linux ca64d59954550c9a193ce9f281a319c40e56c2cf8b96af6f33b0bbf90c0d7bc7 d4c30ee04c532b3ddf5e2b435a3a260cce888dac58afc49f11029ab01d9bfc58
check_file auto/unix a33cad669af36c6324ff3a4cfddfddc361b094aa1b13f472bfeac7d0e31ac0c2 6b45c2bb3bf0a5b92132d0d6506582a5a192a938f3e95707ecf8915a85ca8890

if [ "$state" = patched ]; then
    echo "prepare-nginx-ls200-cross: already prepared"
    exit 0
fi

patch --batch --forward --directory "$source_dir" --strip 1 < "$patch_file"
state=
check_file configure df485655b589ead56ed03f38792445c060bdf3cb4026486b434a2a912cbb65e5 276bd67c9e1d0098446925a73164b136f658abeeab75a1c44b385da21c90971c
check_file auto/cc/name 18e073edd69fc128382d72836623c1ad885f5efba45ec94e3dc50b657a8fa7f1 8bb93f298aeeb19d6d9730905602222f3fb7aaa7dd1fa53eff5dd3bc905afef6
check_file auto/feature 26c2c1025d08d30caef8425e8f314e05cbe19a4bd03a7e9811b00fb73498b3dd 77c1e12b2c9003c31a1d95d18fb2bbd9ad5845ca6f6ecc79b96a17218704d37a
check_file auto/types/sizeof 6d448e631c9d25e4c67d941ac8ae481f28434ef3dbea58a21bfb2513cfb30ead ff2507d37951806164e98f82112940493dacf077b23aa7bf71690b13a360aba9
check_file auto/endianness 744b6b6b497e310cae231997243327dfe77b249625af1ecb191264a802967fae 1c9827ef15a41770c5a4f8623eef287cec18e57aa335f43076a82c172fa8b009
check_file auto/os/linux ca64d59954550c9a193ce9f281a319c40e56c2cf8b96af6f33b0bbf90c0d7bc7 d4c30ee04c532b3ddf5e2b435a3a260cce888dac58afc49f11029ab01d9bfc58
check_file auto/unix a33cad669af36c6324ff3a4cfddfddc361b094aa1b13f472bfeac7d0e31ac0c2 6b45c2bb3bf0a5b92132d0d6506582a5a192a938f3e95707ecf8915a85ca8890
[ "$state" = patched ] || {
    echo "prepare-nginx-ls200-cross: patched source digest mismatch" >&2
    exit 1
}
echo "prepare-nginx-ls200-cross: target profile prepared"
