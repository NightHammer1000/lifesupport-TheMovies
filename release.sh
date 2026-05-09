#!/usr/bin/env bash
# Package an end-user release zip.
#
# Output: release/lifesupport-themovies-<version>.zip
# Contains: dinput8.dll, movies_fix.asi, libmpv-2.dll, README.md, LICENSE,
#          INSTALL.txt
#
# Usage:  bash release.sh [version]   (default: v0.1.0)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

VERSION="${1:-v0.1.0}"
STAGE="release/stage"
ZIP="release/lifesupport-themovies-${VERSION}.zip"

# 1. Build first
make >/dev/null

# 2. Verify all required pieces exist
need=(
    "build/movies_fix.asi"
    "lib/libmpv/libmpv-2.dll"
    "lib/asi-loader/dinput8.dll"
    "README.md"
    "LICENSE"
)
for f in "${need[@]}"; do
    if [[ ! -f "$f" ]]; then
        echo "missing: $f" >&2
        exit 1
    fi
done

# 3. Stage
rm -rf "$STAGE" "$ZIP"
mkdir -p "$STAGE"

cp build/movies_fix.asi          "$STAGE/movies_fix.asi"
cp lib/libmpv/libmpv-2.dll       "$STAGE/libmpv-2.dll"
cp lib/asi-loader/dinput8.dll    "$STAGE/dinput8.dll"
cp lib/asi-loader/NOTICE.md      "$STAGE/NOTICE-asi-loader.md"
cp README.md                     "$STAGE/README.md"
cp LICENSE                       "$STAGE/LICENSE"

cat > "$STAGE/INSTALL.txt" <<'EOF'
KPC LifeSupport — The Movies (WMF replacement)
==============================================

Drop these three files next to MoviesSE.exe in your install directory:

  - dinput8.dll       (Ultimate ASI Loader)
  - movies_fix.asi    (this mod)
  - libmpv-2.dll      (libmpv binary)

Start the game. That's it.

If the game already has a dinput8.dll from another mod (a different ASI
loader chain, dgVoodoo2, ENB, etc.), don't overwrite it — keep your
existing one and only copy movies_fix.asi + libmpv-2.dll.

Logs go to movies_fix.log in the game directory.

USING WITH dgVoodoo2
--------------------
If you also use dgVoodoo2 for graphics (recommended on modern Windows):

  - Drop ALL of dgVoodoo2's wrapper DLLs in together: DDraw.dll,
    D3D8.dll, D3D9.dll, D3DImm.dll. Wrapping D3D9.dll alone causes a
    crash inside dgVoodoo2 at the transition into in-game mode.
  - Keep dgVoodoo.conf at its shipped defaults. Only safe edit is
    the watermark.

VERIFIED SCOPE
--------------
This release has been verified to work for launch + main menu navigation.
Actual in-game play (starting a studio, the campaign, the simulation)
has NOT been tested yet. Movie export writes a real .wmv but the encoded
output may show codec artifacts — fix in progress.

If you hit a problem during gameplay, please file an issue with your
movies_fix.log attached.

See README.md for the full feature matrix and architecture notes.

This is part of the KPC Classics Conservation Program — see kpc.bz.
EOF

# 4. Zip (use Windows bsdtar for native zip support)
TAR="C:/Windows/System32/tar.exe"
if [[ -x "$TAR" ]]; then
    (cd "$STAGE" && "$TAR" -a -cf "../../$ZIP" *)
elif command -v zip >/dev/null; then
    (cd "$STAGE" && zip -r "../../$ZIP" .)
else
    echo "no zip tool available (need bsdtar or zip)" >&2
    exit 1
fi

echo "OK -> $ZIP"
ls -la "$ZIP"
"$TAR" -tf "$ZIP" 2>/dev/null || true
