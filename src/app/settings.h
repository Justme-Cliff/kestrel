#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kestrel {

// User settings for the app, stored as JSON in Application Support.
// Changes to the hardware settings apply the next time Android starts.
struct AppSettings {
  uint32_t width = 1280, height = 720;  // Android screen resolution
  uint32_t dpi = 0;  // 0: automatic (tablet layout for the resolution)
  uint32_t ram_mb = 3072;
  uint32_t cpus = 4;
  bool gpu = true;             // hardware graphics (gfxstream) vs software
  bool show_sidebar = true;
  double mouse_sensitivity = 1.0;  // game aim mode
  bool low_latency_display = true;  // newest frame at once instead of a vsync queue
  bool raw_mouse = true;           // aim with raw mouse input (no macOS acceleration)
  uint32_t refresh_hz = 60;        // Android's display refresh rate = its frame-rate cap
  std::string performance = "balanced";  // "battery", "balanced", "max" or "custom"
  bool pause_when_hidden = false;  // stop Android while Kestrel is minimized
  bool show_library = true;        // show the game Library when Android is ready

  static AppSettings load(const std::string& path);
  bool save(const std::string& path) const;

  // Sensible defaults for this Mac (RAM and CPU cores it has).
  static AppSettings defaults_for_host();
  // The density that gives Android's tablet layout (working taskbar with Back/Home/
  // Recents) for a screen height: at least 600 dp on the short side.
  static uint32_t tablet_dpi(uint32_t height);
  uint32_t effective_dpi() const { return dpi ? dpi : tablet_dpi(height); }
  static std::vector<std::pair<uint32_t, uint32_t>> resolution_choices(uint32_t screen_w, uint32_t screen_h);
  // Applies a performance preset ("battery", "balanced", "max") for this Mac;
  // `max_refresh` is the display's highest refresh rate.
  void apply_performance(const std::string& mode, uint32_t max_refresh);
};

// Physical memory and performance cores of this Mac.
uint64_t host_memory_mb();
uint32_t host_cpu_count();

}  // namespace kestrel
