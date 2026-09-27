#!/bin/sh
# One-time setup after cloning: tools, submodules, Kestrel's patches, patched MoltenVK.
#   tools/setup.sh && cmake -B build -G Ninja && cmake --build build
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

command -v brew >/dev/null || { echo "Install Homebrew first: https://brew.sh"; exit 1; }
brew install cmake ninja vulkan-loader

git submodule update --init --recursive

# gfxstream (GPU command decoding) needs Kestrel's changes: presenting straight into
# the Mac window, the resize/close crash fix, performance-core threads and more.
if git -C third_party/gfxstream apply --check ../patches/gfxstream-kestrel.patch 2>/dev/null; then
  git -C third_party/gfxstream apply ../patches/gfxstream-kestrel.patch
  echo "gfxstream: patched"
else
  echo "gfxstream: already patched"
fi

# MoltenVK with Kestrel's fixes (Unity shaders, swapchain races). Takes a while once.
tools/build-moltenvk.sh
echo "Ready: cmake -B build -G Ninja && cmake --build build"
