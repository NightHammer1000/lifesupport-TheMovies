#!/usr/bin/env bash
# Build movies_fix.asi.
#
# Requires:
#   - MSYS2 with mingw32 toolchain at  C:/msys64/mingw32
#   - Custom static FFmpeg build at    C:/tmp/ffbuild/install
#   - libmpv i686 dev archive extracted into  mod/lib/libmpv
#
# Output: build/movies_fix.asi (~4.5 MB)
# Deploy: copy build/movies_fix.asi and lib/libmpv/libmpv-2.dll into the
# game directory next to dinput8.dll (Ultimate ASI Loader).

set -euo pipefail

export PATH="/c/msys64/mingw32/bin:${PATH}"

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

FF_PREFIX="/c/tmp/ffbuild/install"
MPV_DIR="lib/libmpv"
MH_DIR="lib/minhook"

CFLAGS=(
    -m32 -O2 -Wall
    -Wno-unknown-pragmas -Wno-misleading-indentation
    -I"$MH_DIR" -I"$MH_DIR/include"
    -Isrc
    -I"$FF_PREFIX/include"
    -I"$MPV_DIR/include"
    -DWIN32_LEAN_AND_MEAN
)

mkdir -p build

# --- compile sources ---
SRC=(
    src/main.c
    src/log.c
    src/sync_reader.c
    src/profile_mgr.c
    src/media_props.c
    src/nss_buffer.c
    src/ds_filter.c
    src/ds_output_pin.c
    src/ds_fakegraph.c
    src/mpv_player.c
)

for f in "${SRC[@]}"; do
    o="build/$(basename "${f%.c}").o"
    if [[ "$f" -nt "$o" || ! -f "$o" ]]; then
        echo "  CC   $f"
        gcc "${CFLAGS[@]}" -c -o "$o" "$f"
    fi
done

# --- compile minhook (vendored) ---
MH_SRC=(
    "$MH_DIR/src/hook.c"
    "$MH_DIR/src/buffer.c"
    "$MH_DIR/src/trampoline.c"
    "$MH_DIR/src/hde/hde32.c"
)
for f in "${MH_SRC[@]}"; do
    o="build/$(basename "${f%.c}").o"
    if [[ "$f" -nt "$o" || ! -f "$o" ]]; then
        echo "  CC   $f"
        gcc "${CFLAGS[@]}" -c -o "$o" "$f"
    fi
done

# --- link ---
echo "  LD   build/movies_fix.asi"
i686-w64-mingw32-gcc -m32 -shared -static-libgcc \
    -o build/movies_fix.asi \
    build/main.o build/log.o build/sync_reader.o build/profile_mgr.o \
    build/media_props.o build/nss_buffer.o \
    build/ds_filter.o build/ds_output_pin.o build/ds_fakegraph.o \
    build/mpv_player.o \
    build/hook.o build/buffer.o build/trampoline.o build/hde32.o \
    -L"$FF_PREFIX/lib" -Wl,-Bstatic \
        -lavformat -lavcodec -lswscale -lswresample -lavutil \
        -lz -lwinpthread \
    -Wl,-Bdynamic \
        -L"$MPV_DIR" -lmpv \
        -lole32 -loleaut32 -luuid -lws2_32 -lbcrypt -lm -lsecur32 -lwinmm

echo "OK -> build/movies_fix.asi"
ls -la build/movies_fix.asi
