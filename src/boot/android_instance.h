#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/error.h"
#include "host/block_file.h"

namespace kestrel {

// Everything needed to boot a prebuilt Cuttlefish image directory (docs/DESIGN.md §5.10).
struct AndroidLaunch {
  std::vector<uint8_t> kernel;
  std::vector<uint8_t> initrd;  // ramdisks + bootconfig
  std::string cmdline;
  std::unique_ptr<BlockFile> disk;  // GPT composite of all partitions
};

struct AndroidOptions {
  uint64_t ram_mb;
  // Factory reset: start from empty userdata/metadata/misc.
  bool wipe_data = false;
  // Hardware-accelerated graphics (virtio-gpu + gfxstream) instead of software rendering.
  bool gpu_accel = false;
  bool host_gles = false;  // with gpu_accel: guest GLES executed on the host
  std::map<std::string, std::string> bootconfig_overrides;
};

// Uses <image_dir>/instance/ for per-device writable state (userdata, metadata,
// misc) and the expanded super image; creates them on first use.
Result<AndroidLaunch> prepare_android(const std::string& image_dir, const AndroidOptions& options);

// The boot parameters a Cuttlefish bootloader + launcher would pass
// (device/google/cuttlefish host/commands/assemble_cvd/bootconfig_args.cpp).
std::map<std::string, std::string> kestrel_android_bootconfig(uint64_t ram_mb, bool gpu_accel,
                                                              bool host_gles = false);

}  // namespace kestrel
