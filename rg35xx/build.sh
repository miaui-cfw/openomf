#!/bin/sh
# OpenOMF (One Must Fall 2097) for the RG35XX (MiaUI/MinUI): upstream's sources (every change behind RG35XX, the
# rg35xx renderer plugin), a software paletted renderer with a GLES2 present, 320x200 at 2x, 60 fps. Everything is
# relative to this repository; this kit is in rg35xx/.
#
#   build.sh image      the Docker build image from rg35xx/Dockerfile (Debian 12 + the public RG35XX SDK, CMake, Ninja)
#   build.sh deps       libxmp 4.5.0 (downloaded, checked) cross-built static -> rg35xx/build/deps
#   build.sh assets     the game's resources -> rg35xx/build/resources: the freeware OMF 2097 files (openomf-assets.zip,
#                       downloaded, checked), the language files (upstream's languagetool, built for the host) and
#                       upstream's openomf.bk, gamecontrollerdb.txt and licenses
#   build.sh arm        cross-compiles -> rg35xx/build/arm-Release/openomf (+ .stripped); runs deps first if needed
#   build.sh release    arm at -O3 + the pak -> rg35xx/build/pak/OpenOMF.pak (the stripped binary, resources/,
#                       README.md; MiaUI supplies launch.sh); runs assets first if needed
#
# Settings (environment):
#   BT               CMake build type for arm (default Release)
#   OPT              optimisation level added after upstream's (default -O2; release always uses -O3)
#   RES              the resources for release (default rg35xx/build/resources, made by assets)
#   TOOLCHAIN_IMAGE  the build image (default koriki-soh; "build.sh image" creates it)
set -e
K=$(cd "$(dirname "$0")" && pwd)         # this kit (rg35xx/)
R=$(dirname "$K")                        # the repository root
BT=${BT:-Release}
OPT=${OPT:-"-O2"}
RES=${RES:-"$K/build/resources"}
TOOLCHAIN_IMAGE=${TOOLCHAIN_IMAGE:-koriki-soh}
XMP_URL=https://github.com/libxmp/libxmp/releases/download/libxmp-4.5.0/libxmp-4.5.0.tar.gz
XMP_SHA=7847d262112d14e8442f44e5ac6ed9ddbca54c251284720b563c852b31f26e75
ASSETS_URL=https://www.omf2097.com/pub/files/omf/openomf-assets.zip
ASSETS_SHA=de472c786adf9e618bf4b71a5f2cb85bb0b090f27da2b3008f305453a8dea67d
TC=/opt/rg35xx/bin/arm-buildroot-linux-gnueabihf
ARCH="-marm -mcpu=cortex-a9 -mfpu=neon-fp16 -mfloat-abi=hard"
cd "$R"

die() { echo "build.sh: $*" >&2; exit 1; }

# $1 runs in the build image from the repository root (/src)
tc() {
  command -v docker >/dev/null || die "docker not found"
  docker image inspect "$TOOLCHAIN_IMAGE" >/dev/null 2>&1 || die "no Docker image '$TOOLCHAIN_IMAGE': run 'rg35xx/build.sh image' (or set TOOLCHAIN_IMAGE)"
  docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$R":/src -w /src "$TOOLCHAIN_IMAGE" sh -c "set -e; $1"
}

# fetch URL SHA256 FILE: download once into rg35xx/build/dl/, then check it
fetch() {
  mkdir -p rg35xx/build/dl; F="rg35xx/build/dl/$3"
  [ -f "$F" ] || { command -v curl >/dev/null || die "curl not found"; curl -sSfL -o "$F.part" "$1" && mv "$F.part" "$F"; }
  echo "$2  $F" | sha256sum -c --quiet - || { rm -f "$F"; die "$3: checksum mismatch (removed; run again)"; }
}

image() {
  command -v docker >/dev/null || die "docker not found"
  docker build -t "$TOOLCHAIN_IMAGE" - < "$K/Dockerfile"   # (no build context: the Dockerfile copies nothing)
}

# libxmp >= 4.5 (upstream's minimum; the device has 4.4.1), linked in statically
deps() {
  fetch "$XMP_URL" "$XMP_SHA" libxmp-4.5.0.tar.gz
  tc "rm -rf rg35xx/build/deps-src rg35xx/build/deps; mkdir -p rg35xx/build/deps-src rg35xx/build/deps; cd rg35xx/build/deps-src
    tar xzf ../dl/libxmp-4.5.0.tar.gz; cd libxmp-4.5.0
    ./configure --host=arm-buildroot-linux-gnueabihf CC=$TC-gcc AR=$TC-ar RANLIB=$TC-ranlib \
      CFLAGS='-O2 $ARCH' --disable-shared --enable-static --prefix=/src/rg35xx/build/deps >/dev/null
    make -j\$(nproc) >/dev/null && make install >/dev/null; ls -l /src/rg35xx/build/deps/lib/libxmp.a"
}

# the resources the game reads, as an OpenOMF release ships them (the languages come from resources/*.TXT)
assets() {
  fetch "$ASSETS_URL" "$ASSETS_SHA" openomf-assets.zip
  command -v unzip >/dev/null || die "unzip not found"
  rm -rf rg35xx/build/resources; mkdir -p rg35xx/build/resources
  unzip -q -o -j rg35xx/build/dl/openomf-assets.zip -d rg35xx/build/resources
  LT="tools/languagetool/main.c src/formats/language.c src/formats/error.c src/formats/internal/memreader.c
      src/formats/internal/memwriter.c src/formats/internal/reader.c src/formats/internal/writer.c src/utils/cp437.c
      src/utils/path.c src/utils/str.c src/utils/c_string_util.c src/utils/vector.c src/utils/iterator.c
      src/utils/crash.c src/utils/list.c src/vendored/argtable/argtable3.c rg35xx/languagetool-log.c"
  LT=$(echo $LT)   # (one line: it goes into the container's command)
  tc "gcc -O2 -std=gnu11 -Isrc -Isrc/vendored/argtable -o rg35xx/build/languagetool $LT -lm
    L=rg35xx/build/languagetool; D=rg35xx/build/resources
    for l in ENGLISH GERMAN; do \$L -i resources/\${l}2.TXT -o \$D/\$l.DAT2 --check-count 1; done
    \$L -i resources/DANISH.TXT -o \$D/DANISH.LNG --check-count 1013
    \$L -i resources/DANISH2.TXT -o \$D/DANISH.LNG2 --check-count 1"
  cp resources/openomf.bk resources/gamecontrollerdb/gamecontrollerdb.txt resources/icons/openomf.png \
     resources/flatpak/org.openomf.OpenOMF.desktop README.md LICENSE resources/gamecontrollerdb/LICENSE.gamecontrollerdb \
     src/vendored/argtable/LICENSE.argtable3 src/vendored/zip/LICENSE.zip rg35xx/build/resources/
  echo "$(ls rg35xx/build/resources | wc -l) files in rg35xx/build/resources"
}

# the configuration: our renderer (rg35xx: software + GLES2), the SDL audio backend; no mods, zip, opus, upnp,
# natpmp, libconfuse (our own settings file), languages prebuilt by assets
arm() {
  [ -f rg35xx/build/deps/lib/libxmp.a ] || deps
  tc "B=rg35xx/build/arm-$BT; mkdir -p \$B
    cmake -S . -B \$B -G Ninja -DCMAKE_TOOLCHAIN_FILE=/src/rg35xx/rg35xx.cmake -DCMAKE_BUILD_TYPE=$BT \
      -DCMAKE_PROJECT_INCLUDE=/src/rg35xx/rg35xx-opt.cmake -DRG35XX_OPT=$OPT -DOMF_STRIP_GNU_BINARIES=OFF \
      -DOMF_VIDEO_RENDERERS=rg35xx -DOMF_AUDIO_BACKENDS=sdl -DUSE_LIBPNG=ON -DUSE_OPUSFILE=OFF -DUSE_ZIP=OFF \
      -DENABLE_MODMANAGER=OFF -DUSE_MINIUPNPC=OFF -DUSE_NATPMP=OFF -DUSE_CONFUSE=OFF -DBUILD_LANGUAGES=OFF \
      -DUSE_COLORS=OFF -DXMP_INCLUDE_DIR=/src/rg35xx/build/deps/include -DXMP_LIBRARY=/src/rg35xx/build/deps/lib/libxmp.a >/dev/null
    ninja -C \$B openomf
    $TC-strip -o \$B/openomf.stripped \$B/openomf; ls -l \$B/openomf \$B/openomf.stripped"
}

release() {
  if [ ! -f "$RES/MAIN.BK" ] && [ "$RES" = "$K/build/resources" ]; then assets; fi
  [ -f "$RES/MAIN.BK" ] && [ -f "$RES/ENGLISH.DAT2" ] || die "no OMF 2097 resources in $RES: run 'rg35xx/build.sh assets' (or set RES=...)"
  OPT=-O3; BT=Release; arm
  D="rg35xx/build/pak/OpenOMF.pak"
  rm -rf rg35xx/build/pak; mkdir -p "$D/resources"
  cp rg35xx/build/arm-Release/openomf.stripped "$D/openomf"
  cp "$RES"/* "$D/resources/"
  cp .github/README.md "$D/README.md"
  du -sh "$D"; ls -l "$D"
}

help() { sed -n '2,19p' "$K/build.sh" | sed 's/^# \{0,1\}//'; }

case "$1" in
  image|deps|assets|arm|release) "$1" ;;
  ""|-h|--help|help) help ;;
  *) help; die "unknown command '$1'" ;;
esac
