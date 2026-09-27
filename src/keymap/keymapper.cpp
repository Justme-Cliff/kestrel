#include "keymap/keymapper.h"

#include <map>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <utility>

namespace kestrel {
namespace {

// Names for keys a profile can use (Linux input-event-codes.h values).
constexpr std::array<std::pair<std::string_view, uint16_t>, 84> kKeyNames = {{
    {"ESC", 1},      {"1", 2},        {"2", 3},        {"3", 4},       {"4", 5},
    {"5", 6},        {"6", 7},        {"7", 8},        {"8", 9},       {"9", 10},
    {"0", 11},       {"MINUS", 12},   {"EQUAL", 13},   {"TAB", 15},    {"Q", 16},
    {"W", 17},       {"E", 18},       {"R", 19},       {"T", 20},      {"Y", 21},
    {"U", 22},       {"I", 23},       {"O", 24},       {"P", 25},      {"ENTER", 28},
    {"LCTRL", 29},   {"A", 30},       {"S", 31},       {"D", 32},      {"F", 33},
    {"G", 34},       {"H", 35},       {"J", 36},       {"K", 37},      {"L", 38},
    {"GRAVE", 41},   {"LSHIFT", 42},  {"Z", 44},       {"X", 45},      {"C", 46},
    {"V", 47},       {"B", 48},       {"N", 49},       {"M", 50},      {"LALT", 56},
    {"SPACE", 57},   {"CAPSLOCK", 58}, {"F1", 59},     {"F2", 60},     {"F3", 61},
    {"UP", 103},     {"LEFT", 105},   {"RIGHT", 106},  {"DOWN", 108},  {"RSHIFT", 54},
    {"RCTRL", 97},   {"RALT", 100},   {"MOUSE_LEFT", kMouseLeft}, {"MOUSE_RIGHT", kMouseRight},
    {"BACKSPACE", 14},
    {"F4", 62},      {"F5", 63},      {"F6", 64},      {"F7", 65},     {"F8", 66},
    {"F9", 67},      {"F10", 68},     {"F11", 87},     {"F12", 88},    {"LEFTBRACE", 26},
    {"RIGHTBRACE", 27}, {"SEMICOLON", 39}, {"APOSTROPHE", 40}, {"BACKSLASH", 43}, {"COMMA", 51},
    {"DOT", 52},     {"SLASH", 53},   {"HOME", 102},   {"END", 107},   {"PAGEUP", 104},
    {"PAGEDOWN", 109}, {"MOUSE_MIDDLE", kMouseMiddle}, {"MOUSE_BACK", kMouseBack}, {"MOUSE_FORWARD", kMouseForward},
}};

double num(const Json& j, std::string_view key, double fallback) {
  return j[key].number().value_or(fallback);
}

Result<uint16_t> key_field(const Json& j, std::string_view field) {
  auto name = j[field].string();
  if (!name) return make_error(std::format("control is missing \"{}\"", field));
  auto code = key_from_name(*name);
  if (!code) return make_error(std::format("unknown key \"{}\"", *name));
  return *code;
}

bool in_unit(double v) { return v >= 0.0 && v <= 1.0; }

}  // namespace

std::optional<uint16_t> key_from_name(std::string_view name) {
  for (auto [n, code] : kKeyNames) {
    if (n == name) return code;
  }
  return std::nullopt;
}

std::string key_name(uint16_t code) {
  for (auto [n, c] : kKeyNames) {
    if (c == code) return std::string(n);
  }
  return std::format("KEY_{}", code);
}

Result<KeymapProfile> parse_profile(std::string_view text) {
  auto json = Json::parse(text);
  if (!json) return std::unexpected(json.error());
  const Json& j = *json;
  KeymapProfile p;
  p.name = j["name"].string().value_or("Untitled");
  p.layout = j["layout"].string().value_or("Default");
  p.package = j["package"].string().value_or("");
  if (auto t = j["aim_toggle"].string()) {
    auto code = key_from_name(*t);
    if (!code) return make_error("unknown aim_toggle key " + *t);
    p.aim_toggle = *code;
  }
  if (auto t = j["switch_key"].string()) {
    auto code = key_from_name(*t);
    if (!code) return make_error("unknown switch_key " + *t);
    p.switch_key = *code;
  }
  const Json::Array* controls = j["controls"].array();
  if (!controls) return make_error("profile has no \"controls\" list");
  for (const Json& c : *controls) {
    const std::string type = c["type"].string().value_or("");
    const double x = num(c, "x", -1), y = num(c, "y", -1);
    if (!in_unit(x) || !in_unit(y)) return make_error(std::format("{} control needs x and y in 0..1", type));
    if (type == "tap") {
      auto key = key_field(c, "key");
      if (!key) return std::unexpected(key.error());
      p.taps.push_back({*key, x, y, c["label"].string().value_or(""), c["mode"].string().value_or("") == "hold_to_use"});
    } else if (type == "joystick") {
      KeymapProfile::Joystick js{};
      for (auto [field, dst] : {std::pair{"up", &js.up}, {"left", &js.left}, {"down", &js.down}, {"right", &js.right}}) {
        auto key = key_field(c, field);
        if (!key) return std::unexpected(key.error());
        *dst = *key;
      }
      js.x = x;
      js.y = y;
      js.radius = num(c, "radius", 0.1);
      p.joystick = js;
    } else if (type == "aim") {
      const Json::Array* area = c["area"].array();
      KeymapProfile::Aim aim{x, y, 0.3, 0.05, 0.98, 0.85, num(c, "sensitivity", 1.0),
                             c["invert_y"].boolean().value_or(false)};
      if (area && area->size() == 4) {
        aim.left = (*area)[0].number().value_or(aim.left);
        aim.top = (*area)[1].number().value_or(aim.top);
        aim.right = (*area)[2].number().value_or(aim.right);
        aim.bottom = (*area)[3].number().value_or(aim.bottom);
      }
      if (!(aim.left < x && x < aim.right && aim.top < y && y < aim.bottom)) {
        return make_error("aim point must be inside its area");
      }
      p.aim = aim;
    } else if (type == "fire") {
      auto button = c["button"].string() ? key_field(c, "button") : Result<uint16_t>(kMouseLeft);
      if (!button) return std::unexpected(button.error());
      p.fire = KeymapProfile::Fire{*button, x, y};
    } else {
      return make_error("unknown control type \"" + type + "\"");
    }
  }
  if (p.taps.size() > Keymapper::kMaxTaps) {
    return make_error("too many tap controls");
  }
  return p;
}

std::string key_label(uint16_t code) {
  const std::string name = key_name(code);
  if (name == "LALT" || name == "RALT") return "Option";
  if (name == "LCTRL" || name == "RCTRL") return "Control";
  if (name == "LSHIFT" || name == "RSHIFT") return "Shift";
  return name;
}

std::string default_free_fire_profile() {
  // A starting layout for Free Fire in landscape. Button positions depend on the
  // in-game HUD settings: move them in the profile to match yours.
  return R"({
  "name": "Free Fire",
  "package": "com.dts.freefireth",
  "aim_toggle": "LALT",
  "controls": [
    {"type": "joystick", "x": 0.170, "y": 0.433, "radius": 0.112, "up": "W", "left": "A", "down": "S", "right": "D"},
    {"type": "aim", "x": 0.62, "y": 0.46, "area": [0.30, 0.03, 0.99, 0.97], "sensitivity": 1.0},
    {"type": "fire", "button": "MOUSE_LEFT", "x": 0.847, "y": 0.732},
    {"type": "tap", "key": "MOUSE_RIGHT", "x": 0.959, "y": 0.532, "label": "Scope"},
    {"type": "tap", "key": "SPACE", "x": 0.958, "y": 0.692, "label": "Jump"},
    {"type": "tap", "key": "C", "x": 0.938, "y": 0.880, "label": "Crouch"},
    {"type": "tap", "key": "Z", "x": 0.844, "y": 0.931, "label": "Prone"},
    {"type": "tap", "key": "LSHIFT", "x": 0.171, "y": 0.432, "label": "Sprint"},
    {"type": "tap", "key": "R", "x": 0.899, "y": 0.079, "label": "Reload"},
    {"type": "tap", "key": "F", "x": 0.715, "y": 0.493, "label": "Pick up"},
    {"type": "tap", "key": "H", "x": 0.715, "y": 0.666, "label": "Heal"},
    {"type": "tap", "key": "G", "x": 0.031, "y": 0.738, "label": "Gloo wall"},
    {"type": "tap", "key": "X", "x": 0.028, "y": 0.808, "label": "Switch throwable"},
    {"type": "tap", "key": "1", "x": 0.841, "y": 0.204, "label": "Weapon 1"},
    {"type": "tap", "key": "2", "x": 0.911, "y": 0.204, "label": "Weapon 2"},
    {"type": "tap", "key": "3", "x": 0.969, "y": 0.204, "label": "Melee"},
    {"type": "tap", "key": "4", "x": 0.770, "y": 0.412, "label": "Item 1"},
    {"type": "tap", "key": "5", "x": 0.828, "y": 0.412, "label": "Item 2"},
    {"type": "tap", "key": "6", "x": 0.896, "y": 0.412, "label": "Item 3"},
    {"type": "tap", "key": "7", "x": 0.965, "y": 0.412, "label": "Item 4"},
    {"type": "tap", "key": "Q", "x": 0.667, "y": 0.819, "label": "Extra 1"},
    {"type": "tap", "key": "E", "x": 0.738, "y": 0.819, "label": "Extra 2"},
    {"type": "tap", "key": "TAB", "x": 0.138, "y": 0.926, "label": "Bag"},
    {"type": "tap", "key": "B", "x": 0.270, "y": 0.183, "label": "Emote"},
    {"type": "tap", "key": "T", "x": 0.031, "y": 0.736, "label": "Grenade"},
    {"type": "tap", "key": "V", "x": 0.715, "y": 0.665, "label": "Heart +"},
    {"type": "tap", "key": "Y", "x": 0.339, "y": 0.527, "label": "Flag"},
    {"type": "tap", "key": "U", "x": 0.24, "y": 0.431, "label": "Knife"},
    {"type": "tap", "key": "P", "x": 0.226, "y": 0.112, "label": "Pet"},
    {"type": "tap", "key": "I", "x": 0.185, "y": 0.112, "label": "Mic"},
    {"type": "tap", "key": "J", "x": 0.185, "y": 0.183, "label": "Chat"},
    {"type": "tap", "key": "K", "x": 0.226, "y": 0.183, "label": "Camera"},
    {"type": "tap", "key": "L", "x": 0.229, "y": 0.264, "label": "Quick message"},
    {"type": "tap", "key": "N", "x": 0.763, "y": 0.069, "label": "Fire mode"},
    {"type": "tap", "key": "8", "x": 0.731, "y": 0.15, "label": "Door / vehicle"},
    {"type": "tap", "key": "9", "x": 0.669, "y": 0.355, "label": "Mark"},
    {"type": "tap", "key": "0", "x": 0.731, "y": 0.736, "label": "Loot box"},
    {"type": "tap", "key": "M", "x": 0.076, "y": 0.132, "label": "Map"}
  ]
}
)";
}

std::string profile_to_json(const KeymapProfile& p) {
  auto point = [](Json::Object o, double x, double y) {
    o["x"] = Json(x);
    o["y"] = Json(y);
    return o;
  };
  Json::Array controls;
  if (p.joystick) {
    const auto& j = *p.joystick;
    controls.push_back(Json(point({{"type", Json(std::string("joystick"))}, {"radius", Json(j.radius)},
                                   {"up", Json(key_name(j.up))}, {"left", Json(key_name(j.left))},
                                   {"down", Json(key_name(j.down))}, {"right", Json(key_name(j.right))}},
                                  j.x, j.y)));
  }
  if (p.aim) {
    const auto& a = *p.aim;
    controls.push_back(Json(point({{"type", Json(std::string("aim"))},
                                   {"area", Json(Json::Array{Json(a.left), Json(a.top), Json(a.right), Json(a.bottom)})},
                                   {"sensitivity", Json(a.sensitivity)}, {"invert_y", Json(a.invert_y)}},
                                  a.x, a.y)));
  }
  if (p.fire) {
    controls.push_back(Json(point({{"type", Json(std::string("fire"))}, {"button", Json(key_name(p.fire->button))}},
                                  p.fire->x, p.fire->y)));
  }
  for (const auto& t : p.taps) {
    Json::Object o{{"type", Json(std::string("tap"))}, {"key", Json(key_name(t.key))}, {"label", Json(t.label)}};
    if (t.hold_to_use) o["mode"] = Json(std::string("hold_to_use"));
    controls.push_back(Json(point(std::move(o), t.x, t.y)));
  }
  Json::Object root{{"name", Json(p.name)}, {"layout", Json(p.layout)}, {"package", Json(p.package)},
                    {"controls", Json(std::move(controls))}};
  if (p.aim_toggle) root["aim_toggle"] = Json(key_name(p.aim_toggle));
  if (p.switch_key) root["switch_key"] = Json(key_name(p.switch_key));
  return Json(std::move(root)).dump() + "\n";
}

std::vector<ControlMarker> profile_markers(const KeymapProfile& p) {
  using K = ControlMarker::Kind;
  std::vector<ControlMarker> m;
  if (p.joystick) {
    const auto& j = *p.joystick;
    m.push_back({K::kJoystick, 0, key_name(j.up) + key_name(j.left) + key_name(j.down) + key_name(j.right), j.x, j.y,
                 "Move", 0, {j.up, j.left, j.down, j.right}});
  }
  if (p.aim) m.push_back({K::kAim, 0, "Aim (" + key_name(p.aim_toggle) + ")", p.aim->x, p.aim->y, "Aim", p.aim_toggle, {}});
  if (p.fire) m.push_back({K::kFire, 0, "Fire (click)", p.fire->x, p.fire->y, "Fire", p.fire->button, {}});
  for (size_t i = 0; i < p.taps.size(); ++i) {
    const auto& t = p.taps[i];
    std::string key = key_name(t.key);
    if (key == "MOUSE_RIGHT") key = "right click";
    m.push_back({K::kTap, i, t.label.empty() ? key : t.label + " (" + key + ")", t.x, t.y, t.label, t.key, {},
                 t.hold_to_use});
  }
  return m;
}

std::string key_hint(uint16_t code) {
  static const std::map<std::string, std::string> pretty = {
      {"SPACE", "Space"}, {"LSHIFT", "⇧"},     {"RSHIFT", "⇧"},   {"LALT", "⌥"},     {"RALT", "⌥"},
      {"LCTRL", "⌃"},     {"RCTRL", "⌃"},      {"TAB", "Tab"},    {"ESC", "Esc"},    {"ENTER", "↩"},
      {"BACKSPACE", "⌫"}, {"CAPSLOCK", "⇪"},   {"GRAVE", "`"},    {"MINUS", "-"},    {"EQUAL", "="},
      {"UP", "↑"},        {"DOWN", "↓"},       {"LEFT", "←"},     {"RIGHT", "→"},
      {"MOUSE_LEFT", "LMB"}, {"MOUSE_RIGHT", "RMB"}, {"MOUSE_MIDDLE", "MMB"}, {"MOUSE_BACK", "M4"},
      {"MOUSE_FORWARD", "M5"}, {"LEFTBRACE", "["}, {"RIGHTBRACE", "]"}, {"SEMICOLON", ";"}, {"APOSTROPHE", "'"},
      {"BACKSLASH", "\\"}, {"COMMA", ","}, {"DOT", "."}, {"SLASH", "/"}, {"PAGEUP", "PgUp"}, {"PAGEDOWN", "PgDn"}};
  const std::string name = key_name(code);
  auto it = pretty.find(name);
  return it != pretty.end() ? it->second : name;
}

std::vector<ControlMarker> hint_markers(const KeymapProfile& p) {
  using K = ControlMarker::Kind;
  std::vector<ControlMarker> m;
  if (p.joystick) {
    const auto& j = *p.joystick;
    m.push_back({K::kJoystick, 0,
                 key_hint(j.up) + " " + key_hint(j.left) + " " + key_hint(j.down) + " " + key_hint(j.right), j.x,
                 j.y, "Move", 0, {j.up, j.left, j.down, j.right}});
  }
  if (p.aim && p.aim_toggle) {
    m.push_back({K::kAim, 0, key_hint(p.aim_toggle) + " Aim", p.aim->x, p.aim->y, "Aim", p.aim_toggle, {}});
  }
  if (p.fire) m.push_back({K::kFire, 0, key_hint(p.fire->button), p.fire->x, p.fire->y, "Fire", p.fire->button, {}});
  for (size_t i = 0; i < p.taps.size(); ++i) {
    const auto& t = p.taps[i];
    m.push_back({K::kTap, i, key_hint(t.key), t.x, t.y, t.label, t.key, {}});
  }
  return m;
}

void apply_edited_markers(KeymapProfile& p, const std::vector<ControlMarker>& markers) {
  using K = ControlMarker::Kind;
  apply_markers(p, markers);  // positions of joystick, aim, fire
  std::vector<KeymapProfile::Tap> taps;
  for (const auto& mk : markers) {
    switch (mk.kind) {
      case K::kJoystick:
        if (p.joystick && std::ranges::all_of(mk.joy, [](uint16_t k) { return k != 0; })) {
          p.joystick->up = mk.joy[0], p.joystick->left = mk.joy[1];
          p.joystick->down = mk.joy[2], p.joystick->right = mk.joy[3];
        }
        break;
      case K::kAim:
        if (mk.key) p.aim_toggle = mk.key;
        break;
      case K::kFire:
        break;
      case K::kTap:
        if (mk.key) {
          taps.push_back({mk.key, std::clamp(mk.x, 0.0, 1.0), std::clamp(mk.y, 0.0, 1.0), mk.name, mk.hold_to_use});
        }
        break;
    }
  }
  if (taps.size() > Keymapper::kMaxTaps) taps.resize(Keymapper::kMaxTaps);
  p.taps = std::move(taps);
}

void apply_markers(KeymapProfile& p, const std::vector<ControlMarker>& markers) {
  using K = ControlMarker::Kind;
  for (const auto& mk : markers) {
    const double x = std::clamp(mk.x, 0.0, 1.0), y = std::clamp(mk.y, 0.0, 1.0);
    switch (mk.kind) {
      case K::kJoystick:
        if (p.joystick) p.joystick->x = x, p.joystick->y = y;
        break;
      case K::kAim:
        if (p.aim) {
          // Keep the aim point strictly inside its area.
          p.aim->x = std::clamp(x, p.aim->left + 0.01, p.aim->right - 0.01);
          p.aim->y = std::clamp(y, p.aim->top + 0.01, p.aim->bottom - 0.01);
        }
        break;
      case K::kFire:
        if (p.fire) p.fire->x = x, p.fire->y = y;
        break;
      case K::kTap:
        if (mk.index < p.taps.size()) p.taps[mk.index].x = x, p.taps[mk.index].y = y;
        break;
    }
  }
}

Keymapper::Keymapper(KeymapProfile profile, TouchScreen& touch)
    : profile_(std::move(profile)), touch_(touch) {
  for (int& owner : tap_owner_) owner = -1;
  quick_.resize(profile_.taps.size());
}

std::vector<InputEvent> Keymapper::quick_tap_edge(size_t tap) {
  QuickTap& q = quick_[tap];
  if (q.queued >= 2) return {};  // a press and a release already waiting: enough to stay in sync
  ++q.queued;
  return q.busy ? std::vector<InputEvent>{} : quick_tap_start(tap);
}

std::vector<InputEvent> Keymapper::quick_tap_start(size_t tap) {
  QuickTap& q = quick_[tap];
  int free = -1;
  for (int s = 0; s < static_cast<int>(kTapSlots); ++s) {
    if (tap_owner_[s] < 0) {
      free = s;
      break;
    }
  }
  if (free < 0) {  // every finger busy: drop the tap
    q.queued = 0;
    q.busy = false;
    q.slot = -1;
    return {};
  }
  --q.queued;
  q.busy = true;
  q.slot = free;
  tap_owner_[free] = static_cast<int>(tap);
  timers_.push_back({kTapMs, (uint64_t(tap) << 33) | (uint64_t(q.seq) << 1) | 0});  // bit 0: 0 lift, 1 next tap
  const auto& t = profile_.taps[tap];
  return touch_.down(kFirstTapSlot + static_cast<unsigned>(free), px(t.x), py(t.y));
}

std::vector<InputEvent> Keymapper::on_timer(uint64_t token) {
  const size_t tap = token >> 33;
  const uint32_t seq = static_cast<uint32_t>((token >> 1) & 0xffffffff);
  if (tap >= quick_.size() || quick_[tap].seq != seq || !quick_[tap].busy) return {};  // stale
  QuickTap& q = quick_[tap];
  if ((token & 1) == 0) {  // lift the finger
    std::vector<InputEvent> out;
    if (q.slot >= 0) {
      tap_owner_[q.slot] = -1;
      out = touch_.up(kFirstTapSlot + static_cast<unsigned>(q.slot));
      q.slot = -1;
    }
    if (q.queued > 0) {
      timers_.push_back({kTapGapMs, (uint64_t(tap) << 33) | (uint64_t(q.seq) << 1) | 1});
    } else {
      q.busy = false;
    }
    return out;
  }
  return quick_tap_start(tap);  // the next queued tap
}

void Keymapper::append(std::vector<InputEvent>& to, std::vector<InputEvent> more) {
  to.insert(to.end(), more.begin(), more.end());
}

std::vector<InputEvent> Keymapper::update_joystick() {
  const auto& js = *profile_.joystick;
  double dx = (joy_[3] ? 1 : 0) - (joy_[1] ? 1 : 0);
  double dy = (joy_[2] ? 1 : 0) - (joy_[0] ? 1 : 0);
  if (dx == 0 && dy == 0) return touch_.up(kJoystickSlot);
  const double len = std::hypot(dx, dy);  // diagonals push as far as straight moves
  // Push to 60% of the ring: full running speed, well inside it. Free Fire (like many
  // shooters) locks sprinting when the finger slides up past the ring, and then the
  // character keeps running after the keys are released.
  constexpr double kPush = 0.60;
  const double r = js.radius * kPush * touch_.height();
  const int32_t cx = px(js.x), cy = py(js.y);
  std::vector<InputEvent> out;
  // The finger must land on the joystick's center before it can push it.
  if (!touch_.is_down(kJoystickSlot)) append(out, touch_.down(kJoystickSlot, cx, cy));
  append(out, touch_.move(kJoystickSlot, cx + static_cast<int32_t>(dx / len * r),
                          cy + static_cast<int32_t>(dy / len * r)));
  return out;
}

Keymapper::KeyResult Keymapper::key(uint16_t code, bool pressed) {
  if (profile_.aim_toggle && code == profile_.aim_toggle) {
    return {pressed ? set_aim_mode(!aim_mode_) : std::vector<InputEvent>{}, true};
  }
  if (profile_.joystick) {
    const auto& js = *profile_.joystick;
    const uint16_t dirs[4] = {js.up, js.left, js.down, js.right};
    for (int i = 0; i < 4; ++i) {
      if (code == dirs[i]) {
        joy_[i] = pressed;
        return {update_joystick(), true};
      }
    }
  }
  for (size_t i = 0; i < profile_.taps.size(); ++i) {
    const auto& t = profile_.taps[i];
    if (t.key != code) continue;
    if (t.hold_to_use) {
      if (quick_[i].key_down == pressed) return {{}, true};
      quick_[i].key_down = pressed;
      return {quick_tap_edge(i), true};
    }
    const int tap = static_cast<int>(i);
    int held = -1, free = -1;
    for (int s = 0; s < static_cast<int>(kTapSlots); ++s) {
      if (tap_owner_[s] == tap) held = s;
      if (tap_owner_[s] < 0 && free < 0) free = s;
    }
    if (pressed) {
      if (held >= 0 || free < 0) return {{}, true};  // already down, or all fingers busy
      tap_owner_[free] = tap;
      return {touch_.down(kFirstTapSlot + static_cast<unsigned>(free), px(t.x), py(t.y)), true};
    }
    if (held < 0) return {{}, true};
    tap_owner_[held] = -1;
    return {touch_.up(kFirstTapSlot + static_cast<unsigned>(held)), true};
  }
  return {{}, false};
}

std::vector<InputEvent> Keymapper::mouse_button(uint16_t button, bool pressed) {
  if (!aim_mode_) return {};
  if (profile_.fire && profile_.fire->button == button) {
    return pressed ? touch_.down(kFireSlot, px(profile_.fire->x), py(profile_.fire->y))
                   : touch_.up(kFireSlot);
  }
  return key(button, pressed).events;  // e.g. right button bound as a tap (scope)
}

std::vector<InputEvent> Keymapper::mouse_move(double dx, double dy) {
  if (!aim_mode_ || !profile_.aim) return {};
  const auto& aim = *profile_.aim;
  std::vector<InputEvent> out;
  if (!touch_.is_down(aim_slot_)) {
    aim_x_ = px(aim.x);
    aim_y_ = py(aim.y);
    append(out, touch_.down(aim_slot_, static_cast<int32_t>(aim_x_), static_cast<int32_t>(aim_y_)));
  }
  aim_x_ += dx * aim.sensitivity;
  aim_y_ += (aim.invert_y ? -dy : dy) * aim.sensitivity;
  // Near the edge of the aiming area the finger hands over to a new one at the start
  // point, so the camera can keep turning without running out of screen. Both happen
  // in one input frame: games that track touches by list position (Unity) would
  // otherwise briefly read the joystick finger as the camera one and jerk the stick.
  if (aim_x_ < px(aim.left) || aim_x_ > px(aim.right) || aim_y_ < py(aim.top) || aim_y_ > py(aim.bottom)) {
    const unsigned next = aim_slot_ == kAimSlot ? kAimSlotAlt : kAimSlot;
    aim_x_ = px(aim.x);
    aim_y_ = py(aim.y);
    append(out, touch_.handover(aim_slot_, next, static_cast<int32_t>(aim_x_), static_cast<int32_t>(aim_y_)));
    aim_slot_ = next;
    return out;
  }
  append(out, touch_.move(aim_slot_, static_cast<int32_t>(aim_x_), static_cast<int32_t>(aim_y_)));
  return out;
}

std::vector<InputEvent> Keymapper::set_aim_mode(bool on) {
  aim_mode_ = on && profile_.aim.has_value();
  if (aim_mode_) return {};
  std::vector<InputEvent> out = touch_.up(kAimSlot);
  append(out, touch_.up(kAimSlotAlt));
  append(out, touch_.up(kFireSlot));
  aim_slot_ = kAimSlot;
  return out;
}

std::vector<InputEvent> Keymapper::release_all() {
  std::vector<InputEvent> out;
  for (unsigned slot = kJoystickSlot; slot < kFirstTapSlot + kTapSlots; ++slot) append(out, touch_.up(slot));
  append(out, touch_.up(kAimSlotAlt));
  aim_slot_ = kAimSlot;
  for (int& owner : tap_owner_) owner = -1;
  for (auto& q : quick_) q = QuickTap{0, false, -1, q.seq + 1, false};  // pending timers become stale
  for (bool& j : joy_) j = false;
  aim_mode_ = false;
  return out;
}

}  // namespace kestrel
