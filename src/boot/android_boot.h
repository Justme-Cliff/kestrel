#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/error.h"

namespace kestrel {

// Kernel, ramdisks and boot parameters extracted from Android boot images
// (boot.img v3/v4, init_boot.img, vendor_boot.img v3/v4). This is what a real
// bootloader assembles before starting the kernel.
struct AndroidBoot {
  std::vector<uint8_t> kernel;
  std::vector<uint8_t> ramdisk;  // vendor ramdisk followed by the generic ramdisk
  size_t vendor_ramdisk_size = 0;  // the vendor part's length at the start of `ramdisk`
  std::string vendor_cmdline;
  std::map<std::string, std::string> vendor_bootconfig;
};

Result<AndroidBoot> load_android_boot(const std::string& image_dir);

// Parses "key=value" lines (vendor_boot's bootconfig section).
std::map<std::string, std::string> parse_bootconfig(std::string_view text);

// Appends a bootconfig block to an initrd (Documentation/admin-guide/bootconfig.rst):
// data, le32 size, le32 checksum, "#BOOTCONFIG\n". The kernel needs `bootconfig`
// on its command line to read it.
void append_bootconfig(std::vector<uint8_t>& initrd, const std::map<std::string, std::string>& params);

}  // namespace kestrel
