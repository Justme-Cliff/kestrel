#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "devices/virtio/virtio_input.h"

namespace kestrel {

// Turns "finger N is down at (x, y)" into Linux multi-touch protocol B events
// (Documentation/input/multi-touch-protocol.rst). Pure logic: the caller sends
// the returned events to the virtio touchscreen. Shared by the window's mouse,
// the keymapper and scripted input, so it's thread-safe.
class TouchScreen {
 public:
  static constexpr unsigned kSlots = 21;

  TouchScreen(uint32_t width, uint32_t height) : width_(width), height_(height) {}

  // Each returns a complete batch ending in SYN_REPORT (empty if nothing changed).
  std::vector<InputEvent> down(unsigned slot, int32_t x, int32_t y);
  std::vector<InputEvent> move(unsigned slot, int32_t x, int32_t y);
  std::vector<InputEvent> up(unsigned slot);
  // Lifts the finger on `from` and lands a new one on `to` at (x, y) in the same
  // input frame, so apps never see a moment with that finger missing.
  std::vector<InputEvent> handover(unsigned from, unsigned to, int32_t x, int32_t y);

  bool is_down(unsigned slot) const;
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

 private:
  struct Contact {
    int32_t tracking_id, x, y;
  };
  int32_t clamp_x(int32_t x) const;
  int32_t clamp_y(int32_t y) const;
  unsigned active_count() const;
  int first_active() const;  // lowest slot with a finger, or -1
  std::vector<InputEvent> move_locked(unsigned slot, int32_t x, int32_t y);

  mutable std::mutex mutex_;
  uint32_t width_, height_;
  std::array<std::optional<Contact>, kSlots> slots_{};
  int32_t next_tracking_id_ = 1;
};

}  // namespace kestrel
