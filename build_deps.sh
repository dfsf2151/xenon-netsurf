#!/bin/bash
# Cross-builds the NetSurf dependency libraries for the Xbox 360 (xsdk toolchain).
# Usage: ./build_deps.sh [lib ...]
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
DEPS="$ROOT/deps"
PREFIX="$ROOT/prefix"
TC="$HOME/xsdk-gcc/bin/powerpc-xsdk-elf-"

export PREFIX
export NSSHARED="$DEPS/buildsystem"
export HOST=powerpc-xsdk-elf
export BUILD=x86_64-linux-gnu
export CC="${TC}gcc"
export AR="${TC}ar"
export CFLAGS="-mcpu=cell -fno-pic -O2 -g"

LIBS=("$@")
if [ ${#LIBS[@]} -eq 0 ]; then
    LIBS=(libwapcaplet libparserutils libhubbub libcss libdom libnsutils libnspsl)
fi

mkdir -p "$PREFIX"
status=0
for lib in "${LIBS[@]}"; do
    log="$ROOT/build-$lib.log"
    printf '=== %s ... ' "$lib"
    make -C "$DEPS/$lib" WARNFLAGS= WITH_EXPAT_BINDING=no install >"$log" 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "OK"
    else
        echo "FAIL ($rc) — see $log"
        tail -15 "$log"
        status=1
    fi
done
exit $status
