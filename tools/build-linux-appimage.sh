#!/usr/bin/env bash
# Builds dist/linux/Pet-GPT-x86_64.AppImage, the Linux release for systems without Flatpak.
#
# The build runs inside an Ubuntu 22.04 userland (glibc 2.35, GTK 3.24) started with
# bubblewrap, so the AppImage runs on Ubuntu 22.04 and newer even when this machine is
# newer. No root is needed: only bash, curl, tar and bwrap with unprivileged user
# namespaces. Every download is pinned by version and SHA-256. (The same method as
# tools/build-linux-appimage.sh in FreeSnip and Skein Downloader.)
#
# wxWidgets 3.3.4 is linked statically. GTK 3 and the libraries it brings (GLib/GIO,
# Pango, Cairo, Wayland, X11, libpng...), OpenGL/EGL, ALSA and PipeWire come from the
# system: every Ubuntu desktop and SteamOS has them, and the system copies keep the
# desktop's theme, input methods, native Wayland, GPU driver and sound setup, which
# bundled copies would lose. Downloads are cached in $PET_APPIMAGE_WORK (default
# ~/pet-gpt-appimage); delete it to start from scratch.
#
# PET_TEST_ROM_DIR (default x64/Release/roms): your ROM sets, mounted read-only for the
# CTest run and the smoke test only. They never go into the AppImage. Without them the
# ROM-dependent tests are skipped and the smoke test is left out.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${PET_APPIMAGE_WORK:-$HOME/pet-gpt-appimage}"
CACHE="$WORK/cache"
ROOTFS="$WORK/rootfs-ubuntu-22.04"
OUT="$ROOT/dist/linux"
APPIMAGE="Pet-GPT-x86_64.AppImage"
ID=io.github.tcottrill.Pet-GPT
ROMS="${PET_TEST_ROM_DIR:-$ROOT/x64/Release/roms}"
WX=wxWidgets-3.3.4
mkdir -p "$CACHE" "$WORK/build" "$OUT"

# fetch URL FILE SHA256: download into the cache once and always verify.
fetch() {
  local file="$CACHE/$2"
  if [ ! -f "$file" ] || ! echo "$3  $file" | sha256sum -c --status; then
    curl -fL --retry 3 -o "$file.part" "$1"
    mv "$file.part" "$file"
  fi
  echo "$3  $file" | sha256sum -c --quiet || { echo "Checksum mismatch: $2" >&2; exit 1; }
}
fetch https://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release/ubuntu-base-22.04.5-base-amd64.tar.gz \
  ubuntu-base-22.04.5.tar.gz 242cd8898b33ea806ef5f13b1076ed7c76f9f989d18384452f7166692438ff1a
fetch https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage \
  linuxdeploy-x86_64.AppImage c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
# The same release and checksum as cmake/Dependencies.cmake.
fetch https://github.com/wxWidgets/wxWidgets/releases/download/v3.3.4/$WX.tar.bz2 \
  $WX.tar.bz2 815c16cafa4fcbfcf25c5c0a3418309c62257b8a3b6f91d85a1367a134f98c98

if [ ! -f "$ROOTFS/.extracted" ]; then
  rm -rf "$ROOTFS"; mkdir -p "$ROOTFS"
  tar -xzf "$CACHE/ubuntu-base-22.04.5.tar.gz" -C "$ROOTFS" --no-same-owner --exclude='./dev/*'
  touch "$ROOTFS/.extracted"
fi
if [ ! -f "$WORK/build/$WX/.extracted" ]; then
  rm -rf "$WORK/build/$WX"
  tar -xjf "$CACHE/$WX.tar.bz2" -C "$WORK/build"
  touch "$WORK/build/$WX/.extracted"
fi
rm -f "$ROOTFS/etc/resolv.conf"; cp -L /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
mkdir -p "$ROOTFS/src" "$ROOTFS/cache" "$ROOTFS/build" "$ROOTFS/out" "$ROOTFS/roms"

HAVE_ROMS=0
ROM_MOUNT=()
if [ -d "$ROMS/pet2001n" ]; then
  HAVE_ROMS=1
  ROM_MOUNT=(--ro-bind "$ROMS" /roms)
else
  echo "No ROM sets in $ROMS: ROM tests and the smoke test are skipped" >&2
fi

# inroot SCRIPT: run as (namespaced) root inside the 22.04 userland.
inroot() {
  bwrap --unshare-user --uid 0 --gid 0 --bind "$ROOTFS" / --dev /dev --proc /proc --tmpfs /tmp \
    --ro-bind "$ROOT" /src --bind "$CACHE" /cache --bind "$WORK/build" /build --bind "$OUT" /out \
    "${ROM_MOUNT[@]}" \
    --setenv HOME /root --setenv DEBIAN_FRONTEND noninteractive --setenv LANG C.UTF-8 \
    --setenv PATH /opt/venv/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    --setenv ID "$ID" --setenv APPIMAGE_NAME "$APPIMAGE" --setenv WX "$WX" --setenv HAVE_ROMS "$HAVE_ROMS" \
    bash -euo pipefail -c "$1"
}

if [ ! -f "$ROOTFS/.deps-v1" ]; then
  inroot '
    # apt cannot switch to its _apt user inside a user namespace, and only one uid/gid
    # is mapped there, so package scripts that chown to other groups (e.g. staff) would
    # fail. Ownership is irrelevant in this throwaway build root, so those calls may fail.
    echo "APT::Sandbox::User \"root\";" > /etc/apt/apt.conf.d/99namespace
    for tool in chown chgrp; do
      printf "#!/bin/sh\n/bin/%s \"\$@\" 2>/dev/null || true\n" "$tool" > /usr/local/sbin/$tool; chmod 755 /usr/local/sbin/$tool
    done
    # Some setup scripts still set ownership the namespace cannot grant, through dpkg-statoverride
    # (dbus) or an absolute /bin/chown (fontconfig-config) that the wrappers above miss. Those
    # steps do not matter here: nothing runs a D-Bus daemon or uses a system font cache. settle
    # skips only the scripts that actually failed (their packages are left half-configured,
    # "iF"), then configures again; dpkg --audit must end up clean.
    settle() {
      for attempt in 1 2 3 4; do
        dpkg --configure -a && return 0
        for pkg in $(dpkg-query -W -f="\${db:Status-Abbrev}|\${binary:Package}\n" | grep "^iF" | cut -d"|" -f2); do
          for script in /var/lib/dpkg/info/$pkg.postinst /var/lib/dpkg/info/${pkg%%:*}.postinst; do
            if [ -f "$script" ]; then echo "Skipping the setup script of $pkg"; printf "#!/bin/sh\nexit 0\n" > "$script"; fi
          done
        done
      done
      return 1
    }
    settle
    apt-get update
    # OpenGL/EGL and ALSA headers for the emulator; Mesa and xvfb/xauth run the tests and
    # the smoke test without a display or GPU. No PipeWire: 22.04'"'"'s 0.3.48 headers are too
    # old for the native backend, so the AppImage plays through ALSA, which PipeWire and
    # PulseAudio desktops route to their own server (the Flatpak has native PipeWire).
    PACKAGES="ca-certificates g++-12 ninja-build pkg-config python3 python3-venv file
      libgtk-3-dev libpng-dev zlib1g-dev libgl-dev libegl-dev libgl1-mesa-dri libegl-mesa0
      libasound2-dev libwayland-dev wayland-protocols libxkbcommon-dev xvfb xauth"
    apt-get install -y --no-install-recommends $PACKAGES || { settle; apt-get install -y --no-install-recommends $PACKAGES; }
    test -z "$(dpkg --audit)" || { dpkg --audit; exit 1; }
    python3 -m venv /opt/venv
    pip install --no-cache-dir cmake==3.31.6
  '
  touch "$ROOTFS/.deps-v1"
fi

inroot '
  ROM_TESTS=""
  [ "$HAVE_ROMS" = 1 ] && ROM_TESTS=/roms
  cmake -S /src -B /build/app -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-12 \
    -DCMAKE_CXX_COMPILER=g++-12 -DCMAKE_INSTALL_PREFIX=/usr -DPETGPT_OUTPUT_DIR=/build/app/dist \
    -DPET_WX_SOURCE=/build/$WX -DBUILD_TESTING=ON "-DPET_TEST_ROM_DIR=$ROM_TESTS" -DPET_USE_PIPEWIRE=OFF
  cmake --build /build/app

  # The tests, on the oldest supported base. frontend_tests needs isolated data with
  # pet.ini and all five ROM sets (it reports a skip without them).
  if [ "$HAVE_ROMS" = 1 ]; then
    mkdir -p /tmp/test-data && cp -r /roms /tmp/test-data/roms && touch /tmp/test-data/pet.ini
    export PETGPT_DATA_DIR=/tmp/test-data
  fi
  xvfb-run -a ctest --test-dir /build/app --output-on-failure
  unset PETGPT_DATA_DIR

  rm -rf /build/AppDir /build/tools
  DESTDIR=/build/AppDir cmake --install /build/app
  # The native install'"'"'s own desktop file (Icon=pet-gpt); the AppImage uses the app ID one.
  rm -f /build/AppDir/usr/share/applications/pet-gpt.desktop
  mkdir -p /build/tools
  LICENSES=/build/AppDir/usr/share/licenses/pet-gpt; mkdir -p "$LICENSES"
  install -m644 /src/LICENSE "$LICENSES/LICENSE"
  install -m644 /src/THIRD-PARTY-NOTICES.md "$LICENSES/THIRD-PARTY-NOTICES.md"
  install -m644 /build/$WX/docs/licence.txt "$LICENSES/wxWidgets.txt"
  install -Dm644 /src/packaging/$ID.metainfo.xml /build/AppDir/usr/share/metainfo/$ID.appdata.xml

  install -m755 /cache/linuxdeploy-x86_64.AppImage /build/tools/
  export APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 LDAI_OUTPUT=/build/$APPIMAGE_NAME
  # The desktop'"'"'s own GTK, GL, sound and display stack is used (see the top of this script).
  EXCLUDES=()
  for lib in "libgtk-3.so*" "libgdk-3.so*" "libgdk_pixbuf-2.0.so*" "libglib-2.0.so*" "libgobject-2.0.so*" \
      "libgio-2.0.so*" "libgmodule-2.0.so*" "libgthread-2.0.so*" "libpango-1.0.so*" "libpangocairo-1.0.so*" \
      "libpangoft2-1.0.so*" "libcairo.so*" "libcairo-gobject.so*" "libatk-1.0.so*" "libatk-bridge-2.0.so*" \
      "libatspi.so*" "libepoxy.so*" "libharfbuzz.so*" "libfribidi.so*" "libpixman-1.so*" "libpng16.so*" \
      "libwayland-*.so*" "libxkbcommon.so*" "libX*.so*" "libxcb*.so*" "libdbus-1.so*" "libffi.so*" \
      "libpcre*.so*" "libmount.so*" "libblkid.so*" "libselinux.so*" "libsystemd.so*" \
      "libGL.so*" "libGLX.so*" "libGLdispatch.so*" "libEGL.so*" "libOpenGL.so*" "libgbm.so*" "libdrm.so*" \
      "libasound.so*" "libpipewire-0.3.so*" "libfontconfig.so*" "libfreetype.so*"; do
    EXCLUDES+=(--exclude-library "$lib")
  done
  rm -f /build/$APPIMAGE_NAME
  cd /build/tools && ./linuxdeploy-x86_64.AppImage --appdir /build/AppDir "${EXCLUDES[@]}" \
    -e /build/AppDir/usr/bin/pet-gpt \
    -d /src/packaging/$ID.desktop \
    -i /src/packaging/icons/hicolor/256x256/apps/$ID.png
  # The AppImage file'"'"'s own icon (.DirIcon, used by file managers): linuxdeploy links the
  # 64 px image, so the 256 px one replaces it before the second pass packs the AppImage.
  rm -f /build/AppDir/$ID.png /build/AppDir/.DirIcon
  install -m644 /src/packaging/icons/hicolor/256x256/apps/$ID.png /build/AppDir/$ID.png
  ln -s $ID.png /build/AppDir/.DirIcon
  ./linuxdeploy-x86_64.AppImage --appdir /build/AppDir "${EXCLUDES[@]}" --output appimage

  # Smoke test on the oldest supported base: a PET boots, runs 120 frames and saves a frame.
  if [ "$HAVE_ROMS" = 1 ]; then
    rm -rf /tmp/smoke-data && mkdir -p /tmp/smoke-data && cp -r /roms /tmp/smoke-data/roms && touch /tmp/smoke-data/pet.ini
    PETGPT_DATA_DIR=/tmp/smoke-data APPIMAGE_EXTRACT_AND_RUN=1 xvfb-run -a /build/$APPIMAGE_NAME \
      --smoke-test=/tmp/smoke-data/smoke.png -window
    test -s /tmp/smoke-data/smoke.png
    echo "Smoke test passed"
  fi
  install -m755 /build/$APPIMAGE_NAME /out/$APPIMAGE_NAME
'
(cd "$OUT" && sha256sum "$APPIMAGE" > "$APPIMAGE.sha256")
echo "AppImage: $OUT/$APPIMAGE ($(du -h "$OUT/$APPIMAGE" | cut -f1))"
