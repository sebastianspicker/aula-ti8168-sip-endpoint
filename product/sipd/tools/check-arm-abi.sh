#!/bin/sh
# Inspect ARM ELF metadata without executing the candidate binary.
set -eu

usage() {
    echo "usage: $0 --binary FILE --reference FILE --sysroot DIR [--allowed-needed LIST] [--glibc-max VERSION]" >&2
    exit 2
}

binary=
reference=
sysroot=
# The declared target ABI includes the glibc system resolver for DNS setup.
allowed_needed='libc.so.6 libm.so.6 libpthread.so.0 librt.so.1 libdl.so.2 libresolv.so.2 libgcc_s.so.1 ld-linux.so.3'
glibc_max=2.12
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary=${2:?missing binary}; shift 2 ;;
        --reference) reference=${2:?missing reference}; shift 2 ;;
        --sysroot) sysroot=${2:?missing sysroot}; shift 2 ;;
        --allowed-needed) allowed_needed=${2:?missing library list}; shift 2 ;;
        --glibc-max) glibc_max=${2:?missing version}; shift 2 ;;
        *) usage ;;
    esac
done
[ -n "$binary" ] && [ -f "$binary" ] && [ -n "$reference" ] && [ -f "$reference" ] && [ -d "$sysroot" ] || usage
readelf_bin=${READELF:-readelf}
command -v "$readelf_bin" >/dev/null 2>&1 || { echo "check-arm-abi: readelf unavailable" >&2; exit 2; }

header=$($readelf_bin -h "$binary")
printf '%s\n' "$header" | grep -Eq 'Class:[[:space:]]+ELF32' || { echo "check-arm-abi: not ELF32" >&2; exit 1; }
printf '%s\n' "$header" | grep -Eq 'Data:[[:space:]]+2.s complement, little endian' || { echo "check-arm-abi: not little-endian" >&2; exit 1; }
printf '%s\n' "$header" | grep -Eq 'Machine:[[:space:]]+ARM' || { echo "check-arm-abi: not ARM" >&2; exit 1; }
printf '%s\n' "$header" | grep -Eq 'Flags:.*Version5 EABI' || { echo "check-arm-abi: not ARM EABI5" >&2; exit 1; }
attributes=$($readelf_bin -A "$binary" | grep -E 'Tag_CPU_arch:|Tag_ABI_VFP_args:' | sed 's/^[[:space:]]*//' | sort -u)
reference_attributes=$($readelf_bin -A "$reference" | grep -E 'Tag_CPU_arch:|Tag_ABI_VFP_args:' | sed 's/^[[:space:]]*//' | sort -u)
[ "$attributes" = "$reference_attributes" ] || {
    echo "check-arm-abi: CPU or float-ABI attributes differ from the supplied ABI reference" >&2; exit 1;
}

program_headers=$($readelf_bin -l "$binary")
if printf '%s\n' "$header" | grep -Eq 'Type:[[:space:]]+DYN|Type:[[:space:]]+EXEC'; then
    printf '%s\n' "$program_headers" | grep -Fq 'Requesting program interpreter: /lib/ld-linux.so.3' || {
        echo "check-arm-abi: dynamic interpreter is not /lib/ld-linux.so.3" >&2; exit 1;
    }
fi
dynamic=$($readelf_bin -d "$binary")
if printf '%s\n' "$dynamic" | grep -Eq '\((RPATH|RUNPATH)\)'; then
    echo "check-arm-abi: RPATH/RUNPATH is forbidden" >&2
    exit 1
fi
needed_libraries=$(printf '%s\n' "$dynamic" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
if [ -n "$needed_libraries" ]; then
    printf '%s\n' "$needed_libraries" | while IFS= read -r needed; do
        case " $allowed_needed " in
            *" $needed "*) ;;
            *) echo "check-arm-abi: unexpected DT_NEEDED: $needed" >&2; exit 1 ;;
        esac
    done
fi

versions=$($readelf_bin --version-info "$binary" 2>/dev/null | grep -Eo 'GLIBC_[0-9]+\.[0-9]+' | sort -u || true)
if [ -n "$versions" ]; then
    highest=$(printf '%s\n' "$versions" | sed 's/^GLIBC_//' | sort -V | tail -n 1)
    if [ "$(printf '%s\n%s\n' "$glibc_max" "$highest" | sort -V | tail -n 1)" != "$glibc_max" ]; then
        echo "check-arm-abi: GLIBC_$highest exceeds GLIBC_$glibc_max" >&2
        exit 1
    fi
fi
# Dynamic imports are normal. Resolve each non-weak import against the declared
# DT_NEEDED libraries in the supplied development sysroot without running ARM.
providers=$(mktemp "${TMPDIR:-/tmp}/aula-sipd-abi-providers.XXXXXX")
imports=$(mktemp "${TMPDIR:-/tmp}/aula-sipd-abi-imports.XXXXXX")
trap 'rm -f "$providers" "$imports"' EXIT HUP INT TERM
if [ -n "$needed_libraries" ]; then
    printf '%s\n' "$needed_libraries" | while IFS= read -r needed; do
        library=
        for candidate in "$sysroot/lib/$needed" "$sysroot/usr/lib/$needed"; do
            [ -f "$candidate" ] && { library=$candidate; break; }
        done
        [ -n "$library" ] || { echo "check-arm-abi: missing DT_NEEDED library in sysroot: $needed" >&2; exit 1; }
        "$readelf_bin" --dyn-syms -W "$library" | awk '$7 != "UND" && $8 != "" { sub(/@.*/, "", $8); print $8 }' >> "$providers"
    done
fi
"$readelf_bin" --dyn-syms -W "$binary" | awk '$5 != "WEAK" && $7 == "UND" && $8 != "" { sub(/@.*/, "", $8); print $8 }' | sort -u > "$imports"
sort -u "$providers" -o "$providers"
unresolved=$(comm -23 "$imports" "$providers" || true)
[ -z "$unresolved" ] || { echo "check-arm-abi: unresolved dynamic imports: $unresolved" >&2; exit 1; }
echo "check-arm-abi: OK"
