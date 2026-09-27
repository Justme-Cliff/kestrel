// Kestrel command-line entry point (headless: serial console in the terminal,
// screen optionally written to an image file).

#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <condition_variable>
#include <cmath>
#include <csignal>
#include <cstring>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "android/adb_client.h"
#include "app/android_assistant.h"
#include "app/android_images.h"
#include "app/game_controls.h"
#include "app/boot_progress.h"
#include "app/problem_report.h"
#include "app/settings.h"
#include "android/cuttlefish_sensors.h"
#include "arch/arm64/virt_layout.h"
#include "boot/android_instance.h"
#include "boot/kernel_image.h"
#include "boot/virt_dtb.h"
#include "core/log.h"
#include "core/machine.h"
#include "devices/pci/pci.h"
#include "devices/platform/pl011.h"
#include "devices/platform/pl031.h"
#include "devices/virtio/virtio_blk.h"
#include "devices/virtio/virtio_console.h"
#include "devices/virtio/virtio_gpu.h"
#include "devices/virtio/virtio_gpu_gfxstream.h"
#include "devices/virtio/virtio_input.h"
#include "devices/virtio/virtio_mmio.h"
#include "devices/virtio/virtio_net.h"
#include "devices/virtio/virtio_pci.h"
#include "devices/virtio/virtio_balloon.h"
#include "devices/virtio/virtio_rng.h"
#include "devices/virtio/virtio_snd.h"
#include "host/audio.h"
#include "devices/virtio/virtio_vsock.h"
#include "guest_programs/hello_program.h"
#include "keymap/touch.h"
#include "host/block_file.h"
#include "host/control_server.h"
#include "host/frame_file.h"
#include "host/macos/mac_window.h"
#include "host/macos/raw_mouse.h"
#include "app/app_watch.h"
#include "app/crash_marker.h"
#include "app/instances.h"
#include "host/macos/recorder.h"
#include "host/terminal.h"
#include "net/user_net.h"
#include "net/vsock_host.h"

extern char** environ;

using namespace kestrel;
namespace L = kestrel::arm64::layout;

namespace {

constexpr uint64_t k2MiB = 2ull << 20;
constexpr const char* kLinuxCmdline = "console=ttyAMA0 earlycon=pl011,0x09000000";
constexpr unsigned kAndroidConsoles = 20;  // Cuttlefish's /dev/hvc0 .. /dev/hvc19
constexpr unsigned kLogcatConsole = 2;     // hvc2 carries logcat
constexpr unsigned kSensorsControlConsole = 18, kSensorsDataConsole = 19;
constexpr uint16_t kAdbHostPort = 6520;

struct Options {
  bool hello = false;
  std::optional<std::string> kernel, initrd, android, screenshot, control;
  std::optional<std::string> cmdline;
  std::optional<uint64_t> ram_mb;
  std::optional<unsigned> cpus;
  std::vector<std::string> disks;
  std::vector<std::pair<std::string, std::string>> bootconfig;
  uint32_t display_w = 1280, display_h = 720;
  bool network = true;
  bool pci = false;
  bool headless = false;
  bool low_latency = false;  // show the newest frame without waiting for vsync
  bool wipe_data = false;
  bool show_sidebar = true;
  std::optional<uint32_t> dpi;
  uint32_t refresh_hz = 60;
  std::optional<bool> gpu_accel;  // --gpu host|software; default: host for Android
  bool verbose = false;
};

void usage() {
  std::fprintf(stderr,
               "usage: kestrel --hello\n"
               "       kestrel --android <cuttlefish image dir> [--bootconfig key=value]... [--wipe-data]\n"
               "       kestrel --kernel <Image> [--initrd <file>] [--cmdline <str>] [--pci]\n"
               "common: [--ram <MB>] [--cpus <N>] [--disk [ro:]<file>]... [--no-net]\n"
               "        [--display <W>x<H>] [--screenshot <file.ppm>] [--headless] [--verbose]\n"
               "        [--gpu host|software] [--dpi N] [--refresh HZ] [--no-toolbar] [--control <socket>]  (commands: tap X Y | swipe X1 Y1 X2 Y2 [ms] | key CODE)\n"
               "Disks appear as /dev/vda, /dev/vdb, ...\n"
               "Networking: user-mode NAT, guest gets 10.0.2.15 by DHCP; 10.0.2.2 is the host.\n"
               "Press Ctrl-] to quit.\n");
}

std::optional<Options> parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::optional<std::string> {
      if (i + 1 >= argc) return std::nullopt;
      return std::string(argv[++i]);
    };
    if (a == "--hello") { o.hello = true; continue; } 
    if (a == "--headless") { o.headless = true; continue; }
    if (a == "--low-latency") { o.low_latency = true; continue; }
    if (a == "--verbose") { o.verbose = true; continue; }
    if (a == "--no-net") { o.network = false; continue; }
    if (a == "--pci") { o.pci = true; continue; }
    if (a == "--wipe-data") { o.wipe_data = true; continue; }
    if (a == "--no-toolbar") { o.show_sidebar = false; continue; }
    if (a == "--keep-data") continue;  // the default; kept for old scripts
    auto v = value();
    if (!v) return std::nullopt;
    if (a == "--kernel") o.kernel = v;
    else if (a == "--initrd") o.initrd = v;
    else if (a == "--android") o.android = v;
    else if (a == "--cmdline") o.cmdline = v;
    else if (a == "--screenshot") o.screenshot = v;
    else if (a == "--control") o.control = v;
    else if (a == "--dpi") o.dpi = static_cast<uint32_t>(std::strtoul(v->c_str(), nullptr, 10));
    else if (a == "--refresh") o.refresh_hz = std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(v->c_str(), nullptr, 10)), 20, 240);
    else if (a == "--gpu") {
      if (*v != "host" && *v != "software") return std::nullopt;
      o.gpu_accel = *v == "host";
    }
    else if (a == "--ram") o.ram_mb = std::strtoull(v->c_str(), nullptr, 10);
    else if (a == "--cpus") o.cpus = static_cast<unsigned>(std::strtoul(v->c_str(), nullptr, 10));
    else if (a == "--disk") o.disks.push_back(*v);
    else if (a == "--display") {
      if (std::sscanf(v->c_str(), "%ux%u", &o.display_w, &o.display_h) != 2) return std::nullopt;
    } else if (a == "--bootconfig") {
      const size_t eq = v->find('=');
      if (eq == std::string::npos) return std::nullopt;
      o.bootconfig.emplace_back(v->substr(0, eq), v->substr(eq + 1));
    } else {
      return std::nullopt;
    }
  }
  if (int(o.hello) + int(o.kernel.has_value()) + int(o.android.has_value()) != 1) return std::nullopt;
  if (o.android) o.pci = true;  // Android's kernel only has virtio-pci
  if ((o.ram_mb && *o.ram_mb < 64) || o.display_w < 64 || o.display_h < 64) return std::nullopt;
  return o;
}

struct VirtioLayout {
  unsigned mmio_slots = 0;
  std::vector<unsigned> pci_slots;
};

// Maps devices on PCI (one slot each) or virtio-mmio, in the given order.
Result<VirtioLayout> attach_virtio(Machine& m, bool use_pci,
                                   std::vector<std::unique_ptr<VirtioDevice>> devices) {
  VirtioLayout layout;
  if (use_pci) {
    auto* pci = m.own(std::make_unique<PciHost>(
        L::kPciMmioBase, L::kPciMmio64Base,
        [&m](unsigned slot, bool level) { m.set_spi(L::kPciFirstSpi + slot, level); }));
    for (auto& dev : devices) {
      if (pci->device_count() + 1 >= PciHost::kMaxDevices) return make_error("too many PCI devices");
      layout.pci_slots.push_back(pci->add(std::make_unique<VirtioPci>(m.memory(), std::move(dev))));
    }
    auto r = m.attach_borrowed(Gpa(L::kPciEcamBase), L::kPciEcamSize, pci->ecam())
                 .and_then([&] { return m.attach_borrowed(Gpa(L::kPciMmioBase), L::kPciMmioSize, pci->window()); })
                 .and_then([&] { return m.attach_borrowed(Gpa(L::kPciMmio64Base), L::kPciMmio64Size, pci->window64()); });
    if (!r) return std::unexpected(r.error());
    return layout;
  }
  for (auto& dev : devices) {
    const unsigned slot = layout.mmio_slots++;
    if (slot >= L::kVirtioMmioSlots) return make_error("too many virtio devices");
    const uint32_t spi = L::kVirtioFirstSpi + slot;
    auto transport = std::make_unique<VirtioMmio>(m.memory(), std::move(dev),
                                                  [&m, spi](bool level) { m.set_spi(spi, level); });
    auto r = m.attach(Gpa(L::kVirtioMmioBase + slot * L::kVirtioMmioSize), L::kVirtioMmioSize,
                      std::move(transport));
    if (!r) return std::unexpected(r.error());
  }
  return layout;
}

// virtio-net wired to our user-mode NAT. UserNet only sends in reply to guest
// traffic, so `device` is set before use; the machine destroys owned objects
// (the NAT) before devices.
std::unique_ptr<VirtioDevice> make_network(Machine& m, uint8_t mac_last, std::optional<uint16_t> adb_port) {
  struct NetBridge {
    VirtioNet* device = nullptr;
    std::unique_ptr<net::UserNet> stack;
  };
  auto* bridge = m.own(std::make_unique<NetBridge>());
  bridge->stack = std::make_unique<net::UserNet>(
      [bridge](std::vector<uint8_t> frame) { bridge->device->deliver(std::move(frame)); });
  if (adb_port) {
    // adb: `adb connect 127.0.0.1:6520` reaches adbd's tcp:5555 (same port as Cuttlefish).
    if (bridge->stack->add_port_forward(*adb_port, 5555)) {
      log(LogLevel::kInfo, "adb: adb connect 127.0.0.1:{}", *adb_port);
    } else {
      log(LogLevel::kWarn, "adb port {} is busy; adb forwarding disabled", *adb_port);
    }
  }
  auto dev = std::make_unique<VirtioNet>(
      net::Mac{0x52, 0x54, 0x00, 0x12, 0x34, mac_last},
      [bridge](std::span<const uint8_t> frame) { bridge->stack->from_guest(frame); });
  bridge->device = dev.get();
  return dev;
}

// virtio-vsock with its host bridge. adb reaches adbd's vsock:5555 through
// 127.0.0.1:6520, like Cuttlefish; the lights HAL connects to host port 6900.
std::unique_ptr<VirtioDevice> make_vsock(Machine& m, uint16_t& adb_port) {
  constexpr uint64_t kGuestCid = 3;
  struct VsockBridge {
    VirtioVsock* device = nullptr;
    std::unique_ptr<net::VsockHost> host;
  };
  auto* bridge = m.own(std::make_unique<VsockBridge>());
  bridge->host = std::make_unique<net::VsockHost>(
      kGuestCid, [bridge](VsockPacket p) { bridge->device->deliver(std::move(p)); });
  bridge->host->accept_guest_connections(6900);
  // 6520 like Cuttlefish; if another emulator holds it, take the next free port.
  adb_port = 0;
  for (uint16_t port = kAdbHostPort; port < kAdbHostPort + 20 && !adb_port; ++port) {
    if (bridge->host->add_forward(port, 5555)) adb_port = port;
  }
  if (adb_port) {
    log(LogLevel::kInfo, "adb: adb connect 127.0.0.1:{}", adb_port);
  } else {
    log(LogLevel::kWarn, "no free adb port; adb forwarding disabled");
  }
  auto dev = std::make_unique<VirtioVsock>(
      kGuestCid, [bridge](VsockPacket p) { bridge->host->from_guest(std::move(p)); });
  bridge->device = dev.get();
  return dev;
}

// Loads kernel, initrd and device tree into guest RAM. Returns (entry PC, DTB address).
Result<std::pair<uint64_t, uint64_t>> load_linux(Machine& m, std::vector<uint8_t> kernel_file,
                                                 std::optional<std::vector<uint8_t>> initrd,
                                                 const std::string& cmdline,
                                                 const VirtioLayout& virtio) {
  auto image = unpack_kernel(std::move(kernel_file));
  if (!image) return std::unexpected(image.error());
  auto info = parse_arm64_image(*image);
  if (!info) return std::unexpected(info.error());

  const GuestMemory& mem = m.memory();
  const uint64_t ram_base = mem.base().value;
  const uint64_t kernel_addr = ram_base + info->text_offset;
  if (!mem.write(Gpa(kernel_addr), *image)) return make_error("kernel doesn't fit in guest RAM");
  const uint64_t next_free = align_up(kernel_addr + info->image_size, k2MiB);
  // The DTB goes in the last 2 MB of RAM; everything else must stay below it.
  const uint64_t dtb_addr = ram_base + mem.size() - k2MiB;

  VirtBoardConfig board{
      .ram_base = ram_base,
      .ram_size = mem.size(),
      .cpus = m.config().cpus,
      .gic_dist_base = m.gic().dist_base,
      .gic_dist_size = m.gic().dist_size,
      .gic_redist_base = m.gic().redist_base,
      .gic_redist_size = m.gic().redist_size,
      .cmdline = cmdline,
      .initrd = std::nullopt,
      .virtio_slots = virtio.mmio_slots,
      .pci_slots = virtio.pci_slots,
  };
  if (initrd) {
    if (next_free + initrd->size() > dtb_addr || !mem.write(Gpa(next_free), *initrd)) {
      return make_error("initrd doesn't fit in guest RAM; try a larger --ram");
    }
    board.initrd = {{next_free, next_free + initrd->size()}};
  }
  auto dtb = build_virt_dtb(board);
  if (!dtb) return std::unexpected(dtb.error());
  if (dtb->size() > k2MiB || !mem.write(Gpa(dtb_addr), *dtb)) return make_error("DTB too large");

  log(LogLevel::kInfo, "kernel {} KB, {} vCPU(s), {} MB RAM, {} virtio device(s) on {}",
      image->size() / 1024, m.config().cpus, mem.size() >> 20,
      virtio.mmio_slots + virtio.pci_slots.size(), virtio.pci_slots.empty() ? "MMIO" : "PCI");
  return std::pair{kernel_addr, dtb_addr};
}

template <typename T>
T or_exit(Result<T> r) {
  if (!r) {
    log(LogLevel::kError, "{}", r.error().message);
    std::exit(1);
  }
  return std::move(*r);
}
void or_exit(Result<void> r) {
  if (!r) {
    log(LogLevel::kError, "{}", r.error().message);
    std::exit(1);
  }
}

// Input-to-frame latency for tests (`latency x y` on the control socket): keeps the
// last frame, touches the screen and times how long until the area under the finger
// changes in a frame Android sends out. With Android's "pointer location" overlay on,
// that's input -> app -> SurfaceFlinger -> GPU -> scanout. Headless only (frames are
// read back); a window adds its display's presentation on top.
class LatencyProbe final : public DisplaySink {
 public:
  void present(const Frame& f) override {
    std::lock_guard lock(mutex_);
    last_.assign(f.pixels.begin(), f.pixels.end());
    width_ = f.width;
    height_ = f.height;
    stride_ = f.stride;
    ++frames_;
    cv_.notify_all();
  }
  // Hash of the 21x21 box around (x, y) in the last frame; 0 if no frame yet.
  uint64_t box(int x, int y) {
    std::lock_guard lock(mutex_);
    return box_locked(x, y);
  }
  // Waits until the box differs from `before`; milliseconds since t0, or -1.
  double wait_change(int x, int y, uint64_t before, std::chrono::steady_clock::time_point t0) {
    std::unique_lock lock(mutex_);
    const bool changed = cv_.wait_until(lock, t0 + std::chrono::seconds(1), [&] { return box_locked(x, y) != before; });
    if (!changed) return -1;
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }

 private:
  uint64_t box_locked(int x, int y) const {
    if (last_.empty()) return 0;
    uint64_t h = 1469598103934665603ull;
    for (int yy = std::max(0, y - 10); yy <= std::min<int>(int(height_) - 1, y + 10); ++yy) {
      for (int xx = std::max(0, x - 10); xx <= std::min<int>(int(width_) - 1, x + 10); ++xx) {
        const size_t o = size_t(yy) * stride_ + size_t(xx) * 4;
        uint32_t px;
        std::memcpy(&px, &last_[o], 4);
        h = (h ^ px) * 1099511628211ull;
      }
    }
    return h;
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<uint8_t> last_;
  uint32_t width_ = 0, height_ = 0, stride_ = 0;
  uint64_t frames_ = 0;
};
LatencyProbe* g_latency_probe = nullptr;
VirtioGpuGfxstream* g_test_gpu = nullptr;  // for the gpu_stall test command

// Speakers, plus a copy of the sound for screen recordings while one runs.
class TappedAudioOutput final : public AudioOutput {
 public:
  explicit TappedAudioOutput(std::unique_ptr<AudioOutput> inner) : inner_(std::move(inner)) {}
  using Tap = std::function<void(std::span<const uint8_t>, uint32_t rate, uint32_t channels)>;
  void set_tap(Tap tap) {
    std::lock_guard lock(mutex_);
    tap_ = std::move(tap);
  }
  bool start(uint32_t rate, uint32_t channels) override {
    rate_ = rate;
    channels_ = channels;
    return inner_->start(rate, channels);
  }
  void stop() override { inner_->stop(); }
  size_t write(std::span<const uint8_t> pcm) override {
    const size_t n = inner_->write(pcm);
    std::lock_guard lock(mutex_);
    if (tap_ && n) tap_(pcm.first(n), rate_, channels_);
    return n;
  }
  uint64_t played_bytes() const override { return inner_->played_bytes(); }
  size_t buffered_bytes() const override { return inner_->buffered_bytes(); }

 private:
  std::unique_ptr<AudioOutput> inner_;
  std::mutex mutex_;
  Tap tap_;
  uint32_t rate_ = 48000, channels_ = 2;
};

// Sends frames to several sinks (window + screenshot file).
class TeeSink final : public DisplaySink {
 public:
  explicit TeeSink(std::vector<DisplaySink*> sinks) : sinks_(std::move(sinks)) {}
  void present(const Frame& f) override {
    for (auto* s : sinks_) s->present(f);
  }

 private:
  std::vector<DisplaySink*> sinks_;
};

// Scripted input for tests and automation (--control).
std::string run_control_command(const std::string& line, TouchScreen& touch, VirtioInput& touch_dev,
                                VirtioInput& keyboard, GameControls* game) {
  constexpr unsigned kSlot = Keymapper::kScriptSlot;
  char cmd[16] = {};
  int a = 0, b = 0, c = 0, d = 0, e = 200;
  const int n = std::sscanf(line.c_str(), "%15s %d %d %d %d %d", cmd, &a, &b, &c, &d, &e);
  const std::string command = cmd;
  // Physical-style input, routed like the window's (through game controls if any).
  if ((command == "keydown" || command == "keyup") && n >= 2 && a > 0 && a < 512) {
    if (game) {
      game->key(uint16_t(a), command == "keydown");
    } else {
      const InputEvent ev[] = {{input::kEvKey, uint16_t(a), command == "keydown"}, {input::kEvSyn, input::kSynReport, 0}};
      keyboard.send(ev);
    }
    return {};
  }
  if (command == "mouse" && n >= 3) {
    if (game) game->mouse_delta(a, b);
    return {};
  }
  if (command == "click" && n >= 3) {  // click <0 left|1 right> <1 down|0 up>
    if (game) game->mouse_button(a == 1 ? kMouseRight : kMouseLeft, b != 0);
    return {};
  }
  if (command == "foreground") {  // pretend this package is in front (tests)
    char pkg[256] = {};
    if (game && std::sscanf(line.c_str(), "%*s %255s", pkg) == 1) game->foreground_changed(pkg);
    return {};
  }
  if (command == "latency" && n >= 3) {  // latency <x> <y>: touch there, time the frame
    if (!g_latency_probe) return "latency needs --headless";
    const uint64_t before = g_latency_probe->box(a, b);
    const auto t0 = std::chrono::steady_clock::now();
    touch_dev.send(touch.down(kSlot, a, b));
    const double ms = g_latency_probe->wait_change(a, b, before, t0);
    touch_dev.send(touch.up(kSlot));
    return std::format("ok latency {:.1f}", ms);
  }
  if (command == "gpu_stall" && n >= 2) {  // gpu_stall <seconds>: tests the GPU watchdog
    if (!g_test_gpu) return "no 3D GPU";
    g_test_gpu->debug_stall(std::clamp(a, 1, 60));
    return {};
  }
  if (command == "tap" && n >= 3) {
    touch_dev.send(touch.down(kSlot, a, b));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    touch_dev.send(touch.up(kSlot));
    return {};
  }
  if (command == "swipe" && n >= 5) {
    const int steps = std::max(2, e / 16);  // ~60 events per second
    touch_dev.send(touch.down(kSlot, a, b));
    for (int i = 1; i <= steps; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(e / steps));
      touch_dev.send(touch.move(kSlot, a + (c - a) * i / steps, b + (d - b) * i / steps));
    }
    touch_dev.send(touch.up(kSlot));
    return {};
  }
  if (command == "key" && n >= 2 && a > 0 && a < 256) {
    const InputEvent down[] = {{input::kEvKey, uint16_t(a), 1}, {input::kEvSyn, input::kSynReport, 0}};
    const InputEvent up[] = {{input::kEvKey, uint16_t(a), 0}, {input::kEvSyn, input::kSynReport, 0}};
    keyboard.send(down);
    keyboard.send(up);
    return {};
  }
  return "unknown command (tap X Y | swipe X1 Y1 X2 Y2 [ms] | key CODE | keydown/keyup CODE | mouse DX DY | click BTN DOWN | foreground PKG)";
}

Machine* g_machine = nullptr;

}  // namespace

// `kestrel adb shell <command>` / `kestrel adb install <apk>`: talks to a running
// Android through the built-in adb client (no Android SDK needed).
int adb_main(int argc, char** argv) {
  // A second Kestrel (tests) takes the next free port: KESTREL_ADB_PORT picks it.
  uint16_t port = kAdbHostPort;
  if (const char* p = std::getenv("KESTREL_ADB_PORT")) port = static_cast<uint16_t>(std::strtoul(p, nullptr, 10));
  AdbClient adb(port);
  adb.set_timeout(0);  // interactive: long silent commands are fine
  const std::string verb = argc > 2 ? argv[2] : "";
  if (verb == "shell" && argc > 3) {
    std::string cmd;
    for (int i = 3; i < argc; ++i) cmd += (i > 3 ? " " : "") + std::string(argv[i]);
    auto out = adb.shell(cmd);
    if (!out) {
      std::fprintf(stderr, "%s\n", out.error().message.c_str());
      return 1;
    }
    std::fwrite(out->data(), 1, out->size(), stdout);
    return 0;
  }
  if (verb == "install" && argc == 4) {
    auto r = adb.install(argv[3]);
    std::printf("%s\n", r ? "Success" : r.error().message.c_str());
    return r ? 0 : 1;
  }
  std::fprintf(stderr, "usage: kestrel adb shell <command> | kestrel adb install <apk>\n");
  return 2;
}

// Launched from Finder / the Dock (no arguments): the BlueStacks-style app.
// Logs go to ~/Library/Logs/Kestrel; Android lives in Application Support and is
// downloaded on first launch. Returns the arguments to run with.
std::optional<std::vector<std::string>> prepare_app_mode() {
  std::error_code ec;
  std::filesystem::create_directories(app_logs_dir(), ec);
  // Keep the previous session's logs: a crash is often reported after a restart.
  for (const char* name : {"kestrel.log", "console.log"}) {
    std::error_code ec;
    const std::string path = app_logs_dir() + "/" + name;
    std::filesystem::rename(path, path + ".prev", ec);
  }
  std::freopen((app_logs_dir() + "/kestrel.log").c_str(), "w", stderr);
  std::freopen((app_logs_dir() + "/console.log").c_str(), "w", stdout);
  std::setvbuf(stderr, nullptr, _IONBF, 0);   // freopen may make it buffered
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  install_crash_marker(app_logs_dir() + "/last-crash.txt");
  const std::string dir = instance_dir(app_support_dir(), current_instance());
  if (!android_images_present(dir)) {
    auto r = run_setup_window("Kestrel", [&](const auto& report) {
      return install_android_images(dir, report);
    });
    if (!r) {
      log(LogLevel::kError, "{}", r.error().message);
      show_fatal_error("Kestrel couldn't set up Android",
                       r.error().message + "\n\nCheck your internet connection and open Kestrel again.");
      return std::nullopt;
    }
  }
  const AppSettings s = AppSettings::load(app_support_dir() + "/settings.json");
  std::vector<std::string> args = {"kestrel", "--android", dir,
                                   "--ram", std::to_string(s.ram_mb),
                                   "--cpus", std::to_string(s.cpus),
                                   "--display", std::format("{}x{}", s.width, s.height),
                                   "--dpi", std::to_string(s.effective_dpi()),
                                   "--gpu", s.gpu ? "host" : "software",
                                   "--refresh", std::to_string(s.refresh_hz)};
  if (!s.show_sidebar) args.push_back("--no-toolbar");
  if (s.low_latency_display) args.push_back("--low-latency");
  return args;
}

// `kestrel --preview-editor`: the game-controls editor over a blank screen, no VM
// (for checking the UI).
int preview_editor_main() {
  auto window = MacWindow::create("Kestrel (editor preview)", 1280, 720, WindowInput{});
  auto profile = parse_profile(default_free_fire_profile());
  window->edit_controls(profile->name, profile_markers(*profile), {"Default"}, 0, [](ControlsEditorResult result) {
    if (result.action == ControlsEditorResult::Action::kSave) {
      for (auto& m : result.markers) std::printf("%s key=%u %.3f %.3f\n", m.name.c_str(), m.key, m.x, m.y);
    }
    quit_app_loop();
  });
  run_app_loop();
  return 0;
}

int main(int argc, char** argv) {
  // Writing to a closed connection (network, adb, logs to a closed pipe) must be
  // an error we handle, never a signal that kills the emulator mid-game.
  std::signal(SIGPIPE, SIG_IGN);
  if (argc > 1 && std::string(argv[1]) == "adb") return adb_main(argc, argv);
  if (argc > 1 && std::string(argv[1]) == "--preview-editor") return preview_editor_main();
  // Dev check of the recorder alone: 3 s of moving colour and a 440 Hz tone.
  if (argc > 2 && std::string(argv[1]) == "--test-recorder") {
    auto rec = ScreenRecorder::start(argv[2], 640, 360);
    if (!rec) return std::fprintf(stderr, "%s\n", rec.error().message.c_str()), 1;
    std::vector<uint8_t> px(640 * 360 * 4);
    double phase = 0;
    for (int f = 0; f < 90; ++f) {
      for (size_t i = 0; i < px.size(); i += 4) px[i] = px[i + 1] = static_cast<uint8_t>(f * 2), px[i + 2] = 200, px[i + 3] = 255;
      (*rec)->video(Frame{640, 360, 640 * 4, true, px});
      std::vector<int16_t> pcm(800 * 2);  // 1/60 s of stereo at 48 kHz
      for (size_t i = 0; i < pcm.size(); i += 2) {
        pcm[i] = pcm[i + 1] = static_cast<int16_t>(12000 * std::sin(phase));
        phase += 2 * M_PI * 440 / 48000;
      }
      (*rec)->audio(std::span(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * 2), 48000, 2);
      std::this_thread::sleep_for(std::chrono::microseconds(16667));
    }
    std::promise<bool> done;
    (*rec)->finish([&](bool ok) { done.set_value(ok); });
    return done.get_future().get() ? 0 : 1;
  }
  const bool app_mode = argc == 1 || (argc == 2 && std::string(argv[1]).starts_with("-psn"));
  std::vector<std::string> app_args;
  std::vector<char*> app_argv;
  if (app_mode) {
    auto args = prepare_app_mode();
    if (!args) return 1;
    app_args = std::move(*args);
    for (auto& a : app_args) app_argv.push_back(a.data());
  }
  auto parsed = app_mode ? parse(static_cast<int>(app_argv.size()), app_argv.data()) : parse(argc, argv);
  if (!parsed) {
    usage();
    return 2;
  }
  const Options& opts = *parsed;
  if (opts.verbose) set_log_level(LogLevel::kDebug);

  // Android defaults: 3 GB fits an 8 GB Mac (docs/DESIGN.md §7.1).
  const uint64_t ram_mb = opts.ram_mb.value_or(opts.android ? 3072 : 1024);
  const unsigned cpus = opts.cpus.value_or(opts.android ? 4 : 1);

  // GPU acceleration is decided before boot: Android's graphics settings depend on it.
  bool gpu_accel = opts.android && opts.gpu_accel.value_or(true);
  // Use the bundled (patched) MoltenVK: without this the Vulkan loader also finds
  // Homebrew's copy and may pick it.
  {
    char self[4096];
    uint32_t size = sizeof(self);
    if (gpu_accel && _NSGetExecutablePath(self, &size) == 0) {
      const auto icd = std::filesystem::path(self).parent_path() / "lib64" / "vulkan" / "MoltenVK_icd.json";
      if (std::filesystem::exists(icd)) {
        setenv("VK_DRIVER_FILES", icd.c_str(), 0);
        setenv("VK_ICD_FILENAMES", icd.c_str(), 0);
      }
    }
    if (opts.low_latency) setenv("KESTREL_LOW_LATENCY_PRESENT", "1", 1);
  }
  // Host GLES (ANGLE on Metal) is opt-in (KESTREL_HOST_GLES=1) until its GPU stalls
  // are fixed; the default is the Vulkan path.
  bool host_gles = false;
  if (const char* e = std::getenv("KESTREL_HOST_GLES"); e && std::string_view(e) == "1") {
    char self[4096];
    uint32_t size = sizeof(self);
    if (gpu_accel && _NSGetExecutablePath(self, &size) == 0) {
      const auto dir = std::filesystem::path(self).parent_path() / "lib64" / "gles_angle";
      host_gles = std::filesystem::exists(dir / "libEGL.dylib") && std::filesystem::exists(dir / "libGLESv2.dylib");
    }
  }

  std::optional<AndroidLaunch> android;
  if (opts.android) {
    AndroidOptions aopts{.ram_mb = ram_mb, .wipe_data = opts.wipe_data,
                         .gpu_accel = gpu_accel, .host_gles = host_gles, .bootconfig_overrides = {}};
    if (opts.dpi) aopts.bootconfig_overrides["androidboot.lcd_density"] = std::to_string(*opts.dpi);
    for (auto& [k, v] : opts.bootconfig) aopts.bootconfig_overrides[k] = v;
    android = or_exit(prepare_android(*opts.android, aopts));
  }

  auto machine = or_exit(Machine::create({.ram_size = ram_mb << 20, .cpus = cpus}));
  Machine& m = *machine;
  g_machine = &m;

  // Boot messages on the console also drive the startup screen's progress bar.
  std::unique_ptr<MacWindow> window;  // outlives the VM thread (declared before the machine runs)
  BootProgress boot_progress([&window](double f, const std::string& text) {
    if (window) window->set_boot_progress(f, text);
  });
  auto uart = std::make_unique<Pl011>([&boot_progress](uint8_t b) {
                                        Terminal::write_output(b);
                                        boot_progress.feed(b);
                                      },
                                      [&m](bool level) { m.set_spi(L::kUartSpi, level); });
  Pl011* uart_ptr = uart.get();
  auto rtc = std::make_unique<Pl031>([] { return static_cast<uint32_t>(std::time(nullptr)); },
                                     [&m](bool level) { m.set_spi(L::kRtcSpi, level); });
  or_exit(m.attach(Gpa(L::kUartBase), L::kUartSize, std::move(uart)));
  or_exit(m.attach(Gpa(L::kRtcBase), L::kRtcSize, std::move(rtc)));

  std::function<void()> open_library;           // shows the Library (set with the window)
  std::function<void()> open_editor;            // the controls editor (reopens after layout changes)
  VirtioGpuGfxstream* screenshot_gpu = nullptr;  // set when frames bypass the window (direct display)
  VirtioGpuGfxstream* record_gpu = nullptr;      // 3D GPU: frames for screen recordings
  TappedAudioOutput* record_audio = nullptr;     // game sound for screen recordings
  std::shared_ptr<ScreenRecorder> recorder;      // while recording (main thread)
  std::unique_ptr<GameControls> game;           // keymapper; uses the window's callbacks
  std::unique_ptr<AndroidAssistant> assistant;  // declared after the window: destroyed first
  std::vector<std::string> pending_apks;        // opened before Android's helper existed
  bool factory_reset = false;
  bool restart = false;  // settings changed: start Android again
  uint16_t adb_port = kAdbHostPort;
  uint64_t pc = 0, x0 = 0;
  if (opts.hello) {
    pc = m.memory().base().value;
    m.memory().write(Gpa(pc), hello_program());
  } else {
    std::vector<std::unique_ptr<VirtioDevice>> devices;
    if (android) {
      // Consoles first and in order: Android expects fixed /dev/hvcN numbers.
      const auto instance = std::filesystem::path(*opts.android) / "instance";
      {
        std::error_code ec;  // the last session's Android log survives one restart
        std::filesystem::rename(instance / "logcat.txt", instance / "logcat.prev.txt", ec);
      }
      auto* logcat = m.own(std::make_unique<std::ofstream>(instance / "logcat.txt", std::ios::trunc));
      std::vector<VirtioConsole*> hvc;
      for (unsigned i = 0; i < kAndroidConsoles; ++i) {
        VirtioConsole::OutputFn out;
        if (i == kLogcatConsole) {
          // Apps that die in front of the player: save what Android logged just before.
          auto* deaths = m.own(std::make_unique<AppDeathWatcher>([&](const AppDeathWatcher::Death& d) {
            const std::string file = app_logs_dir() + std::format("/app-closed-{}.txt", std::time(nullptr));
            std::ofstream(file) << d.package << " " << d.reason << "\n\n" << d.context;
            log(LogLevel::kWarn, "android: {} {} (details in {})", d.package, d.reason, file);
            const std::string name = d.package == "com.dts.freefireth" ? "Free Fire" : d.package;
            if (window) window->show_toast(name + " " + d.reason + " — saved for Help › Report a Problem", 6);
          }));
          out = [logcat, deaths](std::span<const uint8_t> d) {
            logcat->write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
            logcat->flush();
            deaths->feed(d);
          };
        }
        auto console = std::make_unique<VirtioConsole>(std::move(out));
        hvc.push_back(console.get());
        devices.push_back(std::move(console));
      }
      // Owned by the machine, so destroyed before the consoles it writes to.
      auto* sensors = m.own(std::make_unique<CuttlefishSensors>(*hvc[kSensorsControlConsole],
                                                                *hvc[kSensorsDataConsole]));
      hvc[kSensorsControlConsole]->set_output(
          [sensors](std::span<const uint8_t> d) { sensors->on_control_bytes(d); });
      devices.push_back(std::make_unique<VirtioBlk>(std::move(android->disk)));  // /dev/vda
    }
    for (const auto& spec : opts.disks) {
      const bool ro = spec.starts_with("ro:");
      devices.push_back(std::make_unique<VirtioBlk>(or_exit(BlockFile::open(ro ? spec.substr(3) : spec, ro))));
    }
    if (opts.network && android) {
      // Cuttlefish's layout: eth0 = mobile (renamed buried_eth0), eth1 = ethernet,
      // eth2 = wifi (Android's default network via virt_wifi). Each gets its own NAT.
      devices.push_back(make_network(m, 0x56, std::nullopt));
      devices.push_back(make_network(m, 0x57, std::nullopt));
      devices.push_back(make_network(m, 0x58, std::nullopt));
    } else if (opts.network) {
      devices.push_back(make_network(m, 0x56, std::nullopt));
    }
    if (android) devices.push_back(make_vsock(m, adb_port));
    if (android) {
      auto speakers = AudioOutput::create();
      std::unique_ptr<AudioOutput> out;
      if (speakers) {
        auto tapped = std::make_unique<TappedAudioOutput>(std::move(speakers));
        record_audio = tapped.get();
        out = std::move(tapped);
      }
      devices.push_back(std::make_unique<VirtioSnd>(std::move(out), AudioInput::create()));
    }
    devices.push_back(std::make_unique<VirtioRng>(
        [](std::span<uint8_t> out) { arc4random_buf(out.data(), out.size()); }));
    // Free page reporting: RAM Android frees goes back to macOS (docs/DESIGN.md §7.1).
    devices.push_back(std::make_unique<VirtioBalloon>(discard_host_pages));
    if (android || opts.screenshot) {
      std::vector<DisplaySink*> sinks;
      if (opts.screenshot) sinks.push_back(m.own(std::make_unique<FrameFile>(*opts.screenshot)));
      if (opts.control && opts.headless) sinks.push_back(g_latency_probe = m.own(std::make_unique<LatencyProbe>()));
      auto touch_dev = std::make_unique<VirtioInput>(VirtioInput::touchscreen(opts.display_w, opts.display_h));
      auto keyboard_dev = std::make_unique<VirtioInput>(VirtioInput::keyboard());
      auto* touch = m.own(std::make_unique<TouchScreen>(opts.display_w, opts.display_h));
      VirtioInput* touch_ptr = touch_dev.get();
      VirtioInput* keyboard_ptr = keyboard_dev.get();
      if (android) {
        game = std::make_unique<GameControls>(
            *touch, GameControls::Callbacks{
                        .touch = [touch_ptr](const std::vector<InputEvent>& ev) { touch_ptr->send(ev); },
                        .keyboard = [keyboard_ptr](uint16_t key, bool pressed) {
                          const InputEvent ev[] = {{input::kEvKey, key, pressed ? 1 : 0},
                                                   {input::kEvSyn, input::kSynReport, 0}};
                          keyboard_ptr->send(ev);
                        },
                        .capture_mouse = [&](bool on) { if (window) window->set_mouse_captured(on); },
                        .status = [&](const std::string& st) {
                          if (window) window->set_status(st);
                          else if (!st.empty()) log(LogLevel::kInfo, "{}", st);
                        },
                        .hints = [&](std::vector<ControlMarker> markers) {
                          if (window) window->show_key_hints(std::move(markers));
                        },
                        .layout_shown = [&](const std::string& layout) {
                          if (window) window->show_toast("Layout: " + layout);
                        },
                    });
        for (auto& problem : game->load_profiles(app_support_dir() + "/profiles")) {
          log(LogLevel::kWarn, "game controls: {}", problem);
        }
        game->set_sensitivity(AppSettings::load(app_support_dir() + "/settings.json").mouse_sensitivity);
        raw_mouse::set_enabled(AppSettings::load(app_support_dir() + "/settings.json").raw_mouse);
      if (opts.control) {
        // Scripted touches use their own slot, apart from the mouse and game controls.
        m.own(std::make_unique<ControlServer>(*opts.control, [&, touch, touch_ptr, keyboard_ptr](const std::string& line) {
          return run_control_command(line, *touch, *touch_ptr, *keyboard_ptr, game.get());
        }));
      }
      if (!opts.headless) {
        // The mouse acts as one finger; keys go to the virtual keyboard.
        // The Library: shows a spinner at once, then the apps when the helper has them.
        open_editor = [&] {
          if (!game || !window) return;
          // Start from what's on disk: the file may have changed since Kestrel started,
          // and saving must never write an older copy back over it.
          for (auto& problem : game->load_profiles(app_support_dir() + "/profiles")) {
            log(LogLevel::kWarn, "game controls: {}", problem);
          }
          auto [name, markers] = game->editable_markers();
          const auto layouts = game->layouts();
          std::vector<std::string> titles;
          for (size_t i = 0; i < layouts.names.size(); ++i) {
            const uint16_t k = i < layouts.keys.size() ? layouts.keys[i] : 0;
            titles.push_back(k ? layouts.names[i] + " — " + key_label(k) : layouts.names[i]);
          }
          window->edit_controls(name, std::move(markers), titles, layouts.active, [&](ControlsEditorResult r) {
            using A = ControlsEditorResult::Action;
            std::optional<std::string> err;
            switch (r.action) {
              case A::kCancel: return;
              case A::kSave: err = game->save_markers(r.markers); break;
              case A::kSelectLayout: err = game->select_layout(r.layout); break;
              case A::kNewLayout: err = game->new_layout(r.name, r.markers); break;
              case A::kRenameLayout: err = game->rename_layout(r.name); break;
              case A::kDeleteLayout: err = game->delete_layout(); break;
              case A::kSetSwitchKey:
                err = game->save_markers(r.markers);  // keep what was edited so far
                if (!err) err = game->set_switch_key(r.key);
                if (!err) window->show_toast(r.key ? "Press " + key_label(r.key) + " in game for this layout"
                                                   : "No switch key for this layout");
                break;
            }
            if (err) window->show_message("Game controls", *err);
            if (r.action != A::kSave) open_editor();  // keep editing, now on that layout
          });
        };
        open_library = [&] {
          if (!window) return;
          window->show_library({}, true);
          if (!assistant) return;
          assistant->list_library([&](std::vector<AndroidAssistant::LibraryApp> apps) {
            std::vector<LibraryItem> items;
            for (auto& a : apps) {
              items.push_back({a.component, a.label, a.icon_file, a.foreground_file, a.background_file,
                               a.background_color, game && game->has_profile(a.package)});
            }
            // Games with controls first.
            std::stable_sort(items.begin(), items.end(),
                             [](const LibraryItem& x, const LibraryItem& y) { return x.has_controls > y.has_controls; });
            if (window) window->show_library(std::move(items), false);
          });
        };
        WindowInput input{
            .pointer = [touch, touch_ptr](bool down, bool up, int32_t x, int32_t y) {
              auto ev = down ? touch->down(0, x, y) : up ? touch->up(0) : touch->move(0, x, y);
              if (!ev.empty()) touch_ptr->send(ev);
            },
            .key = [&, keyboard_ptr](uint16_t key, bool pressed) {
              if (game) {
                game->key(key, pressed);  // game controls first; the rest reaches Android
                return;
              }
              const InputEvent ev[] = {{input::kEvKey, key, pressed ? 1 : 0},
                                       {input::kEvSyn, input::kSynReport, 0}};
              keyboard_ptr->send(ev);
            },
            .close_requested = [&m] { m.request_stop(); },
            .action = [&, keyboard_ptr](WindowAction a, const std::string& arg) {
              auto press = [keyboard_ptr](uint16_t key) {
                const InputEvent down[] = {{input::kEvKey, key, 1}, {input::kEvSyn, input::kSynReport, 0}};
                const InputEvent up[] = {{input::kEvKey, key, 0}, {input::kEvSyn, input::kSynReport, 0}};
                keyboard_ptr->send(down);
                keyboard_ptr->send(up);
              };
              switch (a) {
                case WindowAction::kHome: press(172); break;  // KEY_HOMEPAGE
                case WindowAction::kBack: press(158); break;  // KEY_BACK
                case WindowAction::kRecents:
                  if (assistant) assistant->shell("input keyevent KEYCODE_APP_SWITCH");
                  break;
                case WindowAction::kCloseApp:
                  if (assistant) assistant->close_foreground_app();
                  break;
                case WindowAction::kVolumeUp: press(115); break;    // KEY_VOLUMEUP
                case WindowAction::kVolumeDown: press(114); break;  // KEY_VOLUMEDOWN
                case WindowAction::kAppStore:
                  if (assistant) assistant->open_app_store();
                  break;
                case WindowAction::kShareFile:
                  if (assistant) assistant->push_file(arg);
                  break;
                case WindowAction::kPaste:
                  if (assistant) assistant->paste_text(arg);
                  break;
                case WindowAction::kSidebarChanged: {
                  const std::string path = app_support_dir() + "/settings.json";
                  AppSettings s = AppSettings::load(path);
                  s.show_sidebar = arg == "1";
                  s.save(path);
                  break;
                }
                case WindowAction::kShowLibrary:
                  if (open_library) open_library();
                  break;
                case WindowAction::kLaunchApp:
                  if (assistant) assistant->launch(arg);
                  if (window) window->hide_library();
                  break;
                case WindowAction::kReportProblem:
                  if (window) {
                    auto report = write_problem_report(app_logs_dir(), app_support_dir(), opts.android.value_or(""), KESTREL_VERSION_STRING);
                    if (report) {
                      window->show_message("Report saved to your Desktop",
                                           std::filesystem::path(*report).filename().string() +
                                               " has Kestrel's logs, settings and this Mac's model and macOS "
                                               "version (no passwords or files). Send it along when you "
                                               "describe the problem.");
                    } else {
                      window->show_message("Couldn't save the report", report.error().message);
                    }
                  }
                  break;
                case WindowAction::kSettings: {
                  const std::string path = app_support_dir() + "/settings.json";
                  const bool raw_mouse_was = AppSettings::load(path).raw_mouse;
                  window->show_settings(AppSettings::load(path), [&, path, raw_mouse_was](const AppSettings& s, bool now) {
                    std::error_code ec;
                    std::filesystem::create_directories(app_support_dir(), ec);
                    s.save(path);
                    if (game) game->set_sensitivity(s.mouse_sensitivity);
                    if (s.raw_mouse && !raw_mouse_was) raw_mouse::recalibrate();  // may be a different mouse now
                    raw_mouse::set_enabled(s.raw_mouse);
                    if (now) {
                      restart = true;
                      m.request_stop();
                    }
                  });
                  break;
                }
                case WindowAction::kInstallApk:
                  if (assistant) assistant->install_apk(arg);
                  else pending_apks.push_back(arg);
                  break;
                case WindowAction::kNewInstance:
                case WindowAction::kOpenInstance: {
                  if (!window) break;
                  if (a == WindowAction::kNewInstance) {
                    if (auto r = create_instance(app_support_dir(), arg); !r) {
                      window->show_message("Couldn't create the instance", r.error().message);
                      break;
                    }
                  }
                  if (arg == current_instance()) {
                    window->show_toast("That instance is this window");
                    break;
                  }
                  if (auto r = launch_instance(arg); !r) window->show_message("Couldn't open the instance", r.error().message);
                  else window->show_toast("Starting " + (arg.empty() ? std::string("the main Android") : "“" + arg + "”") + "…", 3);
                  break;
                }
                case WindowAction::kToggleRecording: {
                  if (!window) break;
                  if (recorder) {  // stop: the file is finished in the background
                    if (record_gpu) record_gpu->set_frame_tap({});
                    if (record_audio) record_audio->set_tap({});
                    window->set_recording(false);
                    auto r = std::move(recorder);
                    const double secs = r->seconds();
                    r->finish([&, r, secs](bool ok) {
                      if (!ok) {
                        window->show_message("Recording failed", "The video couldn't be written.");
                        return;
                      }
                      window->show_toast(std::format("Recording saved ({}:{:02}) — Movies › Kestrel", int(secs) / 60,
                                                     int(secs) % 60), 3);
                    });
                    break;
                  }
                  if (!record_gpu) {
                    window->show_message("Can't record", "Recording needs hardware graphics (Settings › Graphics).");
                    break;
                  }
                  const char* home = std::getenv("HOME");
                  const auto folder = std::filesystem::path(home ? home : ".") / "Movies" / "Kestrel";
                  std::error_code ec;
                  std::filesystem::create_directories(folder, ec);
                  const std::time_t now = std::time(nullptr);
                  char stamp[64];
                  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d at %H.%M.%S", std::localtime(&now));
                  auto started = ScreenRecorder::start((folder / (std::string("Kestrel ") + stamp + ".mov")).string(),
                                                       opts.display_w, opts.display_h);
                  if (!started) {
                    window->show_message("Can't record", started.error().message);
                    break;
                  }
                  recorder = std::move(*started);
                  auto rec = recorder;
                  record_gpu->set_frame_tap([rec](const Frame& f) { rec->video(f); });
                  if (record_audio) {
                    record_audio->set_tap([rec](std::span<const uint8_t> pcm, uint32_t rate, uint32_t ch) {
                      rec->audio(pcm, rate, ch);
                    });
                  }
                  window->set_recording(true);
                  break;
                }
                case WindowAction::kScreenshot: {
                  std::string path = arg;
                  if (path.empty()) {
                    const std::time_t now = std::time(nullptr);
                    char stamp[64];
                    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d at %H.%M.%S", std::localtime(&now));
                    const char* home = std::getenv("HOME");
                    path = std::string(home ? home : ".") + "/Desktop/Kestrel " + stamp + ".png";
                  }
                  // In direct display mode the window has no pixels: read this frame back.
                  if (screenshot_gpu && screenshot_gpu->direct_display()) screenshot_gpu->capture_frame();
                  if (window && window->save_screenshot(path)) {
                    window->show_message("Screenshot saved", std::filesystem::path(path).filename().string() + " is on your Desktop.");
                  } else if (window) {
                    window->show_message("Screenshot failed", "Couldn't save " + path);
                  }
                  break;
                }
                case WindowAction::kFactoryReset:
                  factory_reset = true;
                  m.request_stop();
                  break;
                case WindowAction::kToggleGameControls:
                  if (game) {
                    // Also picks up edits to the profile files.
                    for (auto& problem : game->load_profiles(app_support_dir() + "/profiles")) {
                      window->show_message("A game controls file has a problem", problem);
                    }
                    game->toggle_enabled();
                  }
                  break;
                case WindowAction::kEditGameControls:
                  if (open_editor) open_editor();
                  break;
                case WindowAction::kExportControls:
                  if (!game) break;
                  if (auto err = game->export_layout(arg)) window->show_message("Couldn't export the controls", *err);
                  else window->show_toast("Controls saved");
                  break;
                case WindowAction::kImportControls:
                  if (!game) break;
                  game->load_profiles(app_support_dir() + "/profiles");  // start from what's on disk
                  if (auto err = game->import_layout(arg)) {
                    window->show_message("Couldn't import the controls", *err);
                  } else {
                    window->show_toast("Controls imported as a new layout");
                    if (open_editor) open_editor();  // show what arrived
                  }
                  break;
              }
            },
            .mouse_button = [&](uint16_t button, bool pressed) {
              if (game) game->mouse_button(button, pressed);
            },
            .mouse_delta = [&](double dx, double dy) {
              if (game) game->mouse_delta(dx, dy);
            },
            .focus_lost = [&] {
              if (game) game->focus_lost();
            },
            .visibility = [&](bool visible) {
              // Optional: no CPU use (and no heat) while Kestrel is out of sight.
              if (!visible && !AppSettings::load(app_support_dir() + "/settings.json").pause_when_hidden) return;
              if (visible == !m.paused()) return;
              m.set_paused(!visible);
              log(LogLevel::kInfo, "android {}", visible ? "resumed" : "paused (Kestrel is hidden)");
            },
            .scroll = [touch, touch_ptr](int stage, int32_t x, int32_t y) {
              constexpr unsigned slot = Keymapper::kScrollSlot;
              log(LogLevel::kDebug, "scroll stage {} at {},{}", stage, x, y);
              auto ev = stage == 0 ? touch->down(slot, x, y) : stage == 1 ? touch->move(slot, x, y) : touch->up(slot);
              if (!ev.empty()) touch_ptr->send(ev);
            },
            .pinch = [touch, touch_ptr](int stage, double scale, int32_t x, int32_t y) {
              // Two fingers either side of the pointer, spreading with the pinch.
              const int32_t d = static_cast<int32_t>(80 * scale);
              std::vector<InputEvent> ev;
              auto add = [&](std::vector<InputEvent> e) { ev.insert(ev.end(), e.begin(), e.end()); };
              if (stage == 2) {
                add(touch->up(Keymapper::kPinchSlotA));
                add(touch->up(Keymapper::kPinchSlotB));
              } else {
                add(touch->down(Keymapper::kPinchSlotA, x - d, y));
                add(touch->down(Keymapper::kPinchSlotB, x + d, y));
              }
              if (!ev.empty()) touch_ptr->send(ev);
            },
        };
        const std::string instance = current_instance();
        window = MacWindow::create(instance.empty() ? "Kestrel" : "Kestrel — " + instance, opts.display_w,
                                   opts.display_h, std::move(input), opts.show_sidebar);
        if (auto note = take_crash_marker(app_logs_dir() + "/last-crash.txt")) {
          log(LogLevel::kWarn, "the last session crashed:\n{}", *note);
          window->ask("Kestrel quit unexpectedly last time",
                      "Sorry about that. A report with the details (logs and macOS's crash report, no "
                      "personal files) can be saved on your Desktop to send to the developer.",
                      "Save Report", [&] {
                        auto report = write_problem_report(app_logs_dir(), app_support_dir(),
                                                           opts.android.value_or(""), KESTREL_VERSION_STRING);
                        if (!report) window->show_message("Couldn't save the report", report.error().message);
                      });
        }
        sinks.push_back(window.get());

        }
      }
      DisplaySink* sink = sinks.empty() ? nullptr
                          : sinks.size() == 1 ? sinks[0]
                                              : m.own(std::make_unique<TeeSink>(sinks));
      std::unique_ptr<VirtioDevice> gpu;
      VirtioGpuGfxstream* direct_gpu = nullptr;
      if (gpu_accel) {
        auto accel = VirtioGpuGfxstream::create(
            m.memory(), {.width = opts.display_w,
                         .height = opts.display_h,
                         .sink = sink,
                         .host_gles = host_gles,
                         .refresh_hz = opts.refresh_hz,
                         .map = [&m](void* host, uint64_t gpa, uint64_t size) { return m.map_memory(host, gpa, size, true); },
                         .unmap = [&m](uint64_t gpa, uint64_t size) { return m.unmap_memory(gpa, size); },
                         .on_stall = [&](const std::string& what) {
                           // Where every thread is stuck, for the problem report.
                           const std::string file = app_logs_dir() + std::format("/gpu-stall-{}.txt", std::time(nullptr));
                           const std::string pid = std::to_string(::getpid());
                           const char* argv[] = {"/usr/bin/sample", pid.c_str(), "3", "-file", file.c_str(), nullptr};
                           pid_t child;
                           if (posix_spawn(&child, argv[0], nullptr, nullptr, const_cast<char**>(argv), environ) == 0) {
                             std::thread([child] { waitpid(child, nullptr, 0); }).detach();
                           }
                           if (!window) return;
                           if (game) game->focus_lost();  // free the mouse to answer
                           window->ask("Graphics stopped responding",
                                       "Android's picture hasn't updated for " +
                                           std::to_string(VirtioGpuGfxstream::kStallSeconds) + " seconds (" + what +
                                           "). It may still recover by itself. Restarting Android takes a few "
                                           "seconds and keeps your apps and data.",
                                       "Restart Android", [&] {
                                         restart = true;
                                         m.request_stop();
                                       });
                         },
                         .on_recover = [&](double seconds) {
                           if (!window) return;
                           window->dismiss_ask();
                           window->show_toast(std::format("Graphics recovered after {:.0f} s", seconds), 4);
                         }});
        if (accel) {
          direct_gpu = accel->get();
          if (opts.control) g_test_gpu = direct_gpu;
          gpu = std::move(*accel);
        } else {
          // Android was configured for acceleration; boot can't switch now. Tell the user.
          log(LogLevel::kError, "{}", accel.error().message);
          or_exit(Result<void>(std::unexpected(Error{accel.error().message + " (try --gpu software)"})));
        }
      } else {
        gpu = std::make_unique<VirtioGpu>(m.memory(), opts.display_w, opts.display_h, sink);
      }
      devices.push_back(std::move(gpu));
      // Frames go straight from the GPU into the window (no copy per frame). The host
      // GLES path keeps the readback: it runs without a Vulkan display.
      if (window && direct_gpu && !host_gles && !std::getenv("KESTREL_NO_DIRECT_DISPLAY")) {
        window->set_direct_display([direct_gpu](void* w, int x, int y, int ww, int wh, float dpr) {
          return direct_gpu->show_in_window(w, x, y, ww, wh, dpr);
        });
        screenshot_gpu = direct_gpu;
      }
      if (direct_gpu) {
        record_gpu = direct_gpu;
      }
      devices.push_back(std::move(touch_dev));
      devices.push_back(std::move(keyboard_dev));
    }
    const VirtioLayout virtio = or_exit(attach_virtio(m, opts.pci, std::move(devices)));

    std::pair<uint64_t, uint64_t> entry;
    if (android) {
      std::string cmdline = android->cmdline;
      if (opts.cmdline) cmdline += " " + *opts.cmdline;
      entry = or_exit(load_linux(m, std::move(android->kernel), std::move(android->initrd), cmdline, virtio));
    } else {
      std::optional<std::vector<uint8_t>> initrd;
      if (opts.initrd) initrd = or_exit(read_file(*opts.initrd));
      entry = or_exit(load_linux(m, or_exit(read_file(*opts.kernel)), std::move(initrd),
                                 opts.cmdline.value_or(kLinuxCmdline), virtio));
    }
    std::tie(pc, x0) = entry;
  }

  std::signal(SIGINT, [](int) {
    if (g_machine) g_machine->request_stop();
  });
  std::optional<Terminal> terminal;
  if (!opts.hello && !app_mode) {  // app mode has no terminal
    terminal.emplace([uart_ptr](uint8_t c) { uart_ptr->push_input(c); }, [&m] { m.request_stop(); });
  }

  if (window && android && adb_port) {
    assistant = std::make_unique<AndroidAssistant>(
        adb_port, [&](const std::string& st) { window->set_status(st); },
        [&](const std::string& t, const std::string& x) { window->show_message(t, x); });
    {
      std::error_code ec;
      std::filesystem::create_directories(app_support_dir() + "/cache", ec);
      assistant->set_downloader(
          [](const std::string& url, const std::string& path) { return download_file(url, path, nullptr); },
          app_support_dir() + "/cache");
    }
    assistant->set_boot_listener([&] { boot_progress.finish(); }, [&] {
      if (window) window->android_ready();
      if (open_library && AppSettings::load(app_support_dir() + "/settings.json").show_library) open_library();
    });
    if (game) {
      assistant->set_text_input_listener([&](bool typing) { game->set_text_input(typing); });
      assistant->set_foreground_listener([&](const std::string& package) { game->foreground_changed(package); });
    }
    set_open_files_handler([&](const std::string& path) {
      if (std::filesystem::path(path).extension() == ".apk") assistant->install_apk(path);
    });
    for (auto& apk : pending_apks) assistant->install_apk(apk);
  }

  Result<hv::StopInfo> stop = make_error("not run");
  if (window) {
    // AppKit owns the main thread; the VM runs beside it and ends the event loop when it stops.
    std::thread vm([&] {
      stop = m.run(pc, x0);
      quit_app_loop();
    });
    run_app_loop();
    if (recorder) {  // quitting while recording: keep what was recorded
      if (record_gpu) record_gpu->set_frame_tap({});
      if (record_audio) record_audio->set_tap({});
      auto done = std::make_shared<std::promise<void>>();
      auto finished = done->get_future();
      recorder->finish([done](bool) { done->set_value(); });
      finished.wait_for(std::chrono::seconds(10));
      recorder.reset();
    }
    m.request_stop();
    vm.join();
  } else {
    stop = m.run(pc, x0);
  }
  assistant.reset();
  game.reset();
  terminal.reset();
  // Android asked to reboot (e.g. from its power menu): start it again.
  if (stop && stop->reason == hv::StopReason::kReset && opts.android) restart = true;
  if ((factory_reset || restart) && opts.android) {
    // Start again with the same arguments (app mode re-reads the settings);
    // a factory reset erases the user data first.
    machine.reset();
    window.reset();
    std::error_code ec;
    for (const char* f : {"userdata.img", "metadata.img", "misc.img"}) {
      if (factory_reset) std::filesystem::remove(std::filesystem::path(*opts.android) / "instance" / f, ec);
    }
    char self[4096];
    uint32_t size = sizeof(self);
    if (_NSGetExecutablePath(self, &size) == 0) {
      execv(self, app_mode ? std::vector<char*>{self, nullptr}.data() : argv);
    }
    return 1;
  }
  if (!stop) {
    log(LogLevel::kError, "{}", stop.error().message);
    return 1;
  }
  static constexpr const char* kReasons[] = {"guest powered off", "guest requested reset",
                                             "stopped by user", "CPU off"};
  log(LogLevel::kInfo, "{}", kReasons[static_cast<int>(stop->reason)]);
  return 0;
}
