#include "app/game_controls.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <thread>

using namespace kestrel;
using namespace kestrel::input;
namespace fs = std::filesystem;

namespace {

struct GameControlsTest : ::testing::Test {
  fs::path dir = fs::temp_directory_path() / ("kestrel-gc-" + std::to_string(::getpid()));
  TouchScreen touch{1000, 500};
  std::mutex m;
  std::vector<InputEvent> sent;
  std::vector<std::string> shown;
  bool captured = false;
  std::unique_ptr<GameControls> gc;

  void SetUp() override {
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "game.json") << R"({"name": "G", "layout": "On foot", "package": "g", "aim_toggle": "LALT",
      "switch_key": "F5", "controls": [
      {"type": "joystick", "x": 0.2, "y": 0.6, "radius": 0.1, "up": "W", "left": "A", "down": "S", "right": "D"},
      {"type": "aim", "x": 0.6, "y": 0.5, "area": [0.3, 0.1, 0.9, 0.9]},
      {"type": "tap", "key": "SPACE", "x": 0.9, "y": 0.8}]})";
    std::ofstream(dir / "game-2.json") << R"({"name": "G", "layout": "Vehicle", "package": "g", "aim_toggle": "LALT",
      "switch_key": "F6", "controls": [
      {"type": "joystick", "x": 0.3, "y": 0.7, "radius": 0.1, "up": "W", "left": "A", "down": "S", "right": "D"},
      {"type": "aim", "x": 0.6, "y": 0.5, "area": [0.3, 0.1, 0.9, 0.9]},
      {"type": "tap", "key": "MOUSE_RIGHT", "x": 0.95, "y": 0.5, "mode": "hold_to_use"}]})";
    gc = std::make_unique<GameControls>(touch, GameControls::Callbacks{
        .touch = [this](const std::vector<InputEvent>& ev) { std::lock_guard l(m); sent.insert(sent.end(), ev.begin(), ev.end()); },
        .keyboard = [](uint16_t, bool) {},
        .capture_mouse = [this](bool c) { captured = c; },
        .status = [](const std::string&) {},
        .hints = [](std::vector<ControlMarker>) {},
        .layout_shown = [this](const std::string& n) { shown.push_back(n); },
    });
    ASSERT_TRUE(gc->load_profiles(dir.string()).empty());
    gc->foreground_changed("g");
  }
  void TearDown() override {
    gc.reset();
    fs::remove_all(dir);
  }
  int32_t last_x(unsigned slot) {
    std::lock_guard l(m);
    int32_t cur = -1, x = -1;
    for (auto& e : sent) {
      if (e.type == kEvAbs && e.code == kAbsMtSlot) cur = e.value;
      if (cur == int32_t(slot) && e.type == kEvAbs && e.code == kAbsMtPositionX) x = e.value;
    }
    return x;
  }
};

}  // namespace

TEST_F(GameControlsTest, SwitchKeyKeepsAimAndMovement) {
  gc->key(56, true);  // Option: aim mode
  gc->key(56, false);
  ASSERT_TRUE(captured);
  gc->key(17, true);  // hold W
  EXPECT_EQ(last_x(Keymapper::kJoystickSlot), 200);
  gc->key(64, true);  // F6: vehicle layout
  gc->key(64, false);
  EXPECT_TRUE(captured) << "aim mode must survive the switch";
  EXPECT_EQ(last_x(Keymapper::kJoystickSlot), 300) << "held W moves the new layout's joystick";
  ASSERT_EQ(shown, std::vector<std::string>{"Vehicle"});
  EXPECT_EQ(gc->layouts().names.at(gc->layouts().active), "Vehicle");
  // Hold-to-use scope in the vehicle layout: tap now, lifted by the timer thread.
  gc->mouse_button(kMouseRight, true);
  EXPECT_TRUE(touch.is_down(Keymapper::kFirstTapSlot));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_FALSE(touch.is_down(Keymapper::kFirstTapSlot));
  gc->key(63, true);  // F5: back on foot
  gc->key(63, false);
  EXPECT_EQ(shown.back(), "On foot");
  gc->key(17, false);
  EXPECT_FALSE(touch.is_down(Keymapper::kJoystickSlot));
}

TEST_F(GameControlsTest, ExportThenImportAddsALayout) {
  const auto out = fs::temp_directory_path() / ("kestrel-gc-shared-" + std::to_string(::getpid()) + ".json");
  ASSERT_FALSE(gc->export_layout(out.string()));
  ASSERT_FALSE(gc->import_layout(out.string()));
  auto l = gc->layouts();
  ASSERT_EQ(l.names.size(), 3u);
  EXPECT_EQ(l.names[l.active], "On foot 2");  // renamed apart from the original
  std::ofstream(out) << "not json";
  EXPECT_TRUE(gc->import_layout(out.string()));
  fs::remove(out);
}

TEST_F(GameControlsTest, SwitchKeyBelongsToOneLayout) {
  ASSERT_FALSE(gc->set_switch_key(64));  // give F6 to "On foot"
  gc->key(64, true);
  gc->key(64, false);
  EXPECT_EQ(gc->layouts().names.at(gc->layouts().active), "On foot");  // F6 no longer means Vehicle
}

TEST_F(GameControlsTest, ReloadsWhenTheFileChangesOutside) {
  gc->key(57, true);  // SPACE in "On foot"
  EXPECT_TRUE(touch.is_down(Keymapper::kFirstTapSlot));
  gc->key(57, false);
  // Someone moves Jump to a new key while Kestrel runs.
  std::string text;
  {
    std::ifstream in(dir / "game.json");
    text.assign(std::istreambuf_iterator<char>(in), {});
  }
  text.replace(text.find(R"("key": "SPACE")"), 14, R"("key": "J")");
  std::ofstream(dir / "game.json") << text;
  std::this_thread::sleep_for(std::chrono::milliseconds(2600));
  gc->key(36, true);  // J now presses Jump
  EXPECT_TRUE(touch.is_down(Keymapper::kFirstTapSlot));
  gc->key(36, false);
  ASSERT_FALSE(shown.empty());
  EXPECT_EQ(shown.back(), "On foot (updated)");
}
