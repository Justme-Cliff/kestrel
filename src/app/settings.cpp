#include "app/settings.h"

#include <sys/sysctl.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "keymap/json.h"

namespace kestrel {

uint64_t host_memory_mb() {
  uint64_t bytes = 0;
  size_t len = sizeof(bytes);
  sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0);
  return bytes >> 20;
}

uint32_t host_cpu_count() {
  int n = 0;
  size_t len = sizeof(n);
  sysctlbyname("hw.logicalcpu", &n, &len, nullptr, 0);
  return n > 0 ? static_cast<uint32_t>(n) : 4;
}

uint32_t AppSettings::tablet_dpi(uint32_t height) {
  // 600 dp = height * 160 / dpi; round down to a multiple of 20, at most 320.
  return std::clamp<uint32_t>(height * 160 / 600 / 20 * 20, 120, 320);
}

AppSettings AppSettings::defaults_for_host() {
  AppSettings s;
  const uint64_t mem = host_memory_mb();
  // Leave macOS at least half the memory on small Macs.
  s.ram_mb = mem <= 8192 ? 3072 : mem <= 16384 ? 4096 : 6144;
  s.cpus = std::clamp<uint32_t>(host_cpu_count() - 2, 2, 8);
  return s;
}

AppSettings AppSettings::load(const std::string& path) {
  AppSettings s = defaults_for_host();
  std::ifstream in(path);
  if (!in) return s;
  std::stringstream text;
  text << in.rdbuf();
  auto j = Json::parse(text.str());
  if (!j) return s;
  auto u32 = [&](const char* key, uint32_t& dst, uint32_t lo, uint32_t hi) {
    if (auto v = (*j)[key].number(); v && *v >= lo && *v <= hi) dst = static_cast<uint32_t>(*v);
  };
  u32("width", s.width, 640, 3840);
  u32("height", s.height, 360, 2160);
  u32("dpi", s.dpi, 0, 640);
  if (s.dpi != 0 && s.dpi < 120) s.dpi = 0;
  u32("ram_mb", s.ram_mb, 1536, 16384);
  u32("cpus", s.cpus, 1, 8);
  s.gpu = (*j)["gpu"].boolean().value_or(s.gpu);
  s.show_sidebar = (*j)["show_sidebar"].boolean().value_or(s.show_sidebar);
  if (auto v = (*j)["mouse_sensitivity"].number(); v && *v > 0.05 && *v < 20) s.mouse_sensitivity = *v;
  u32("refresh_hz", s.refresh_hz, 30, 120);
  if (auto v = (*j)["performance"].string(); v && (*v == "battery" || *v == "balanced" || *v == "max" || *v == "custom")) {
    s.performance = *v;
  }
  s.pause_when_hidden = (*j)["pause_when_hidden"].boolean().value_or(s.pause_when_hidden);
  s.show_library = (*j)["show_library"].boolean().value_or(s.show_library);
  s.raw_mouse = (*j)["raw_mouse"].boolean().value_or(s.raw_mouse);
  s.low_latency_display = (*j)["low_latency_display"].boolean().value_or(s.low_latency_display);
  return s;
}

bool AppSettings::save(const std::string& path) const {
  Json::Object o{{"width", Json(double(width))},   {"height", Json(double(height))},
                 {"dpi", Json(double(dpi))},       {"ram_mb", Json(double(ram_mb))},
                 {"cpus", Json(double(cpus))},     {"gpu", Json(gpu)},
                 {"show_sidebar", Json(show_sidebar)}, {"mouse_sensitivity", Json(mouse_sensitivity)},
                 {"refresh_hz", Json(double(refresh_hz))}, {"performance", Json(performance)},
                 {"pause_when_hidden", Json(pause_when_hidden)}, {"show_library", Json(show_library)},
                 {"raw_mouse", Json(raw_mouse)}, {"low_latency_display", Json(low_latency_display)}};
  std::ofstream out(path, std::ios::trunc);
  out << Json(std::move(o)).dump() << "\n";
  return bool(out);
}

void AppSettings::apply_performance(const std::string& mode, uint32_t max_refresh) {
  const uint64_t mem = host_memory_mb();
  const uint32_t cores = host_cpu_count();
  performance = mode;
  if (mode == "battery") {
    // Fewer frames and cores: least heat and battery drain.
    refresh_hz = 30;
    width = 1280, height = 720;
    cpus = 2;
    ram_mb = 3072;
  } else if (mode == "max") {
    // As many frames as the Mac's display shows, all spare cores; 720p keeps FPS high.
    refresh_hz = std::clamp<uint32_t>(max_refresh, 60, 120);
    width = 1280, height = 720;
    cpus = std::clamp<uint32_t>((cores - 1) & ~1u, 2, 8);
    ram_mb = mem >= 12288 ? 4096 : 3072;
  } else if (mode == "balanced") {
    refresh_hz = 60;
    width = 1280, height = 720;
    cpus = std::clamp<uint32_t>(cores >= 6 ? 4 : 2, 2, 8);
    ram_mb = 3072;
  }
}

std::vector<std::pair<uint32_t, uint32_t>> AppSettings::resolution_choices(uint32_t screen_w, uint32_t screen_h) {
  std::vector<std::pair<uint32_t, uint32_t>> r = {{1280, 720}, {1600, 900}, {1920, 1080}};
  if (screen_w && screen_h) {
    // "Match my screen": the display's aspect ratio at a size games handle well,
    // so full screen has no black bars. Even numbers for the GPU.
    const uint32_t h = std::min<uint32_t>(screen_h, 1080) & ~1u;
    const uint32_t w = static_cast<uint32_t>(uint64_t(h) * screen_w / screen_h) & ~1u;
    if (std::find(r.begin(), r.end(), std::pair{w, h}) == r.end()) r.push_back({w, h});
  }
  return r;
}

}  // namespace kestrel
