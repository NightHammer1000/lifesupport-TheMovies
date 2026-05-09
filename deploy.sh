#!/usr/bin/env bash
# Copy built ASI + libmpv-2.dll into the game directory.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
GAME="$(cd "$ROOT/.." && pwd)"

if [[ ! -f "$ROOT/build/movies_fix.asi" ]]; then
    echo "build/movies_fix.asi missing — run build.sh first" >&2
    exit 1
fi

if [[ ! -f "$ROOT/lib/libmpv/libmpv-2.dll" ]]; then
    echo "lib/libmpv/libmpv-2.dll missing — extract the libmpv dev archive there" >&2
    exit 1
fi

cp "$ROOT/build/movies_fix.asi"      "$GAME/movies_fix.asi"
cp "$ROOT/lib/libmpv/libmpv-2.dll"   "$GAME/libmpv-2.dll"
# dinput8.dll only if the user doesn't already have one
if [[ ! -f "$GAME/dinput8.dll" ]] && [[ -f "$ROOT/lib/asi-loader/dinput8.dll" ]]; then
    cp "$ROOT/lib/asi-loader/dinput8.dll" "$GAME/dinput8.dll"
fi

echo "Deployed to $GAME"
ls -la "$GAME/dinput8.dll" "$GAME/movies_fix.asi" "$GAME/libmpv-2.dll" 2>/dev/null
