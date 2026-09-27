#pragma once

#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "core/error.h"
#include "keymap/json.h"
#include "keymap/touch.h"

namespace kestrel {

// Pseudo key codes for mouse buttons in profiles (above Linux key codes we use).
inline constexpr uint16_t kMouseLeft = 0x110;   // BTN_LEFT
inline constexpr uint16_t kMouseRight = 0x111;  // BTN_RIGHT
inline constexpr uint16_t kMouseMiddle = 0x112;  // BTN_MIDDLE
inline constexpr uint16_t kMouseBack = 0x113;    // BTN_SIDE (side button, usually "back")
inline constexpr uint16_t kMouseForward = 0x114; // BTN_EXTRA (side button, usually "forward")

// "W", "SPACE", "F1", "MOUSE_LEFT"... -> Linux code, and back.
std::optional<uint16_t> key_from_name(std::string_view name);
std::string key_name(uint16_t code);
// Name as printed on a Mac keyboard, for the UI ("Option" rather than "LALT").
std::string key_label(uint16_t code);

// A game's control layout (docs/DESIGN.md §5.7). Positions are fractions of the screen
// (0..1) so a profile works at any resolution; radii are fractions of the height.
struct KeymapProfile {
  struct Tap {
    uint16_t key;
    double x, y;
    std::string label;
    // For on/off buttons (scope): pressing the key taps the button, releasing it taps
    // again, so the button is "on" only while the key is held. Otherwise the finger
    // simply stays down while the key is held.
    bool hold_to_use = false;
  };
  struct Joystick {
    uint16_t up, left, down, right;
    double x, y, radius;
  };
  struct Aim {
    double x, y;                  // where the aiming finger lands
    double left, top, right, bottom;  // it lifts and re-lands before leaving this area
    double sensitivity;
    bool invert_y;
  };
  struct Fire {
    uint16_t button;
    double x, y;
  };

  std::string name;
  std::string layout = "Default";  // a game can have several layouts (HUDs)
  std::string package;  // Android package this profile is for
  uint16_t aim_toggle = 0;  // key that locks the mouse for aiming, e.g. F1
  uint16_t switch_key = 0;  // pressing it switches the game to this layout (0: none)
  std::vector<Tap> taps;
  std::optional<Joystick> joystick;
  std::optional<Aim> aim;
  std::optional<Fire> fire;
};

Result<KeymapProfile> parse_profile(std::string_view json);
std::string profile_to_json(const KeymapProfile& profile);

// A draggable marker in the control editor: one per control.
struct ControlMarker {
  enum class Kind { kJoystick, kAim, kFire, kTap } kind;
  size_t index;       // which tap, for kTap
  std::string label;  // shown on the marker, e.g. "WASD" or "Jump (SPACE)"
  double x, y;        // fractions of the screen
  std::string name;   // what the control does, e.g. "Jump" (taps)
  uint16_t key = 0;   // taps: its key; aim: the aim on/off key; fire: the mouse button
  std::array<uint16_t, 4> joy{};  // joystick keys: up, left, down, right
  bool hold_to_use = false;       // taps: see KeymapProfile::Tap::hold_to_use
};
std::vector<ControlMarker> profile_markers(const KeymapProfile& profile);
// Markers labelled with just the key, for the on-screen key hints. The joystick's
// label is its four keys "up left down right", separated by spaces.
std::vector<ControlMarker> hint_markers(const KeymapProfile& profile);
// Short key text for hints: "Space", "⇧", "LMB"...
std::string key_hint(uint16_t code);
// Moves controls to the markers' positions (keeping the aim area around the aim point).
void apply_markers(KeymapProfile& profile, const std::vector<ControlMarker>& markers);
// The editor's full result: positions, keys, and the list of taps (added, removed,
// rebound). Taps without a key are dropped.
void apply_edited_markers(KeymapProfile& profile, const std::vector<ControlMarker>& markers);
std::string default_free_fire_profile();  // JSON text

// Turns keys and mouse movement into touches. Every input maps to exactly one
// touch action with no repetition or screen reading (docs/DESIGN.md §4.1): holding a key
// holds a finger, releasing it lifts the finger. The only timing is the length of
// a single tap for hold-to-use buttons (a press and a release are each one tap).
class Keymapper {
 public:
  Keymapper(KeymapProfile profile, TouchScreen& touch);

  struct KeyResult {
    std::vector<InputEvent> events;
    bool consumed;  // false: not a game control, send to Android's keyboard
  };
  KeyResult key(uint16_t code, bool pressed);
  std::vector<InputEvent> mouse_button(uint16_t button, bool pressed);  // aim mode only
  std::vector<InputEvent> mouse_move(double dx, double dy);             // aim mode only

  std::vector<InputEvent> set_aim_mode(bool on);
  bool aim_mode() const { return aim_mode_; }
  uint16_t aim_toggle_key() const { return profile_.aim_toggle; }
  const KeymapProfile& profile() const { return profile_; }

  // Lifts every finger (window lost focus, profile switched).
  std::vector<InputEvent> release_all();

  // Hold-to-use taps need their finger lifted a moment after it lands (a tap
  // shorter than a game frame can go unseen). Calls above may ask for that: the
  // caller takes the requests and calls on_timer(token) after `delay_ms`.
  struct Timer {
    unsigned delay_ms;
    uint64_t token;
  };
  std::vector<Timer> take_timers() { return std::exchange(timers_, {}); }
  std::vector<InputEvent> on_timer(uint64_t token);
  static constexpr unsigned kTapMs = 40, kTapGapMs = 40;

  // Touch slots: 0 mouse pointer, 1-3 joystick/aim/fire, 4-15 held tap keys
  // (lent while a key is down, so a profile can have any number of taps),
  // 16-17 pinch, 18 scroll, 19 scripted input.
  static constexpr unsigned kJoystickSlot = 1, kAimSlot = 2, kFireSlot = 3, kFirstTapSlot = 4;
  static constexpr unsigned kTapSlots = 12;  // taps held at the same time
  static constexpr unsigned kMaxTaps = 64;   // taps in a profile
  static constexpr unsigned kPinchSlotA = 16, kPinchSlotB = 17, kScrollSlot = 18, kScriptSlot = 19;
  static constexpr unsigned kAimSlotAlt = 20;  // aiming alternates between kAimSlot and this

 private:
  std::vector<InputEvent> update_joystick();
  int32_t px(double fx) const { return static_cast<int32_t>(fx * touch_.width()); }
  int32_t py(double fy) const { return static_cast<int32_t>(fy * touch_.height()); }
  static void append(std::vector<InputEvent>& to, std::vector<InputEvent> more);

  KeymapProfile profile_;
  TouchScreen& touch_;
  bool aim_mode_ = false;
  bool joy_[4] = {};  // up, left, down, right held
  double aim_x_ = 0, aim_y_ = 0;  // current aiming finger position (pixels)
  unsigned aim_slot_ = kAimSlot;  // slot of the aiming finger (alternates on re-centre)
  int tap_owner_[kTapSlots];       // tap index holding each tap slot, or -1

  // Hold-to-use taps: each press/release queues one tap, played one after another.
  struct QuickTap {
    unsigned queued = 0;  // taps still to play
    bool busy = false;    // a tap (or the gap after it) is in progress
    int slot = -1;        // tap slot while the finger is down
    uint32_t seq = 0;     // invalidates old timers after release_all
    bool key_down = false;  // repeats and stray releases don't tap
  };
  std::vector<InputEvent> quick_tap_edge(size_t tap);
  std::vector<InputEvent> quick_tap_start(size_t tap);
  std::vector<QuickTap> quick_;
  std::vector<Timer> timers_;
};

}  // namespace kestrel
