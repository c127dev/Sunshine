# syntax=docker/dockerfile:1.7
# Build context: a Sunshine checkout with submodules. This branch is mounted at
# /packaging, so it never has to be part of the context.
ARG BASE_IMAGE=ubuntu:24.04
# An image built from the ffmpeg stage, reused instead of compiling FFmpeg.
ARG FFMPEG_IMAGE=toolchain

FROM ${BASE_IMAGE} AS toolchain
ARG TARGET=generic
ARG NODE_VERSION=22.23.2
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf automake build-essential ca-certificates ccache cmake curl file \
    g++-14 gcc-14 git glslang-tools libtool meson nasm ninja-build pkg-config \
    python3 python3-jinja2 python3-setuptools xz-utils yasm \
    libayatana-appindicator3-dev libboost-filesystem-dev libboost-locale-dev \
    libboost-log-dev libboost-program-options-dev libboost-thread-dev \
    libcap-dev libcurl4-openssl-dev libdrm-dev libevdev-dev libgbm-dev \
    libicu-dev libminiupnpc-dev libnotify-dev libnuma-dev libopus-dev \
    libpipewire-0.3-dev libpulse-dev libsdl2-dev libssl-dev libsystemd-dev \
    libudev-dev libva-dev libvdpau-dev libvulkan-dev libwayland-dev \
    libx11-dev libx11-xcb-dev libxcb-dri3-dev libxcb-shape0-dev \
    libxcb-shm0-dev libxcb-xfixes0-dev libxcb1-dev libxcursor-dev \
    libxfixes-dev libxi-dev libxinerama-dev libxrandr-dev libxtst-dev \
    wayland-protocols \
 && rm -rf /var/lib/apt/lists/*
# Ubuntu 24.04 ships Node 18; the web UI needs >= 20.19. riscv64 only has
# unofficial builds.
RUN set -eux; \
    case "$(uname -m)" in \
      x86_64)  a=x64;     base=https://nodejs.org/dist ;; \
      aarch64) a=arm64;   base=https://nodejs.org/dist ;; \
      riscv64) a=riscv64; base=https://unofficial-builds.nodejs.org/download/release ;; \
      *) echo "no Node.js build for $(uname -m)" >&2; exit 1 ;; \
    esac; \
    curl -fsSL "$base/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-$a.tar.xz" \
      | tar -xJ -C /usr/local --strip-components=1; \
    node --version
RUN --mount=type=bind,from=packaging,target=/packaging \
    if grep -q '^RKMPP=ON' "/packaging/targets/$TARGET.env"; then /packaging/scripts/rockchip-libs.sh; fi

FROM ${FFMPEG_IMAGE} AS ffmpeg-cache

FROM toolchain AS ffmpeg-build
ARG TARGET=generic
RUN --mount=type=bind,target=/src-ro \
    --mount=type=bind,from=packaging,target=/packaging \
    --mount=type=cache,target=/ccache \
    cp -a /src-ro /src && TARGET="$TARGET" FFMPEG_ONLY=1 /packaging/scripts/sunshine-builder.sh

FROM scratch AS ffmpeg
COPY --from=ffmpeg-build /ffmpeg-dist /ffmpeg-dist

FROM toolchain AS build
ARG TARGET=generic
# Read by cmake/prep/build_version.cmake; unset means 0.0.0.
ARG BUILD_VERSION=""
ARG BRANCH=""
ARG COMMIT=""
RUN --mount=type=bind,target=/src-ro \
    --mount=type=bind,from=packaging,target=/packaging \
    --mount=type=bind,from=ffmpeg-cache,target=/ffmpeg-cache \
    --mount=type=cache,target=/ccache \
    cp -a /src-ro /src && TARGET="$TARGET" BUILD_VERSION="$BUILD_VERSION" BRANCH="$BRANCH" COMMIT="$COMMIT" /packaging/scripts/sunshine-builder.sh

FROM scratch AS deb
COPY --from=build /out/ /

FROM ${BASE_IMAGE} AS runtime
ARG TARGET=generic
ENV DEBIAN_FRONTEND=noninteractive
RUN --mount=type=bind,from=build,source=/out,target=/out \
    --mount=type=bind,from=build,source=/usr/lib,target=/buildlib \
    set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends /out/sunshine.deb \
      libegl1 libgl1-mesa-dri libvulkan1 mesa-va-drivers; \
    if [ -e /buildlib/librockchip_mpp.so ]; then \
      cp -a /buildlib/librockchip_mpp.so* /buildlib/librga.so* /usr/lib/; \
      ldconfig; \
    fi; \
    rm -rf /var/lib/apt/lists/*
CMD ["/usr/bin/sunshine"]
