#!/bin/bash
# ./build.sh [--target generic|opi5pro] [--platform linux/arm64] [--deb DIR] SOURCE
# SOURCE is a regular clone of Sunshine (not a worktree) with submodules,
# already at the ref to build.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
engine=${ENGINE:-podman}
target=generic
platform=""
deb=""

while [ $# -gt 1 ]; do
  case "$1" in
    --target) target=$2; shift 2 ;;
    --platform) platform=$2; shift 2 ;;
    --deb) deb=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done
src=${1:?source checkout required}

if [ -z "$platform" ]; then
  [ "$target" = opi5pro ] && platform=linux/arm64 || platform=linux/$(uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')
fi

args=(--platform "$platform" --build-context "packaging=$here" --build-arg "TARGET=$target"
      -f "$here/Dockerfile")
tag="sunshine:$target-${platform##*/}"

if [ -n "$deb" ]; then
  mkdir -p "$deb"
  "$engine" build "${args[@]}" --target deb -o "type=local,dest=$deb" "$src"
fi
"$engine" build "${args[@]}" --target runtime -t "$tag" "$src"
echo "$tag"
