#include "boot/android_instance.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <filesystem>
#include <format>
#include <fstream>

#include "boot/android_boot.h"
#include "boot/android_modules.h"
#include "core/log.h"
#include "storage/android_sparse.h"
#include "storage/composite_disk.h"

namespace kestrel {
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kMiB = 1ull << 20;

Result<void> ensure_blank(const fs::path& path, uint64_t size) {
  if (fs::exists(path)) return {};
  std::error_code ec;
  { std::ofstream(path, std::ios::binary); }  // NOLINT: create
  fs::resize_file(path, size, ec);            // sparse: costs no disk space until written
  if (ec) return make_error(std::format("cannot create {}: {}", path.string(), ec.message()));
  log(LogLevel::kInfo, "created {} ({} MB)", path.filename().string(), size / kMiB);
  return {};
}

}  // namespace

std::map<std::string, std::string> kestrel_android_bootconfig(uint64_t ram_mb, bool gpu_accel,
                                                              bool host_gles) {
  std::map<std::string, std::string> config = {
      {"androidboot.serialno", "KESTREL0001"},
      {"androidboot.boot_devices", "10000.pci"},  // our PCI host (arch/arm64/virt_layout.h)
      {"androidboot.slot_suffix", "_a"},
      {"androidboot.force_normal_boot", "1"},
      {"androidboot.fstab_suffix", "cf.ext4.hctr2"},
      {"androidboot.ddr_size", std::format("{}MB", ram_mb)},
      // Unlocked "orange" state: AVB checks run, but a mismatch doesn't stop boot.
      {"androidboot.verifiedbootstate", "orange"},
      {"androidboot.vbmeta.device_state", "unlocked"},
      {"androidboot.vbmeta.avb_version", "1.1"},
      {"androidboot.vbmeta.hash_alg", "sha256"},
      {"androidboot.vbmeta.invalidate_on_error", "yes"},
      {"androidboot.console", "ttyAMA0"},
      {"androidboot.serialconsole", "1"},
      {"androidboot.lcd_density", "180"},  // tablet layout at 1280x720 (AppSettings::tablet_dpi)
      {"androidboot.setupwizard_mode", "DISABLED"},
      {"androidboot.enable_bootanimation", "0"},
      {"androidboot.hw_timeout_multiplier", "3"},
      {"androidboot.hypervisor.vm.supported", "0"},
      {"androidboot.hypervisor.protected_vm.supported", "0"},
      {"androidboot.wifi_impl", "virt_wifi"},
      {"androidboot.wifi_mac_prefix", "5554"},  // setup_wifi derives wlan0's MAC from it
      // Cuttlefish HALs that read these at startup and abort if they're missing.
      {"androidboot.vsock_lights_port", "6900"},
      {"androidboot.vsock_lights_cid", "2"},
      {"androidboot.openthread_node_id", "1"},
      // The radio HAL exits (and init restarts it every 5 s) without a modem simulator
      // port. Nothing listens there, so the phone has no SIM and stays idle.
      {"androidboot.modem_simulator_ports", "9600"},
      // ANGLE's VK_EXT_swapchain_maintenance1 present fences leak one sync_file per
      // frame on the gfxstream guest Vulkan driver (the fence imported into each
      // present fence is never released): games hit the 32768-fd limit after ~9
      // minutes and abort in BLASTBufferQueue with EMFILE. Cuttlefish's init copies
      // this into debug.angle.feature_overrides_disabled before any app starts.
      {"androidboot.hardware.angle_feature_overrides_disabled", "supportsSwapchainMaintenance1"},
      // Guest-side security HALs: we don't run Cuttlefish's host services.
      {"androidboot.vendor.apex.com.android.hardware.keymint",
       "com.android.hardware.keymint.rust_nonsecure"},
      {"androidboot.vendor.apex.com.android.hardware.gatekeeper",
       "com.android.hardware.gatekeeper.nonsecure"},
      {"androidboot.vendor.apex.com.android.hardware.secure_element",
       "com.android.hardware.secure_element"},
      {"androidboot.vendor.apex.com.android.hardware.strongbox", "none"},
      {"androidboot.vendor.apex.com.android.hardware.weaver", "none"},
      // Two camera HAL APEXes ship; one must be chosen or apexd aborts the boot.
      {"androidboot.vendor.apex.com.google.emulated.camera.provider.hal",
       "com.google.emulated.camera.provider.hal"},
      // Software rendering in the guest until Phase 5 (crosvm_manager.cpp, guest_swiftshader).
      {"androidboot.vendor.apex.com.android.hardware.graphics.composer",
       "com.android.hardware.graphics.composer.ranchu"},
      {"androidboot.cpuvulkan.version", "4206592"},  // VK_API_VERSION_1_3
      {"androidboot.hardware.gralloc", "minigbm"},
      {"androidboot.hardware.hwcomposer", "ranchu"},
      {"androidboot.hardware.hwcomposer.mode", "client"},
      {"androidboot.hardware.hwcomposer.display_finder_mode", "drm"},
      {"androidboot.hardware.hwcomposer.display_framebuffer_format", "rgba"},
      {"androidboot.hardware.egl", "angle"},
      {"androidboot.hardware.vulkan", "pastel"},
      {"androidboot.opengles.version", "196609"},
  };
  if (gpu_accel) {
    // Cuttlefish's "gfxstream_guest_angle" mode (crosvm_manager.cpp): guest GLES on
    // ANGLE over Vulkan, Vulkan streamed to the host through virtio-gpu "ASG" rings.
    config["androidboot.cpuvulkan.version"] = "0";
    config["androidboot.hardware.egl"] = "angle";
    config["androidboot.hardware.vulkan"] = "ranchu";
    config["androidboot.hardware.gltransport"] = "virtio-gpu-asg";
    config["androidboot.opengles.version"] = "196609";  // GLES 3.1
    if (host_gles) {
      // Cuttlefish's "gfxstream" mode: the guest's GLES calls are streamed to the
      // host ("emulation" EGL) instead of running on guest ANGLE.
      config["androidboot.hardware.egl"] = "emulation";
    }
  }
  return config;
}

Result<AndroidLaunch> prepare_android(const std::string& image_dir, const AndroidOptions& options) {
  const fs::path dir(image_dir);
  const fs::path instance = dir / "instance";
  std::error_code ec;
  fs::create_directories(instance, ec);
  if (ec) return make_error("cannot create " + instance.string());

  // One VM per instance: two VMs writing the same userdata would corrupt it.
  // The lock is held until the process exits.
  const int lock_fd = ::open((instance / ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock_fd < 0 || ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    if (lock_fd >= 0) ::close(lock_fd);
    return make_error("this Android is already running in another Kestrel window");
  }

  auto boot = load_android_boot(image_dir);
  if (!boot) return std::unexpected(boot.error());

  if (options.wipe_data) {
    for (const char* name : {"userdata.img", "metadata.img", "misc.img"}) fs::remove(instance / name, ec);
  }

  // Expand super once; the raw copy is sparse on disk.
  const fs::path super_raw = instance / "super.raw";
  if (!fs::exists(super_raw)) {
    log(LogLevel::kInfo, "expanding super.img (first run only)...");
    if (auto r = unsparse_image((dir / "super.img").string(), super_raw.string()); !r) return std::unexpected(r.error());
  }
  for (auto [name, size] : {std::pair{"misc.img", 1 * kMiB}, std::pair{"frp.img", 1 * kMiB},
                            std::pair{"metadata.img", 64 * kMiB},
                            std::pair{"userdata.img", 8192 * kMiB}}) {
    if (auto r = ensure_blank(instance / name, size); !r) return std::unexpected(r.error());
  }

  // Partition table like Cuttlefish's os_composite (slot A only).
  struct Spec {
    const char* name;
    fs::path path;
    bool writable;
  };
  const std::vector<Spec> specs = {
      {"misc", instance / "misc.img", true},
      {"frp", instance / "frp.img", true},  // PersistentDataBlockService (factory reset protection)
      {"boot_a", dir / "boot.img", false},
      {"init_boot_a", dir / "init_boot.img", false},
      {"vendor_boot_a", dir / "vendor_boot.img", false},
      {"vbmeta_a", dir / "vbmeta.img", false},
      {"vbmeta_system_a", dir / "vbmeta_system.img", false},
      {"vbmeta_vendor_dlkm_a", dir / "vbmeta_vendor_dlkm.img", false},
      {"vbmeta_system_dlkm_a", dir / "vbmeta_system_dlkm.img", false},
      {"super", super_raw, false},
      {"userdata", instance / "userdata.img", true},
      {"metadata", instance / "metadata.img", true},
  };
  std::vector<CompositeDisk::Partition> parts;
  for (const auto& s : specs) {
    if (!fs::exists(s.path)) {
      if (!s.writable && std::string_view(s.name).starts_with("vbmeta_")) continue;  // optional
      return make_error("missing " + s.path.string());
    }
    auto file = BlockFile::open(s.path.string(), !s.writable);
    if (!file) return std::unexpected(file.error());
    parts.push_back({s.name, std::move(*file)});
  }
  auto disk = CompositeDisk::create(std::move(parts));
  if (!disk) return std::unexpected(disk.error());

  auto bootconfig = boot->vendor_bootconfig;
  for (auto& [k, v] : kestrel_android_bootconfig(options.ram_mb, options.gpu_accel, options.host_gles)) bootconfig[k] = v;
  for (auto& [k, v] : options.bootconfig_overrides) bootconfig[k] = v;

  AndroidLaunch launch;
  launch.kernel = std::move(boot->kernel);
  launch.initrd = std::move(boot->ramdisk);
  // virtio_balloon: without it, RAM Android frees never goes back to macOS. Best
  // effort: Android boots the same without it.
  if (auto super = BlockFile::open(super_raw.string(), true)) {
    const SuperReader read = [&](uint64_t off, std::span<uint8_t> out) { return (*super)->read_at(off, out); };
    const auto vendor = std::span<const uint8_t>(launch.initrd).first(std::min(boot->vendor_ramdisk_size, launch.initrd.size()));
    if (auto extra = build_extra_modules_cpio(read, vendor, {"virtio_balloon.ko"}); extra) {
      launch.initrd.insert(launch.initrd.end(), extra->begin(), extra->end());
      if (!extra->empty()) log(LogLevel::kInfo, "android: loading virtio_balloon (returns freed RAM to macOS)");
    } else {
      log(LogLevel::kWarn, "android: no virtio_balloon ({})", extra.error().message);
    }
  }
  append_bootconfig(launch.initrd, bootconfig);
  launch.cmdline = boot->vendor_cmdline + " console=ttyAMA0 earlycon=pl011,0x09000000";
  launch.disk = std::move(*disk);
  return launch;
}

}  // namespace kestrel
