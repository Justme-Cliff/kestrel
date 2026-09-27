#!/bin/sh
# Builds MoltenVK 1.4.2 with Kestrel's SPIRV-Cross fix (third_party/patches/spirv-cross-kestrel.patch):
# Unity games (Free Fire) use fragment output arrays that stock MoltenVK can't compile.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
tp="$root/third_party"
[ -d "$tp/MoltenVK" ] || git clone -q --depth 1 --branch v1.4.2 https://github.com/KhronosGroup/MoltenVK.git "$tp/MoltenVK"
rev=$(cat "$tp/MoltenVK/ExternalRevisions/SPIRV-Cross_repo_revision")
if [ ! -d "$tp/spirv-cross-kestrel" ]; then
  git clone -q https://github.com/KhronosGroup/SPIRV-Cross.git "$tp/spirv-cross-kestrel"
  git -C "$tp/spirv-cross-kestrel" checkout -q "$rev"
  git -C "$tp/spirv-cross-kestrel" apply "$tp/patches/spirv-cross-kestrel.patch"
fi
cd "$tp/MoltenVK"
# Kestrel fixes to MoltenVK itself (presents racing swapchain destruction).
git apply --check "$tp/patches/moltenvk-kestrel.patch" 2>/dev/null && git apply "$tp/patches/moltenvk-kestrel.patch"
./fetchDependencies --macos --spirv-cross-root "$tp/spirv-cross-kestrel"
make macos
