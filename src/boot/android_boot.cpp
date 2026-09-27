#include "boot/android_boot.h"

#include <cstring>
#include <format>

#include "boot/kernel_image.h"
#include "core/gpa.h"

namespace kestrel {
namespace {

uint32_t le32(const std::vector<uint8_t>& d, size_t off) {
  uint32_t v = 0;
  if (off + 4 <= d.size()) std::memcpy(&v, d.data() + off, 4);
  return v;
}

std::string c_string(const std::vector<uint8_t>& d, size_t off, size_t max) {
  if (off >= d.size()) return {};
  const char* p = reinterpret_cast<const char*>(d.data() + off);
  return std::string(p, strnlen(p, std::min(max, d.size() - off)));
}

Result<std::vector<uint8_t>> slice(const std::vector<uint8_t>& d, uint64_t off, uint64_t len,
                                   const char* what) {
  if (off > d.size() || len > d.size() - off) return make_error(std::format("{} out of bounds", what));
  return std::vector<uint8_t>(d.begin() + static_cast<long>(off),
                              d.begin() + static_cast<long>(off + len));
}

// boot.img / init_boot.img, header v3 or v4 (page size fixed at 4096).
struct BootImage {
  std::vector<uint8_t> kernel, ramdisk;
};

Result<BootImage> parse_boot(const std::vector<uint8_t>& d, const std::string& name) {
  if (d.size() < 4096 || std::memcmp(d.data(), "ANDROID!", 8) != 0) {
    return make_error(name + " is not an Android boot image");
  }
  const uint32_t version = le32(d, 40);
  if (version < 3) return make_error(std::format("{}: boot header v{} not supported", name, version));
  constexpr uint64_t kPage = 4096;
  const uint32_t kernel_size = le32(d, 8), ramdisk_size = le32(d, 12);
  BootImage img;
  auto k = slice(d, kPage, kernel_size, "kernel");
  if (!k) return std::unexpected(k.error());
  auto r = slice(d, kPage + align_up(kernel_size, kPage), ramdisk_size, "ramdisk");
  if (!r) return std::unexpected(r.error());
  img.kernel = std::move(*k);
  img.ramdisk = std::move(*r);
  return img;
}

}  // namespace

std::map<std::string, std::string> parse_bootconfig(std::string_view text) {
  std::map<std::string, std::string> out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    auto trim = [](std::string_view s) {
      while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.remove_prefix(1);
      while (!s.empty() && (s.back() == ' ' || s.back() == '"' || s.back() == '\0')) s.remove_suffix(1);
      return std::string(s);
    };
    out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  return out;
}

void append_bootconfig(std::vector<uint8_t>& initrd, const std::map<std::string, std::string>& params) {
  std::string data;
  for (const auto& [k, v] : params) data += std::format("{} = \"{}\"\n", k, v);
  while (data.size() % 4) data.push_back('\0');  // kernel expects 4-byte alignment
  uint32_t checksum = 0;
  for (unsigned char c : data) checksum += c;
  const auto size = static_cast<uint32_t>(data.size());
  initrd.insert(initrd.end(), data.begin(), data.end());
  const auto* sp = reinterpret_cast<const uint8_t*>(&size);
  const auto* cp = reinterpret_cast<const uint8_t*>(&checksum);
  initrd.insert(initrd.end(), sp, sp + 4);
  initrd.insert(initrd.end(), cp, cp + 4);
  constexpr std::string_view kMagic = "#BOOTCONFIG\n";
  initrd.insert(initrd.end(), kMagic.begin(), kMagic.end());
}

Result<AndroidBoot> load_android_boot(const std::string& dir) {
  AndroidBoot out;

  auto boot_file = read_file(dir + "/boot.img");
  if (!boot_file) return std::unexpected(boot_file.error());
  auto boot = parse_boot(*boot_file, "boot.img");
  if (!boot) return std::unexpected(boot.error());
  out.kernel = std::move(boot->kernel);

  // Android 13+ keeps the generic ramdisk in init_boot; older builds in boot.
  std::vector<uint8_t> generic_ramdisk = std::move(boot->ramdisk);
  if (auto init_file = read_file(dir + "/init_boot.img")) {
    auto init_boot = parse_boot(*init_file, "init_boot.img");
    if (!init_boot) return std::unexpected(init_boot.error());
    if (!init_boot->ramdisk.empty()) generic_ramdisk = std::move(init_boot->ramdisk);
  }

  // vendor_boot.img header v3/v4 (system/tools/mkbootimg/include/bootimg/bootimg.h).
  auto vb = read_file(dir + "/vendor_boot.img");
  if (!vb) return std::unexpected(vb.error());
  const auto& d = *vb;
  if (d.size() < 2128 || std::memcmp(d.data(), "VNDRBOOT", 8) != 0) {
    return make_error("vendor_boot.img is not a vendor boot image");
  }
  const uint32_t version = le32(d, 8), page = le32(d, 12);
  if (version < 3 || page == 0 || (page & (page - 1)) != 0) {
    return make_error("unsupported vendor_boot.img header");
  }
  const uint32_t vendor_ramdisk_size = le32(d, 24);
  out.vendor_cmdline = c_string(d, 28, 2048);
  const uint32_t header_size = le32(d, 2096), dtb_size = le32(d, 2100);
  uint64_t off = align_up(header_size, page);
  auto vendor_ramdisk = slice(d, off, vendor_ramdisk_size, "vendor ramdisk");
  if (!vendor_ramdisk) return std::unexpected(vendor_ramdisk.error());
  off += align_up(vendor_ramdisk_size, page) + align_up(dtb_size, page);
  if (version >= 4) {
    const uint32_t table_size = le32(d, 2112), bootconfig_size = le32(d, 2124);
    off += align_up(table_size, page);
    auto bc = slice(d, off, bootconfig_size, "vendor bootconfig");
    if (!bc) return std::unexpected(bc.error());
    out.vendor_bootconfig =
        parse_bootconfig(std::string_view(reinterpret_cast<const char*>(bc->data()), bc->size()));
  }

  // The kernel unpacks concatenated (compressed) cpio archives in order.
  out.ramdisk = std::move(*vendor_ramdisk);
  out.vendor_ramdisk_size = out.ramdisk.size();
  out.ramdisk.insert(out.ramdisk.end(), generic_ramdisk.begin(), generic_ramdisk.end());
  return out;
}

}  // namespace kestrel
