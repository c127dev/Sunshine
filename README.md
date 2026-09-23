# compiler

Builds Sunshine `.deb` packages and container images from `master` plus the
patches under `patches/`.

| Target | Source | Patches | Platforms | Image tags |
| --- | --- | --- | --- | --- |
| `generic` | `master` (input `source_ref`) | `patches/common` | amd64, arm64, riscv64 | `<version>`, `latest`, `sha-<short>` |
| `opi5pro` | `master` (input `source_ref`) | `patches/common`, `patches/opi5pro` | arm64 | `<version>-opi5pro`, `opi5pro` |

Images go to `ghcr.io/<owner>/sunshine`, and to `docker.io/$DOCKERHUB_USERNAME/sunshine`
when the `DOCKERHUB_USERNAME` and `DOCKERHUB_TOKEN` secrets are set. Every run
publishes a release `vYYYY.MM.DD.<run number>.<branch>` with one `.deb` per
target and architecture.

riscv64 runs under QEMU. LizardByte/build-deps releases no riscv64 FFmpeg, so
the `ffmpeg-riscv64` job compiles it once into
`ghcr.io/<owner>/sunshine-ffmpeg:<build-deps sha>-<patches hash>-riscv64` and
later runs reuse that image until build-deps or `patches/build-deps` changes.
A failed riscv64 build does not stop the other images or the release.

## Run

From the Actions tab with this branch selected, or:

```bash
curl -X POST \
    -H "Accept: application/vnd.github+json" \
    -H "Authorization: Bearer $GH_TOKEN" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    https://api.github.com/repos/c127dev/Sunshine/actions/workflows/c127-build.yml/dispatches \
    -d '{"ref":"compiler","inputs":{"source_ref":"master"}}'
```

`release: false` builds and uploads the `.deb` artifacts only.

## Local build

The source must be a regular clone (not a worktree) with submodules, at the
ref to build:

```bash
git clone --recurse-submodules --shallow-submodules https://github.com/c127dev/Sunshine.git sunshine
./build.sh --deb out sunshine                          # generic, host arch
./build.sh --platform linux/riscv64 sunshine           # generic, emulated
./build.sh --target opi5pro --deb out sunshine
```

`ENGINE=docker` switches from podman.

## Patches

Each target applies the directories in its `PATCH_DIRS` to `master` with
`git apply`, in order. `patches/build-deps` applies to
`third-party/build-deps` when FFmpeg is compiled. To refresh one:

```bash
git -C sunshine am ../patches/common/*.patch   # resolve, then
git -C sunshine format-patch -o ../patches/common origin/master
```

## Run the image

```bash
podman run --rm -it --read-only --name sunshine \
    --device /dev/dri --device /dev/uinput \
    --security-opt label=disable --security-opt seccomp=unconfined \
    -e HOSTNAME="$(hostname)" \
    -v "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY:/tmp/wayland-0" \
    -e XDG_RUNTIME_DIR=/tmp -e WAYLAND_DISPLAY=wayland-0 \
    -v "$XDG_RUNTIME_DIR/pulse:/tmp/pulse" -e PULSE_SERVER=unix:/tmp/pulse/native \
    -p 47984-47990:47984-47990/tcp -p 48010:48010/tcp \
    -p 47998-48000:47998-48000/udp -p 48002:48002/udp -p 48010:48010/udp \
    ghcr.io/c127dev/sunshine:latest
```

On the Orange Pi 5 Pro use `:opi5pro` and add
`--device /dev/mpp_service --device /dev/dma_heap --group-add keep-groups --security-opt unmask=/sys/firmware`.
MPP detects the SoC from `/proc/device-tree/compatible`; with `/sys/firmware`
masked it reports an unknown SoC and offers no HEVC.
