#include "keymap/keymapper.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace kestrel;
using namespace kestrel::input;

namespace {

// Last reported (x, y) for a slot in a batch of events; nullopt if none.
std::optional<std::pair<int32_t, int32_t>> position(const std::vector<InputEvent>& ev, unsigned slot) {
  int32_t cur = -1;
  std::optional<int32_t> x, y;
  for (auto& e : ev) {
    if (e.type == kEvAbs && e.code == kAbsMtSlot) cur = e.value;
    if (cur == int32_t(slot) && e.type == kEvAbs && e.code == kAbsMtPositionX) x = e.value;
    if (cur == int32_t(slot) && e.type == kEvAbs && e.code == kAbsMtPositionY) y = e.value;
  }
  if (!x || !y) return std::nullopt;
  return std::pair{*x, *y};
}

bool lifted(const std::vector<InputEvent>& ev, unsigned slot) {
  int32_t cur = -1;
  for (auto& e : ev) {
    if (e.type == kEvAbs && e.code == kAbsMtSlot) cur = e.value;
    if (cur == int32_t(slot) && e.code == kAbsMtTrackingId && e.value == -1) return true;
  }
  return false;
}

struct KeymapperTest : ::testing::Test {
  TouchScreen touch{1000, 500};
  Keymapper km{*parse_profile(default_free_fire_profile()), touch};
};

}  // namespace

TEST(Json, ParsesAndRejects) {
  auto j = Json::parse(R"({"a": [1, 2.5, "x\n"], "b": {"c": true}, "d": null})");
  ASSERT_TRUE(j);
  EXPECT_EQ((*j)["a"].array()->size(), 3u);
  EXPECT_EQ((*(*j)["a"].array())[1].number(), 2.5);
  EXPECT_EQ((*(*j)["a"].array())[2].string(), "x\n");
  EXPECT_EQ((*j)["b"]["c"].boolean(), true);
  EXPECT_TRUE((*j)["d"].is_null());
  EXPECT_TRUE((*j)["missing"].is_null());
  EXPECT_FALSE(Json::parse("{\"a\": }"));
  EXPECT_FALSE(Json::parse("[1, 2"));
  EXPECT_FALSE(Json::parse(std::string(100, '[')));  // too deep
  auto round = Json::parse(j->dump());
  ASSERT_TRUE(round);
  EXPECT_EQ((*round)["b"]["c"].boolean(), true);
}

TEST(Profile, DefaultFreeFireParses) {
  auto p = parse_profile(default_free_fire_profile());
  ASSERT_TRUE(p) << p.error().message;
  EXPECT_EQ(p->package, "com.dts.freefireth");
  EXPECT_TRUE(p->joystick && p->aim && p->fire);
  EXPECT_EQ(p->aim_toggle, 56);  // Left Option: F1 is brightness on a Mac
}

TEST(Profile, RejectsBadInput) {
  EXPECT_FALSE(parse_profile(R"({"controls": [{"type": "tap", "key": "W", "x": 2, "y": 0.5}]})"));
  EXPECT_FALSE(parse_profile(R"({"controls": [{"type": "tap", "key": "NOPE", "x": 0.5, "y": 0.5}]})"));
  EXPECT_FALSE(parse_profile(R"({"controls": [{"type": "laser", "x": 0.5, "y": 0.5}]})"));
  EXPECT_FALSE(parse_profile(R"({"name": "no controls"})"));
}

TEST_F(KeymapperTest, TapHoldsWhileKeyIsDown) {
  auto down = km.key(57, true);  // SPACE -> jump
  EXPECT_TRUE(down.consumed);
  auto pos = position(down.events, Keymapper::kFirstTapSlot);  // first free finger
  ASSERT_TRUE(pos);
  EXPECT_EQ(pos->first, 958);
  EXPECT_EQ(pos->second, 346);
  EXPECT_TRUE(lifted(km.key(57, false).events, Keymapper::kFirstTapSlot));
}

TEST_F(KeymapperTest, HeldTapsGetTheirOwnFingers) {
  auto jump = km.key(57, true);   // SPACE
  auto crouch = km.key(46, true);  // C
  EXPECT_TRUE(position(jump.events, Keymapper::kFirstTapSlot));
  EXPECT_TRUE(position(crouch.events, Keymapper::kFirstTapSlot + 1));
  EXPECT_TRUE(km.key(57, true).events.empty());  // repeat while held: no new finger
  EXPECT_TRUE(lifted(km.key(57, false).events, Keymapper::kFirstTapSlot));
  EXPECT_TRUE(lifted(km.key(46, false).events, Keymapper::kFirstTapSlot + 1));
}

TEST_F(KeymapperTest, UnboundKeysPassThrough) {
  auto r = km.key(24, true);  // O (not bound)
  EXPECT_FALSE(r.consumed);
  EXPECT_TRUE(r.events.empty());
}

TEST_F(KeymapperTest, JoystickPushesFromCenter) {
  auto w = km.key(17, true);  // W: finger lands at the center, then moves up
  auto pos = position(w.events, Keymapper::kJoystickSlot);
  ASSERT_TRUE(pos);
  EXPECT_EQ(pos->first, 170);
  EXPECT_LT(pos->second, 216);
  auto wd = km.key(32, true);  // + D: diagonal, same radius
  auto diag = position(wd.events, Keymapper::kJoystickSlot);
  ASSERT_TRUE(diag);
  EXPECT_GT(diag->first, 170);
  const double r = std::hypot(diag->first - 170.0, diag->second - 216.0);
  EXPECT_NEAR(r, 0.112 * 0.60 * 500, 1.5);  // inside the ring: no sprint lock
  km.key(17, false);
  EXPECT_TRUE(lifted(km.key(32, false).events, Keymapper::kJoystickSlot));
}

TEST_F(KeymapperTest, AimOnlyInAimModeAndRecenters) {
  EXPECT_TRUE(km.mouse_move(10, 0).empty());
  EXPECT_TRUE(km.mouse_button(kMouseLeft, true).empty());  // plain clicks stay clicks
  km.key(56, true);  // Option toggles aim mode
  km.key(56, false);
  EXPECT_TRUE(km.aim_mode());
  auto first = km.mouse_move(10, 5);
  auto p = position(first, Keymapper::kAimSlot);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->first, 630);
  EXPECT_EQ(p->second, 235);
  // A big move leaves the area: a new finger lands at the start and the old one lifts,
  // in a single input frame (no moment without an aiming finger).
  auto jump = km.mouse_move(1000, 0);
  EXPECT_TRUE(lifted(jump, Keymapper::kAimSlot));
  EXPECT_EQ(position(jump, Keymapper::kAimSlotAlt)->first, 620);
  EXPECT_EQ(std::ranges::count_if(jump, [](const InputEvent& e) { return e.type == kEvSyn; }), 1);
  // The next re-centre hands back to the first slot.
  auto jump2 = km.mouse_move(1000, 0);
  EXPECT_TRUE(lifted(jump2, Keymapper::kAimSlotAlt));
  EXPECT_EQ(position(jump2, Keymapper::kAimSlot)->first, 620);
  auto fire = km.mouse_button(kMouseLeft, true);
  EXPECT_EQ(position(fire, Keymapper::kFireSlot)->first, 847);
  // Leaving aim mode lifts the aiming and firing fingers.
  km.key(56, true);
  EXPECT_FALSE(km.aim_mode());
}

TEST_F(KeymapperTest, ReleaseAllLiftsEverything) {
  km.key(17, true);
  km.key(57, true);
  auto r = km.release_all();
  EXPECT_TRUE(lifted(r, Keymapper::kJoystickSlot));
  EXPECT_TRUE(lifted(r, Keymapper::kFirstTapSlot));
}

TEST(Profile, RoundTripsThroughJson) {
  auto p = parse_profile(default_free_fire_profile());
  ASSERT_TRUE(p);
  auto again = parse_profile(profile_to_json(*p));
  ASSERT_TRUE(again) << again.error().message;
  EXPECT_EQ(again->taps.size(), p->taps.size());
  EXPECT_EQ(again->taps[1].key, p->taps[1].key);
  EXPECT_DOUBLE_EQ(again->aim->left, p->aim->left);
  EXPECT_EQ(again->aim_toggle, p->aim_toggle);
}

TEST(Profile, MarkersMoveControls) {
  auto p = parse_profile(default_free_fire_profile());
  ASSERT_TRUE(p);
  auto markers = profile_markers(*p);
  ASSERT_EQ(markers.size(), 3 + p->taps.size());
  for (auto& m : markers) {
    if (m.kind == ControlMarker::Kind::kTap && m.index == 1) m.x = 0.5, m.y = 0.25;
    if (m.kind == ControlMarker::Kind::kAim) m.x = 0.0;  // outside its area: clamped
  }
  apply_markers(*p, markers);
  EXPECT_DOUBLE_EQ(p->taps[1].x, 0.5);
  EXPECT_DOUBLE_EQ(p->taps[1].y, 0.25);
  EXPECT_GT(p->aim->x, p->aim->left);
}

// Long random play session (like the Phase B robot, without a VM): every batch must
// be a valid protocol-B frame, fingers stay on screen, the joystick lets go as soon as
// no direction is held, and nothing is left pressed at the end.
TEST_F(KeymapperTest, RandomPlaySessionKeepsTouchStateConsistent) {
  constexpr uint16_t W = 17, A = 30, S = 31, D = 32, kAlt = 56;
  const uint16_t taps[] = {57, 46, 44, 19, 33, 34, 35, 2, 3, 4, 42, 15, 45, 16, 18};
  const uint16_t unmapped[] = {24, 39, 51, 52, 13, 12};
  std::vector<bool> down(TouchScreen::kSlots, false);
  auto check = [&](const std::vector<InputEvent>& ev) {
    if (ev.empty()) return;
    ASSERT_EQ(ev.back().type, kEvSyn) << "batch must end with SYN_REPORT";
    int32_t cur = 0;
    for (auto& e : ev) {
      if (e.type != kEvAbs) continue;
      if (e.code == kAbsMtSlot) {
        ASSERT_GE(e.value, 0);
        ASSERT_LT(e.value, int32_t(TouchScreen::kSlots));
        cur = e.value;
      } else if (e.code == kAbsMtTrackingId) {
        if (e.value >= 0) {
          ASSERT_FALSE(down[cur]) << "slot " << cur << " pressed twice";
          down[cur] = true;
        } else {
          ASSERT_TRUE(down[cur]) << "slot " << cur << " lifted while up";
          down[cur] = false;
        }
      } else if (e.code == kAbsMtPositionX) {
        ASSERT_TRUE(down[cur]) << "moved a lifted finger";
        ASSERT_GE(e.value, 0);
        ASSERT_LT(e.value, 1000);
      } else if (e.code == kAbsMtPositionY) {
        ASSERT_TRUE(down[cur]) << "moved a lifted finger";
        ASSERT_GE(e.value, 0);
        ASSERT_LT(e.value, 500);
      }
    }
  };
  auto key = [&](uint16_t k, bool p) { check(km.key(k, p).events); };

  std::srand(7);
  auto rnd = [] { return double(std::rand()) / RAND_MAX; };
  key(kAlt, true);
  key(kAlt, false);
  ASSERT_TRUE(km.aim_mode());
  std::vector<uint16_t> dirs, held_taps;
  bool firing = false;
  for (int step = 0; step < 200000 && !HasFatalFailure(); ++step) {
    if (rnd() < 0.02) {  // new WASD combo, often none (stand still)
      for (auto k : dirs) key(k, false);
      if (dirs.empty() == false) EXPECT_FALSE(down[Keymapper::kJoystickSlot]) << "joystick stuck at step " << step;
      dirs.clear();
      const uint16_t all[] = {W, A, S, D};
      for (auto k : all)
        if (rnd() < 0.3) dirs.push_back(k);
      for (auto k : dirs) key(k, true);
      auto has = [&](uint16_t k) { return std::find(dirs.begin(), dirs.end(), k) != dirs.end(); };
      const bool moving = has(W) != has(S) || has(A) != has(D);  // opposite keys cancel
      EXPECT_EQ(down[Keymapper::kJoystickSlot], moving);
    }
    double dx = (rnd() - 0.5) * 12, dy = (rnd() - 0.5) * 8;
    if (rnd() < 0.08) dx += (rnd() < 0.5 ? -1 : 1) * (80 + rnd() * 900);  // fast flicks
    if (rnd() < 0.05) dy += (rnd() < 0.5 ? -1 : 1) * (40 + rnd() * 400);
    check(km.mouse_move(dx, dy));
    ASSERT_TRUE(down[Keymapper::kAimSlot] || down[Keymapper::kAimSlotAlt]) << "aim finger lost";
    if (rnd() < 0.02) {
      firing = !firing;
      check(km.mouse_button(kMouseLeft, firing));
    }
    if (rnd() < 0.01) {
      check(km.mouse_button(kMouseRight, true));
      check(km.mouse_button(kMouseRight, false));
    }
    if (rnd() < 0.03) {
      const uint16_t k = taps[std::rand() % std::size(taps)];
      if (std::find(held_taps.begin(), held_taps.end(), k) == held_taps.end()) {
        key(k, true);
        held_taps.push_back(k);
      }
    }
    if (!held_taps.empty() && rnd() < 0.1) {
      key(held_taps.front(), false);
      held_taps.erase(held_taps.begin());
    }
    if (rnd() < 0.01) {  // keys the profile doesn't map must never touch the screen
      const uint16_t k = unmapped[std::rand() % std::size(unmapped)];
      auto r = km.key(k, true);
      EXPECT_TRUE(r.events.empty());
      EXPECT_TRUE(km.key(k, false).events.empty());
    }
  }
  for (auto k : dirs) key(k, false);
  for (auto k : held_taps) key(k, false);
  if (firing) check(km.mouse_button(kMouseLeft, false));
  key(kAlt, true);
  key(kAlt, false);
  for (unsigned s = 0; s < TouchScreen::kSlots; ++s) EXPECT_FALSE(down[s]) << "slot " << s << " left pressed";
}

TEST(Keymapper, HoldToUseTapsOnPressAndOnRelease) {
  auto p = parse_profile(R"({"package": "g", "switch_key": "F6", "controls": [
      {"type": "tap", "key": "MOUSE_RIGHT", "x": 0.9, "y": 0.5, "mode": "hold_to_use"}]})");
  ASSERT_TRUE(p) << p.error().message;
  EXPECT_EQ(p->switch_key, *key_from_name("F6"));
  EXPECT_TRUE(p->taps[0].hold_to_use);
  auto again = parse_profile(profile_to_json(*p));  // survives a save
  ASSERT_TRUE(again);
  EXPECT_TRUE(again->taps[0].hold_to_use);
  EXPECT_EQ(again->switch_key, p->switch_key);

  TouchScreen touch{1000, 500};
  Keymapper km{*p, touch};
  const unsigned slot = Keymapper::kFirstTapSlot;
  // Press: a tap lands, and a timer lifts it.
  auto down = km.key(kMouseRight, true);
  ASSERT_TRUE(position(down.events, slot));
  auto t = km.take_timers();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_TRUE(km.key(kMouseRight, true).events.empty());  // a repeat doesn't tap again
  // Release before the first tap finished: queued, played after the lift and a gap.
  EXPECT_TRUE(km.key(kMouseRight, false).events.empty());
  EXPECT_TRUE(lifted(km.on_timer(t[0].token), slot));
  auto gap = km.take_timers();
  ASSERT_EQ(gap.size(), 1u);
  EXPECT_TRUE(position(km.on_timer(gap[0].token), slot));  // second tap: scope off
  auto lift = km.take_timers();
  ASSERT_EQ(lift.size(), 1u);
  EXPECT_TRUE(lifted(km.on_timer(lift[0].token), slot));
  EXPECT_TRUE(km.take_timers().empty());
  EXPECT_FALSE(touch.is_down(slot));
  // Old timers after release_all do nothing.
  km.key(kMouseRight, true);
  auto stale = km.take_timers();
  km.release_all();
  EXPECT_TRUE(km.on_timer(stale[0].token).empty());
}
