#!/usr/bin/env bash
# Builds Pet-GPT inside the Freedesktop 26.08 SDK and packages it as a single-file
# Flatpak bundle in dist/linux/.
#
#   bash tools/package-flatpak.sh              release bundle, no ROMs:
#                                              dist/linux/Pet-GPT-x86_64.flatpak
#   bash tools/package-flatpak.sh --testing    also bundles your local ROM sets
#     [ROM_DIR]                                (default x64/Release/roms) for your
#                                              own machines ONLY - never publish it:
#                                              dist/linux/Pet-GPT-testing-x86_64.flatpak
#
# Building inside the SDK (not on the host) makes the binary depend only on the
# runtime, so it runs on SteamOS whatever glibc the build machine has. The build
# directory lives under $PET_FLATPAK_WORK (default ~/pet-gpt-flatpak) on the Linux
# filesystem, because building wxWidgets through /mnt/c is slow.
#
# wxWidgets: set PET_WX_SOURCE to a local source tree to use it (it is mounted into
# the SDK); without it CMake downloads the pinned release, so the SDK gets network
# access for the configure step.
#
# Install and run on the target:
#   flatpak install --user Pet-GPT-x86_64.flatpak
#   flatpak run io.github.tcottrill.Pet-GPT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ID=io.github.tcottrill.Pet-GPT
# Keep SDK, runtime metadata and compiled dependency caches on the same branch.
# 26.08 is supported through September 2028; review before that date.
RUNTIME_VERSION=26.08
SDK="org.freedesktop.Sdk//$RUNTIME_VERSION"
WORK="${PET_FLATPAK_WORK:-$HOME/pet-gpt-flatpak}"
BUILD="$WORK/build-$RUNTIME_VERSION"
OUT="$ROOT/dist/linux"

TESTING=0
ROMS=""
if [ "${1:-}" = "--testing" ]; then
    TESTING=1
    ROMS="${2:-$ROOT/x64/Release/roms}"
    for model in pet2001 pet2001n pet4000-9 pet4000-12 cbm8032; do
        [ -d "$ROMS/$model" ] || { echo "No ROM set $ROMS/$model" >&2; exit 1; }
    done
fi
NAME=Pet-GPT
[ "$TESTING" = 1 ] && NAME=Pet-GPT-testing
mkdir -p "$WORK" "$OUT"

# 1. Build the binary inside the SDK.
mounts=(--filesystem="$ROOT" --filesystem="$WORK")
wx_arg=""
if [ -n "${PET_WX_SOURCE:-}" ]; then
    mounts+=(--filesystem="$PET_WX_SOURCE")
    wx_arg="-DPET_WX_SOURCE=$PET_WX_SOURCE"
fi
flatpak run "${mounts[@]}" --share=network --command=bash "$SDK" -c "
    set -e
    cmake -S '$ROOT' -B '$BUILD' -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF $wx_arg
    cmake --build '$BUILD' --target pet-gpt --parallel \${FLATPAK_BUILDER_N_JOBS:-\$(nproc)}
"

# 2. Stage the app.
STAGE="$(mktemp -d "$WORK/stage-XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
flatpak build-init "$STAGE" "$ID" org.freedesktop.Sdk org.freedesktop.Platform "$RUNTIME_VERSION"
install -Dm755 "$BUILD/bin/pet-gpt" "$STAGE/files/bin/pet-gpt"
# /app/share/pet-gpt is the resource folder (../share/pet-gpt from the binary).
# Your own ROMs in ~/Pet-GPT/roms always take precedence over anything here.
mkdir -p "$STAGE/files/share/pet-gpt"
if [ "$TESTING" = 1 ]; then
    cp -r "$ROMS" "$STAGE/files/share/pet-gpt/roms"
    find "$STAGE/files/share/pet-gpt/roms" -type f ! -name '*.bin' -delete
fi
install -Dm644 "$ROOT/packaging/$ID.desktop" "$STAGE/files/share/applications/$ID.desktop"
install -Dm644 "$ROOT/packaging/$ID.metainfo.xml" "$STAGE/files/share/metainfo/$ID.metainfo.xml"
mkdir -p "$STAGE/files/share/icons"
cp -r "$ROOT/packaging/icons/hicolor" "$STAGE/files/share/icons/"
LICENSES="$STAGE/files/share/licenses/pet-gpt"
mkdir -p "$LICENSES"
for f in "$ROOT/LICENSE" "$ROOT/COPYING"; do [ -f "$f" ] && install -m644 "$f" "$LICENSES/"; done
WX_LICENCE="${PET_WX_SOURCE:-$BUILD/_deps/wx-src}/docs/licence.txt"
[ -f "$WX_LICENCE" ] && install -m644 "$WX_LICENCE" "$LICENSES/wxWidgets.txt"

# 3. Check the metadata the way Flathub would.
flatpak run --filesystem="$STAGE" --command=bash "$SDK" -c "
    set -e
    desktop-file-validate '$STAGE/files/share/applications/$ID.desktop'
    if command -v appstreamcli >/dev/null; then
        appstreamcli validate --no-net --explain '$STAGE/files/share/metainfo/$ID.metainfo.xml'
    fi
"

# 4. Permissions. Same reasoning as AAE's manifest (packaging/flatpak in AAE):
#   wayland + x11 (not fallback-x11): native Wayland on the desktop, X11 for
#     gamescope in Steam Game Mode, which fallback-x11 would drop.
#   dri: OpenGL. pulseaudio + pipewire-0: sound (native PipeWire first, ALSA
#     through the pulse socket as the fallback).
#   device=all: /dev/input for the evdev gamepad backend, plus hidraw pads.
#   ~/Pet-GPT:create: programs, ROMs and screenshots (system/portable_paths.cpp).
#   home:ro and /run/media:ro: load a program or disk from anywhere (Downloads,
#     an SD card); Pet-GPT never writes there.
flatpak build-finish --command=pet-gpt \
    --socket=wayland --socket=x11 --share=ipc --device=dri \
    --socket=pulseaudio --filesystem=xdg-run/pipewire-0:ro \
    --device=all \
    --filesystem=~/Pet-GPT:create --filesystem=home:ro --filesystem=/run/media:ro \
    "$STAGE"

# 5. Export and bundle.
BRANCH=preview
[ "$TESTING" = 1 ] && BRANCH=testing
flatpak build-export "$WORK/repo" "$STAGE" "$BRANCH"
flatpak build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo \
    "$WORK/repo" "$OUT/$NAME-x86_64.flatpak" "$ID" "$BRANCH"
(cd "$OUT" && sha256sum "$NAME-x86_64.flatpak" > "$NAME-x86_64.flatpak.sha256")
echo "Flatpak: $OUT/$NAME-x86_64.flatpak (branch $BRANCH)"
[ "$TESTING" = 1 ] && echo "This bundle contains ROMs: keep it to your own machines."
exit 0
