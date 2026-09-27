#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "keymap/keymapper.h"

namespace kestrel {

// Owns the game profiles and the active Keymapper. Controls switch on when the
// app in front has a profile (and the user hasn't turned them off). Thread-safe.
class GameControls {
 public:
  struct Callbacks {
    std::function<void(const std::vector<InputEvent>&)> touch;  // to the touchscreen
    std::function<void(uint16_t key, bool pressed)> keyboard;   // unmapped keys
    std::function<void(bool)> capture_mouse;                    // aim mode on/off
    std::function<void(const std::string&)> status;             // title-bar hint
    std::function<void(std::vector<ControlMarker>)> hints;      // key hints to show (empty: none)
    std::function<void(const std::string&)> layout_shown;       // a layout was switched to by key
  };

  GameControls(TouchScreen& touch, Callbacks callbacks);
  ~GameControls();

  // Loads every *.json profile in `dir`, writing the default Free Fire one first
  // if it's missing. Returns the problems found (bad profiles are skipped).
  std::vector<std::string> load_profiles(const std::string& dir);
  std::string profile_path(const std::string& package) const;
  std::string edit_package_locked() const;

  // For the editor: the controls of the app in front (or Free Fire's), and saving them.
  std::pair<std::string, std::vector<ControlMarker>> editable_markers() const;
  bool has_profile(const std::string& package) const;

  // Layouts: a game can have several (one file each); one is in use.
  struct Layouts {
    std::vector<std::string> names;
    std::vector<uint16_t> keys;  // each layout's switch key (0: none)
    size_t active = 0;
  };
  Layouts layouts() const;  // of the game being edited
  std::optional<std::string> select_layout(size_t index);
  std::optional<std::string> new_layout(const std::string& name, const std::vector<ControlMarker>& markers);
  std::optional<std::string> rename_layout(const std::string& name);
  std::optional<std::string> delete_layout();
  std::optional<std::string> save_markers(const std::vector<ControlMarker>& markers);  // error text
  // The key that switches to the layout being edited (0: none).
  uint16_t switch_key() const;
  std::optional<std::string> set_switch_key(uint16_t key);
  // Sharing layouts: the one being edited to a file, or a file added as a new layout
  // (and made active). Errors as text.
  std::optional<std::string> export_layout(const std::string& path) const;
  std::optional<std::string> import_layout(const std::string& path);

  void foreground_changed(const std::string& package);
  void toggle_enabled();
  void set_sensitivity(double factor);  // multiplies every profile's aim sensitivity

  void key(uint16_t code, bool pressed);
  void mouse_button(uint16_t button, bool pressed);
  void mouse_delta(double dx, double dy);
  void focus_lost();
  // A text field has the keyboard: keys go to Android as typing, not to the game.
  void set_text_input(bool typing);

 private:
  void activate_locked(const std::string& package);
  std::vector<std::string> load_locked();
  void save_active_choice_locked();
  void sync_capture_locked();
  void drain_timers_locked();
  bool switch_layout_locked(uint16_t key);
  void timer_loop();
  std::string files_signature_locked() const;  // names, sizes and times of the profile files

  Callbacks cb_;
  TouchScreen& touch_;
  mutable std::mutex mutex_;
  std::string dir_;
  std::map<std::string, KeymapProfile> profiles_;  // by package
  std::map<std::string, std::string> paths_;       // package -> file of the layout in use
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> layouts_;  // package -> (file, name)
  std::map<std::string, std::string> active_file_;  // package -> chosen layout file name (layouts.json)
  std::map<std::string, KeymapProfile> all_layouts_;  // every layout file (path) -> profile
  std::optional<Keymapper> active_;
  std::string foreground_;
  bool enabled_ = true;
  bool captured_ = false;
  bool typing_ = false;
  double sensitivity_ = 1.0;
  std::string files_signature_;     // as of our last load or write
  std::set<uint16_t> held_keys_;    // keys down right now (layout switches keep them held)
  std::set<uint16_t> switch_down_;  // layout-switch keys down: their release is ours too

  // Finger lifts for hold-to-use taps, run on their own thread.
  struct PendingTimer {
    std::chrono::steady_clock::time_point at;
    uint64_t token, generation;
  };
  std::vector<PendingTimer> timers_;
  uint64_t generation_ = 0;  // a new Keymapper makes older timers stale
  bool quit_ = false;
  std::condition_variable timer_cv_;
  std::thread timer_thread_;
};

}  // namespace kestrel
