#!/bin/bash
# librga and librockchip_mpp from the nyanmisaka forks, as on the board.
set -euo pipefail

work=$(mktemp -d)
git clone -b jellyfin-rga --depth=1 https://github.com/nyanmisaka/rk-mirrors.git "$work/rga"
meson setup "$work/rga/build" "$work/rga" --prefix=/usr --libdir=lib --buildtype=release \
  --default-library=shared -Dcpp_args=-fpermissive -Dlibdrm=true -Dlibrga_demo=false
ninja -C "$work/rga/build" install

git clone -b jellyfin-mpp --depth=1 https://github.com/nyanmisaka/rk-mirrors.git "$work/mpp"
cmake -S "$work/mpp" -B "$work/mpp/build" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DBUILD_TEST=OFF
cmake --build "$work/mpp/build" -j "$(nproc)"
cmake --install "$work/mpp/build"

ldconfig
rm -rf "$work"
