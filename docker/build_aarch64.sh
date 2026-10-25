#!/usr/bin/env bash
# Builds ht2 for aarch64 (PortMaster / AmberELEC) inside docker/Dockerfile.aarch64.
#
#   docker/build_aarch64.sh [extra cmake args...]     -> build-aarch64/ht2
#   docker/build_aarch64.sh --shell                   an interactive shell in the image
#
# gen/ must already exist (scripts/run_lift.py all, on this host): the lifted
# C is the same for every host, so it is not lifted again in the container.
# ../pcrecomp is mounted next to this repo at the same relative path, so
# CMakeLists.txt finds it as it does here.
#
# On an x86 host the container runs under qemu-user, which docker can only use
# once binfmt knows the static qemu with the F (fix-binary) flag:
#   docker run --rm --privileged multiarch/qemu-user-static --reset -p yes
# This script does that when it is missing. The full build then takes tens of
# minutes (qemu); rebuilds after a src/ change take seconds, since gen/ is
# not touched.

set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
pcrecomp="$(cd "$repo/../pcrecomp" && pwd)"
image=ht2-builder-aarch64

[ -d "$repo/gen" ] && ls "$repo"/gen/recomp_0*.c >/dev/null 2>&1 ||
    { echo "gen/ is empty: run scripts/run_lift.py all first" >&2; exit 1; }

if [ "$(uname -m)" != aarch64 ] &&
   ! grep -qs '^flags: .*F' /proc/sys/fs/binfmt_misc/qemu-aarch64; then
    echo "[build_aarch64] registering qemu-user-static for aarch64 containers"
    docker run --rm --privileged multiarch/qemu-user-static --reset -p yes >/dev/null
fi

docker build --platform=linux/arm64 -t "$image" -f "$repo/docker/Dockerfile.aarch64" "$repo/docker"

run=(docker run --rm --platform=linux/arm64 --user "$(id -u):$(id -g)"
     -v "$repo":/workspace/hardtruck2-recomp -v "$pcrecomp":/workspace/pcrecomp
     -w /workspace/hardtruck2-recomp)

if [ "${1:-}" = --shell ]; then
    exec "${run[@]}" -it "$image" bash
fi

"${run[@]}" "$image" bash -c '
    set -e
    cmake -B build-aarch64 -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
    cmake --build build-aarch64 -j"$(nproc)"
    strip -o build-aarch64/ht2.aarch64 build-aarch64/ht2
' bash "$@"

echo "Built: build-aarch64/ht2.aarch64 (stripped; build-aarch64/ht2 keeps its symbols)"
echo "Package: scripts/package_port.sh"
