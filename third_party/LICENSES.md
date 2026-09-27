# Third-party code

| Component | Where | License | Used for |
|---|---|---|---|
| GoogleTest | `third_party/googletest` (submodule, v1.17.0) | BSD-3-Clause | unit tests |
| gfxstream | `third_party/gfxstream` (submodule, ebe5b42) | Apache-2.0 | decoding guest Vulkan/GLES command streams on the host (Phase 5) |
| MoltenVK 1.4.2 + SPIRV-Cross (patched) | built by `tools/build-moltenvk.sh`, bundled in `Kestrel.app/Contents/MacOS/lib64/vulkan` | Apache-2.0 | Vulkan on Metal; patch keeps color attributes on output arrays |
| Vulkan loader | bundled next to MoltenVK (from Homebrew) | Apache-2.0 | loads MoltenVK for gfxstream |
| AOSP Cuttlefish images | downloaded at first launch, not redistributed | Apache-2.0 / GPL-2.0 (kernel) | the Android guest |
| ANGLE (from Electron 43.6.0) | `tools/fetch-angle.sh`, `lib64/gles_angle` (host GLES, opt-in) | BSD-3-Clause | OpenGL ES on Metal |
