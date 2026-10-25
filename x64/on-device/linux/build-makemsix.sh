#!/usr/bin/env bash
# Build Microsoft's open-source MSIX SDK packer (makemsix, with pack support) on Linux.
# Source: https://github.com/microsoft/msix-packaging (MIT). Pinned commit below.
# Local-only build patch: C++14 -> C++17 so the bundled xerces compiles against
# Debian's ICU 76 headers. No functional change to packing or signing.
#   usage: build-makemsix.sh [SRC_DIR]   (default /workspace/tools/msix-packaging)
#   prints the absolute path of the built makemsix on success.
set -euo pipefail
src=${1:-${MSIX_SRC:-/workspace/tools/msix-packaging}}
pin=25a65f5c1690930813bcc10cdf1d59fa865f2bb1
bin=$src/.vs/bin/makemsix
if [[ -x $bin ]] && "$bin" pack -? 2>&1 | grep -q 'Creates an app package'; then
    echo "$bin"; exit 0
fi
for tool in git cmake make clang clang++; do
    command -v "$tool" >/dev/null || { echo "build-makemsix: missing $tool (apt-get install -y git cmake make clang)" >&2; exit 1; }
done
if [[ ! -d $src/.git ]]; then
    git clone --quiet https://github.com/microsoft/msix-packaging.git "$src"
fi
git -C "$src" fetch --quiet --depth 1 origin "$pin" 2>/dev/null || true
git -C "$src" checkout --quiet "$pin" 2>/dev/null || {
    echo "build-makemsix: WARNING pinned commit $pin unavailable; using $(git -C "$src" rev-parse HEAD)" >&2; }
sed -i 's/set(CMAKE_CXX_STANDARD 14)/set(CMAKE_CXX_STANDARD 17)/' "$src/CMakeLists.txt" "$src/lib/xerces/CMakeLists.txt"
mkdir -p "$src/.vs"
cd "$src/.vs"
cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DSKIP_BUNDLES=off -DUSE_VALIDATION_PARSER=on \
      -DCMAKE_TOOLCHAIN_FILE=../cmake/linux.cmake -DMSIX_PACK=on \
      -DMSIX_SAMPLES=off -DMSIX_TESTS=off -DLINUX=on .. >/dev/null
make -j"$(nproc)" >/dev/null
"$bin" pack -? 2>&1 | grep -q 'Creates an app package'
echo "$bin"
