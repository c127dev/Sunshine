#!/bin/bash
# Runs inside the build stage: /src is a Sunshine checkout with submodules,
# /packaging is this branch. Leaves /out/sunshine.deb.
set -euo pipefail

TARGET="${TARGET:-generic}"
PKG=/packaging
# shellcheck source=/dev/null
source "$PKG/targets/$TARGET.env"

export CC CXX
export CFLAGS="-O3 ${TUNE}"
export CXXFLAGS="-O3 ${TUNE}"
export CCACHE_DIR=/ccache CCACHE_MAXSIZE=10G
arch=$(uname -m)
echo "target=$TARGET arch=$arch ffmpeg=$FFMPEG cc=$CC tune=${TUNE:-none}"

cd /src

for pin in ${SUBMODULE_PINS:-}; do
  path=${pin%%=*}
  sha=${pin#*=}
  url=$(git config -f .gitmodules "submodule.$path.url")
  echo "--- pin $path at $sha ---"
  rm -rf "$path"
  git clone -q "$url" "$path"
  git -C "$path" checkout -q "$sha"
  git -C "$path" submodule update -q --init --recursive
done

for patch in "$PKG/$PATCH_DIR"/*.patch; do
  echo "--- apply $(basename "$patch") ---"
  git apply --verbose "$patch"
done

sunshine_flags=(
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX=/usr
  -DCMAKE_C_COMPILER_LAUNCHER=ccache
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  -DBUILD_DOCS=OFF
  -DBUILD_TESTS=OFF
  -DSUNSHINE_ASSETS_DIR=share/sunshine
  -DSUNSHINE_EXECUTABLE_PATH=/usr/bin/sunshine
  -DSUNSHINE_BUILD_HOMEBREW=OFF
  -DSUNSHINE_ENABLE_CUDA=OFF
  -DSUNSHINE_ENABLE_DRM=ON
  -DSUNSHINE_ENABLE_KWIN=OFF
  -DSUNSHINE_ENABLE_TRAY=OFF
  -DSUNSHINE_ENABLE_VULKAN=ON
  -DSUNSHINE_ENABLE_WAYLAND=ON
  -DSUNSHINE_ENABLE_X11=ON
  -DGLAD_SKIP_PIP_INSTALL=ON
)

if [ "$FFMPEG" = prebuilt ] && [ "$arch" != x86_64 ] && [ "$arch" != aarch64 ]; then
  echo "no prebuilt FFmpeg for $arch, building from source"
  FFMPEG=source
fi

if [ "$FFMPEG" != prebuilt ]; then
  deps=/tmp/build-deps
  cp -a third-party/build-deps "$deps"
  cd "$deps"
  for patch in "$PKG"/patches/build-deps/*.patch; do
    echo "--- apply build-deps $(basename "$patch") ---"
    git apply --verbose "$patch"
  done

  deps_flags=(
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=/ffmpeg-dist
    -DCMAKE_C_COMPILER_LAUNCHER=ccache
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    -DBUILD_FFMPEG_AMF=OFF
    -DBUILD_FFMPEG_NV_CODEC_HEADERS=OFF
  )

  if [ "$FFMPEG" = rockchip ]; then
    ff=third-party/FFmpeg/FFmpeg
    echo "--- FFmpeg from nyanmisaka/ffmpeg-rockchip@8.1 ---"
    rm -rf "$ff"
    git clone -q --depth 1 -b 8.1 https://github.com/nyanmisaka/ffmpeg-rockchip.git "$ff"
    python3 - cmake/ffmpeg/ffmpeg.cmake <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
anchor = "        --enable-swscale\n"
if anchor not in src:
    sys.exit("anchor '--enable-swscale' not found in " + path)
inject = (
    "        --enable-version3\n"
    "        --enable-libdrm\n"
    "        --enable-rkmpp\n"
    "        --enable-rkrga\n"
    "        --enable-encoder=h264_rkmpp,hevc_rkmpp,mjpeg_rkmpp\n"
)
open(path, "w").write(src.replace(anchor, anchor + inject, 1))
PY
    # VAAPI, Vulkan and SVT-AV1 are unusable on RK3588.
    deps_flags+=(-DBUILD_FFMPEG_LIBVA=OFF -DBUILD_FFMPEG_VULKAN=OFF -DBUILD_FFMPEG_SVT_AV1=OFF)
    sunshine_flags+=(
      -DSUNSHINE_ENABLE_RKMPP=ON
      # libavcodec.a references mpp/rga, and Sunshine links the archives directly.
      "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--no-as-needed -lrockchip_mpp -lrga -Wl,--as-needed"
    )
  fi

  cmake -B build -S . "${deps_flags[@]}"
  cmake --build build -j "$(nproc)"
  cmake --install build
  sunshine_flags+=(-DFFMPEG_PREPARED_BINARIES=/ffmpeg-dist/ffmpeg)
  cd /src
fi

cmake -B build -S . "${sunshine_flags[@]}"
cmake --build build -j "$(nproc)"
(cd build && cpack -G DEB)

mkdir -p /out
find build -maxdepth 2 -name '*.deb' -exec cp {} /out/sunshine.deb \;
test -s /out/sunshine.deb
ccache -s
