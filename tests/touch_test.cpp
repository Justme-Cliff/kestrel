#include "keymap/touch.h"
#include "devices/virtio/virtio_input.h"
#include <deque>
#include <map>

#include <gtest/gtest.h>

using namespace kestrel;
using namespace kestrel::input;

namespace {
bool has(const std::vector<InputEvent>& ev, uint16_t type, uint16_t code, int32_t value) {
  for (auto& e : ev) {
    if (e.type == type && e.code == code && e.value == value) return true;
  }
  return false;
}
}  // namespace

TEST(TouchScreen, TapProducesProtocolB) {
  TouchScreen t(1280, 720);
  auto d = t.down(0, 100, 200);
  EXPECT_TRUE(has(d, kEvAbs, kAbsMtSlot, 0));
  EXPECT_TRUE(has(d, kEvAbs, kAbsMtPositionX, 100));
  EXPECT_TRUE(has(d, kEvKey, kBtnTouch, 1));
  EXPECT_EQ(d.back().type, kEvSyn);
  auto u = t.up(0);
  EXPECT_TRUE(has(u, kEvAbs, kAbsMtTrackingId, -1));
  EXPECT_TRUE(has(u, kEvKey, kBtnTouch, 0));
  EXPECT_TRUE(t.up(0).empty());
}

TEST(TouchScreen, SecondFingerKeepsTouchDown) {
  TouchScreen t(1280, 720);
  t.down(0, 10, 10);
  auto d = t.down(1, 20, 20);
  EXPECT_FALSE(has(d, kEvKey, kBtnTouch, 1));  // already down
  auto u = t.up(0);
  EXPECT_FALSE(has(u, kEvKey, kBtnTouch, 0));  // finger 1 still down
  EXPECT_TRUE(has(t.up(1), kEvKey, kBtnTouch, 0));
}

TEST(TouchScreen, ClampsAndSkipsNoOpMoves) {
  TouchScreen t(1280, 720);
  auto d = t.down(0, -5, 5000);
  EXPECT_TRUE(has(d, kEvAbs, kAbsMtPositionX, 0));
  EXPECT_TRUE(has(d, kEvAbs, kAbsMtPositionY, 719));
  EXPECT_TRUE(t.move(0, -1, 9999).empty());
  EXPECT_FALSE(t.move(0, 50, 50).empty());
  EXPECT_TRUE(t.move(5, 1, 1).empty());  // slot not down
}

// A guest that stalls must not lose a lift (stuck finger) or a finger's final
// position; only intermediate moves are merged.
TEST(InputBacklog, CompactionKeepsPressesLiftsAndLastPositions) {
  TouchScreen t{1000, 500};
  std::deque<InputEvent> q;
  auto add = [&](std::vector<InputEvent> ev) { q.insert(q.end(), ev.begin(), ev.end()); };
  add(t.down(1, 100, 100));                                    // joystick down
  for (int i = 0; i < 200; ++i) add(t.move(1, 101 + i % 50, 100));  // wiggle
  add(t.down(2, 600, 250));                                    // aim down
  for (int i = 0; i < 300; ++i) add(t.move(2, 600 + i, 250));  // flick
  add(t.up(1));                                                // stop running
  for (int i = 0; i < 100; ++i) add(t.move(2, 300 + i, 260));
  const size_t before = q.size();
  VirtioInput::compact_backlog(q);
  EXPECT_LT(q.size(), before / 10);
  // Replay: the slot state after the compacted stream equals the original's.
  std::map<int, int> id, x, y;
  int slot = 0;
  for (auto& e : q) {
    if (e.type == kEvAbs && e.code == kAbsMtSlot) slot = e.value;
    if (e.type == kEvAbs && e.code == kAbsMtTrackingId) id[slot] = e.value;
    if (e.type == kEvAbs && e.code == kAbsMtPositionX) x[slot] = e.value;
    if (e.type == kEvAbs && e.code == kAbsMtPositionY) y[slot] = e.value;
  }
  EXPECT_EQ(id[1], -1);  // the lift survived
  EXPECT_GE(id[2], 0);
  EXPECT_EQ(x[2], 399);
  EXPECT_EQ(y[2], 260);
  EXPECT_EQ(q.back().type, kEvSyn);
  // The joystick's last position before it lifted is kept.
  int last_js_x = -1;
  slot = 0;
  for (auto& e : q) {
    if (e.type == kEvAbs && e.code == kAbsMtSlot) slot = e.value;
    if (slot == 1 && e.type == kEvAbs && e.code == kAbsMtPositionX) last_js_x = e.value;
  }
  EXPECT_EQ(last_js_x, 101 + 199 % 50);
}
