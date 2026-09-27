#include "keymap/touch.h"

#include <algorithm>

namespace kestrel {

using namespace input;

int32_t TouchScreen::clamp_x(int32_t x) const { return std::clamp<int32_t>(x, 0, int32_t(width_) - 1); }
int32_t TouchScreen::clamp_y(int32_t y) const { return std::clamp<int32_t>(y, 0, int32_t(height_) - 1); }

unsigned TouchScreen::active_count() const {
  return static_cast<unsigned>(std::ranges::count_if(slots_, [](const auto& s) { return s.has_value(); }));
}

int TouchScreen::first_active() const {
  for (unsigned i = 0; i < kSlots; ++i) {
    if (slots_[i]) return static_cast<int>(i);
  }
  return -1;
}

bool TouchScreen::is_down(unsigned slot) const {
  std::lock_guard lock(mutex_);
  return slot < kSlots && slots_[slot].has_value();
}

std::vector<InputEvent> TouchScreen::down(unsigned slot, int32_t x, int32_t y) {
  std::lock_guard lock(mutex_);
  if (slot >= kSlots) return {};
  if (slots_[slot]) return move_locked(slot, x, y);
  x = clamp_x(x);
  y = clamp_y(y);
  const bool first = active_count() == 0;
  slots_[slot] = Contact{next_tracking_id_, x, y};
  next_tracking_id_ = next_tracking_id_ % 65535 + 1;
  std::vector<InputEvent> e = {
      {kEvAbs, kAbsMtSlot, int32_t(slot)},
      {kEvAbs, kAbsMtTrackingId, slots_[slot]->tracking_id},
      {kEvAbs, kAbsMtPositionX, x},
      {kEvAbs, kAbsMtPositionY, y},
  };
  if (first) {  // single-touch emulation follows the first finger
    e.push_back({kEvKey, kBtnTouch, 1});
    e.push_back({kEvAbs, kAbsX, x});
    e.push_back({kEvAbs, kAbsY, y});
  }
  e.push_back({kEvSyn, kSynReport, 0});
  return e;
}

std::vector<InputEvent> TouchScreen::move(unsigned slot, int32_t x, int32_t y) {
  std::lock_guard lock(mutex_);
  return move_locked(slot, x, y);
}

std::vector<InputEvent> TouchScreen::move_locked(unsigned slot, int32_t x, int32_t y) {
  if (slot >= kSlots || !slots_[slot]) return {};
  x = clamp_x(x);
  y = clamp_y(y);
  if (slots_[slot]->x == x && slots_[slot]->y == y) return {};
  slots_[slot]->x = x;
  slots_[slot]->y = y;
  std::vector<InputEvent> e = {
      {kEvAbs, kAbsMtSlot, int32_t(slot)},
      {kEvAbs, kAbsMtPositionX, x},
      {kEvAbs, kAbsMtPositionY, y},
  };
  if (first_active() == int(slot)) {  // single-touch emulation follows the first finger only
    e.push_back({kEvAbs, kAbsX, x});
    e.push_back({kEvAbs, kAbsY, y});
  }
  e.push_back({kEvSyn, kSynReport, 0});
  return e;
}

std::vector<InputEvent> TouchScreen::handover(unsigned from, unsigned to, int32_t x, int32_t y) {
  std::lock_guard lock(mutex_);
  if (from >= kSlots || to >= kSlots || from == to || slots_[to]) return {};
  x = clamp_x(x);
  y = clamp_y(y);
  std::vector<InputEvent> e;
  const bool had = slots_[from].has_value();
  // New finger first, then the old one lifts: one SYN_REPORT for both.
  slots_[to] = Contact{next_tracking_id_, x, y};
  next_tracking_id_ = next_tracking_id_ % 65535 + 1;
  e.push_back({kEvAbs, kAbsMtSlot, int32_t(to)});
  e.push_back({kEvAbs, kAbsMtTrackingId, slots_[to]->tracking_id});
  e.push_back({kEvAbs, kAbsMtPositionX, x});
  e.push_back({kEvAbs, kAbsMtPositionY, y});
  if (had) {
    slots_[from].reset();
    e.push_back({kEvAbs, kAbsMtSlot, int32_t(from)});
    e.push_back({kEvAbs, kAbsMtTrackingId, -1});
  } else if (active_count() == 1) {
    e.push_back({kEvKey, kBtnTouch, 1});
  }
  e.push_back({kEvSyn, kSynReport, 0});
  return e;
}

std::vector<InputEvent> TouchScreen::up(unsigned slot) {
  std::lock_guard lock(mutex_);
  if (slot >= kSlots || !slots_[slot]) return {};
  slots_[slot].reset();
  std::vector<InputEvent> e = {
      {kEvAbs, kAbsMtSlot, int32_t(slot)},
      {kEvAbs, kAbsMtTrackingId, -1},
  };
  if (active_count() == 0) e.push_back({kEvKey, kBtnTouch, 0});
  e.push_back({kEvSyn, kSynReport, 0});
  return e;
}

}  // namespace kestrel
