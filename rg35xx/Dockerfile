# The RG35XX build image for OpenOMF, self-contained ("rg35xx/build.sh image" builds it as koriki-soh):
# - the same base as MiaUI's koriki-toolchain: Debian 12 and the public Batocera RG35XX SDK (buildroot GCC 12.2, armhf,
#   SDL2, SDL2_mixer, enet, libpng and GLES2/EGL in the sysroot), relocated to /opt/rg35xx;
# - CMake 3.30 and Ninja on top (the same image as the MiaUI Shipwright, Sonic Mania and OpenTyrian ports).
FROM docker.io/debian:bookworm-slim

ARG SDK_URL=https://github.com/rg35xx-cfw/rg35xx-cfw.github.io/releases/download/rg35xx_batocera_sdk_alpha_0.5/arm-buildroot-linux-gnueabihf_sdk-buildroot.tar.gz
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y make gcc g++ cmake python3 file wget cpio zip unzip rsync bc git autoconf curl \
 && apt-get install -y --no-install-recommends ninja-build lsb-release zlib1g-dev libbz2-dev libpng-dev ca-certificates \
 && apt-get -y autoremove && apt-get -y clean && rm -rf /var/lib/apt/lists/*

RUN curl -sSfL "$SDK_URL" | tar -xzC /opt \
 && mv "/opt/$(ls /opt)" /opt/rg35xx \
 && /opt/rg35xx/relocate-sdk.sh

RUN curl -sSfL https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-linux-x86_64.tar.gz | tar -xz -C /opt \
 && ln -sf /opt/cmake-3.30.5-linux-x86_64/bin/* /usr/local/bin/

RUN git config --system --add safe.directory "*"
