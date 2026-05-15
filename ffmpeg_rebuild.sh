#!/usr/bin/env bash
# Rebuild static FFmpeg with the features the MP4/H.264/AAC writer needs.
#
# Run this from an MSYS2 mingw32 shell (the bash that lives in
# C:/msys64/mingw32.exe or sets MSYSTEM=MINGW32). Other shells will
# pick the wrong gcc and produce 64-bit objects we can't link against.
#
# Prerequisite (one-time):
#   pacman -S mingw-w64-i686-openh264
#
# Output: /c/tmp/ffbuild/install/{include,lib}/ — same layout as before,
# but libavcodec.a now exports libopenh264_encoder and aac_encoder, and
# libavformat.a exports mov_muxer (which writes .mp4).

set -euo pipefail

export PATH="/c/msys64/mingw32/bin:/c/msys64/usr/bin:${PATH}"
export PKG_CONFIG_PATH="/c/msys64/mingw32/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
# FFmpeg's configure writes to TMPDIR during probes; gcc/cc1/as use the
# Windows-style TEMP/TMP env vars for THEIR scratch files. If TEMP/TMP
# point at C:\Windows (the default when unset), the assembler can't
# write there. Set them all to a known-writable path.
mkdir -p /c/tmp
export TMPDIR=/c/tmp
export TEMP='C:\tmp'
export TMP='C:\tmp'

# FFmpeg's configure script's `cd` calls can't handle spaces in the
# source path. We mirror the source to a clean path under /c/tmp/ before
# configuring. If the mirror doesn't exist, create it.
FF_SRC_REAL="/c/Games/The Movies Complete Collection/mod/lib/ffmpeg-7.1.1"
FF_SRC="/c/tmp/ffmpeg-src"
FF_BUILD="/c/tmp/ffbuild/build"
FF_INSTALL="/c/tmp/ffbuild/install"

if [ ! -d "$FF_SRC" ]; then
    echo "Mirroring FFmpeg source to $FF_SRC (one-time)..."
    cp -r "$FF_SRC_REAL" "$FF_SRC"
fi

if ! pkg-config --exists openh264; then
    echo "ERROR: openh264 not found via pkg-config." >&2
    echo "Install it first:  pacman -S mingw-w64-i686-openh264" >&2
    exit 1
fi

mkdir -p "$FF_BUILD"
cd "$FF_BUILD"

# Configure. We intentionally do NOT pass --enable-gpl: libx264 is GPL
# and would force GPL on the whole mod, which is unacceptable for our
# licensing posture. OpenH264 is BSD-2-Clause + Cisco patent coverage,
# and the FFmpeg native AAC encoder is LGPL. Both stay clean.
"$FF_SRC/configure" \
    --prefix="$FF_INSTALL" \
    --target-os=mingw32 \
    --arch=i686 \
    --cc=i686-w64-mingw32-gcc \
    --enable-static \
    --disable-shared \
    --enable-libopenh264 \
    --disable-everything \
    --enable-protocol=file \
    --enable-encoder=libopenh264,aac \
    --enable-muxer=mp4,mov \
    --enable-bsf=h264_mp4toannexb,aac_adtstoasc \
    --enable-swscale \
    --enable-swresample \
    --disable-iconv \
    --disable-doc \
    --disable-programs \
    --disable-debug \
    --disable-vaapi \
    --disable-vdpau \
    --disable-d3d11va \
    --disable-d3d12va \
    --disable-dxva2 \
    --disable-cuda \
    --disable-cuvid \
    --disable-nvenc \
    --disable-nvdec \
    --disable-vulkan \
    --disable-mediafoundation \
    --disable-amf \
    --disable-network \
    --disable-postproc \
    --disable-avdevice \
    --disable-avfilter

make -j"$(nproc)"
make install

echo
echo "=== FFmpeg rebuilt with libopenh264 + AAC + MP4/MOV ==="
echo
echo "Next:  cd to mod/ and run 'make' to rebuild movies_fix.asi"
