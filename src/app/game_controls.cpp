#include "app/game_controls.h"
#include "keymap/json.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace kestrel {
namespace fs = std::filesystem;

GameControls::GameControls(TouchScreen& touch, Callbacks callbacks)
    : cb_(std::move(callbacks)), touch_(touch), timer_thread_([this] { timer_loop(); }) {}

GameControls::~GameControls() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  timer_cv_.notify_all();
  timer_thread_.join();
}

void GameControls::timer_loop() {
  std::unique_lock lock(mutex_);
  auto next_file_check = std::chrono::steady_clock::now();
  while (!quit_) {
    // Controls files edited outside Kestrel (or a shared layout dropped in) apply by
    // themselves: no restart, no toggling.
    if (std::chrono::steady_clock::now() >= next_file_check) {
      next_file_check = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      if (!dir_.empty() && files_signature_locked() != files_signature_) {
        const bool aiming = active_ && active_->aim_mode();
        load_locked();
        if (active_ && aiming) {
          if (auto ev = active_->set_aim_mode(true); !ev.empty()) cb_.touch(ev);
          sync_capture_locked();
        }
        if (cb_.layout_shown && profiles_.contains(foreground_)) cb_.layout_shown(profiles_.at(foreground_).layout + " (updated)");
      }
    }
    auto wake = next_file_check;
    if (!timers_.empty()) {
      auto next = std::min_element(timers_.begin(), timers_.end(), [](auto& a, auto& b) { return a.at < b.at; });
      wake = std::min(wake, next->at);
    }
    if (timer_cv_.wait_until(lock, wake) != std::cv_status::timeout) continue;  // new timer or quit: recheck
    if (timers_.empty()) continue;
    const auto now = std::chrono::steady_clock::now();
    std::vector<PendingTimer> due;
    std::erase_if(timers_, [&](const PendingTimer& t) {
      if (t.at > now) return false;
      due.push_back(t);
      return true;
    });
    for (const auto& t : due) {
      if (!active_ || t.generation != generation_) continue;  // controls changed since
      auto ev = active_->on_timer(t.token);
      if (!ev.empty()) cb_.touch(ev);
    }
    drain_timers_locked();
  }
}

std::string GameControls::files_signature_locked() const {
  std::string sig;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir_, ec)) {
    if (e.path().extension() != ".json") continue;
    std::error_code e1, e2;
    sig += e.path().filename().string() + ":" + std::to_string(e.file_size(e1)) + ":" +
           std::to_string(static_cast<long long>(e.last_write_time(e2).time_since_epoch().count())) + ";";
  }
  return sig;
}

void GameControls::drain_timers_locked() {
  if (!active_) return;
  const auto now = std::chrono::steady_clock::now();
  bool added = false;
  for (const auto& t : active_->take_timers()) {
    timers_.push_back({now + std::chrono::milliseconds(t.delay_ms), t.token, generation_});
    added = true;
  }
  if (added) timer_cv_.notify_all();
}

std::vector<std::string> GameControls::load_profiles(const std::string& dir) {
  std::vector<std::string> problems;
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path ff = fs::path(dir) / "freefire.json";
  if (!fs::exists(ff)) {
    std::ofstream(ff) << default_free_fire_profile();
  } else {
    // Old default: F1 is screen brightness on a Mac keyboard. Move it to Option.
    std::ifstream in(ff);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::string old_toggle = R"("aim_toggle": "F1")";
    if (auto pos = text.find(old_toggle); pos != std::string::npos) {
      text.replace(pos, old_toggle.size(), R"("aim_toggle": "LALT")");
      std::ofstream(ff) << text;
    }
  }

  std::lock_guard lock(mutex_);
  dir_ = dir;
  // Which layout each game uses.
  active_file_.clear();
  if (std::ifstream in(fs::path(dir) / "layouts.json"); in) {
    std::stringstream text;
    text << in.rdbuf();
    if (auto j = Json::parse(text.str()); j && j->object()) {
      for (const auto& [package, file] : *j->object()) {
        if (auto f = file.string()) active_file_[package] = *f;
      }
    }
  }
  auto found = load_locked();
  problems.insert(problems.end(), found.begin(), found.end());
  return problems;
}

std::vector<std::string> GameControls::load_locked() {
  std::vector<std::string> problems;
  std::error_code ec;
  profiles_.clear();
  paths_.clear();
  layouts_.clear();
  all_layouts_.clear();
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(dir_, ec)) {
    if (entry.path().extension() == ".json" && entry.path().filename() != "layouts.json") files.push_back(entry.path());
  }
  // By name without ".json": "freefire" comes before "freefire-2", so a game's first
  // layout is the default until the player picks another.
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return a.stem() < b.stem(); });
  for (const auto& path : files) {
    const fs::directory_entry entry(path);
    if (entry.path().extension() != ".json") continue;
    std::ifstream in(entry.path());
    std::stringstream text;
    text << in.rdbuf();
    auto p = parse_profile(text.str());
    if (!p) {
      problems.push_back(entry.path().filename().string() + ": " + p.error().message);
      continue;
    }
    if (p->package.empty()) {
      problems.push_back(entry.path().filename().string() + ": missing \"package\"");
      continue;
    }
    layouts_[p->package].emplace_back(entry.path().string(), p->layout);
    all_layouts_[entry.path().string()] = *p;
    // The chosen layout, else the first file found for the game.
    const auto chosen = active_file_.find(p->package);
    const bool is_chosen = chosen != active_file_.end() && chosen->second == entry.path().filename().string();
    if (!profiles_.contains(p->package) || is_chosen) {
      paths_[p->package] = entry.path().string();
      profiles_[p->package] = std::move(*p);
    }
  }
  activate_locked(foreground_);
  files_signature_ = files_signature_locked();  // our own view of the files: not a change
  return problems;
}

void GameControls::save_active_choice_locked() {
  Json::Object o;
  for (const auto& [package, path] : paths_) o[package] = Json(fs::path(path).filename().string());
  std::ofstream(fs::path(dir_) / "layouts.json", std::ios::trunc) << Json(std::move(o)).dump() << "\n";
  files_signature_ = files_signature_locked();
}

GameControls::Layouts GameControls::layouts() const {
  std::lock_guard lock(mutex_);
  Layouts l;
  const std::string package = edit_package_locked();
  auto it = layouts_.find(package);
  if (it == layouts_.end()) return l;
  for (size_t i = 0; i < it->second.size(); ++i) {
    l.names.push_back(it->second[i].second);
    auto profile = all_layouts_.find(it->second[i].first);
    l.keys.push_back(profile == all_layouts_.end() ? 0 : profile->second.switch_key);
    if (paths_.contains(package) && it->second[i].first == paths_.at(package)) l.active = i;
  }
  return l;
}

std::optional<std::string> GameControls::select_layout(size_t index) {
  std::lock_guard lock(mutex_);
  const std::string package = edit_package_locked();
  auto it = layouts_.find(package);
  if (it == layouts_.end() || index >= it->second.size()) return "no such layout";
  active_file_[package] = fs::path(it->second[index].first).filename().string();
  paths_[package] = it->second[index].first;
  save_active_choice_locked();
  load_locked();
  return std::nullopt;
}

std::optional<std::string> GameControls::new_layout(const std::string& name, const std::vector<ControlMarker>& markers) {
  std::lock_guard lock(mutex_);
  const std::string package = edit_package_locked();
  auto it = profiles_.find(package);
  if (it == profiles_.end()) return "no game controls to copy";
  KeymapProfile copy = it->second;
  apply_edited_markers(copy, markers);
  copy.layout = name.empty() ? "Layout" : name;
  const std::string stem = fs::path(paths_[package]).stem().string();
  fs::path file;
  for (int n = 2; n < 1000; ++n) {
    file = fs::path(dir_) / std::format("{}-{}.json", stem.substr(0, stem.find('-')), n);
    if (!fs::exists(file)) break;
  }
  std::ofstream out(file, std::ios::trunc);
  out << profile_to_json(copy);
  if (!out) return "couldn't save " + file.string();
  out.close();
  active_file_[package] = file.filename().string();
  paths_[package] = file.string();
  save_active_choice_locked();
  load_locked();
  return std::nullopt;
}

std::optional<std::string> GameControls::rename_layout(const std::string& name) {
  std::lock_guard lock(mutex_);
  const std::string package = edit_package_locked();
  auto it = profiles_.find(package);
  if (it == profiles_.end() || name.empty()) return "nothing to rename";
  it->second.layout = name;
  std::ofstream out(paths_[package], std::ios::trunc);
  out << profile_to_json(it->second);
  if (!out) return "couldn't save " + paths_[package];
  out.close();
  load_locked();
  return std::nullopt;
}

std::optional<std::string> GameControls::delete_layout() {
  std::lock_guard lock(mutex_);
  const std::string package = edit_package_locked();
  auto it = layouts_.find(package);
  if (it == layouts_.end() || it->second.size() < 2) return "A game needs at least one layout.";
  std::error_code ec;
  fs::remove(paths_[package], ec);
  active_file_.erase(package);
  paths_.erase(package);
  load_locked();
  save_active_choice_locked();
  return std::nullopt;
}

std::string GameControls::profile_path(const std::string& package) const {
  std::lock_guard lock(mutex_);
  if (auto it = paths_.find(package.empty() ? foreground_ : package); it != paths_.end()) return it->second;
  return (fs::path(dir_) / "freefire.json").string();
}

std::string GameControls::edit_package_locked() const {
  if (profiles_.contains(foreground_)) return foreground_;
  return "com.dts.freefireth";
}

std::pair<std::string, std::vector<ControlMarker>> GameControls::editable_markers() const {
  std::lock_guard lock(mutex_);
  auto it = profiles_.find(edit_package_locked());
  if (it == profiles_.end()) return {"", {}};
  return {it->second.name, profile_markers(it->second)};
}

bool GameControls::has_profile(const std::string& package) const {
  std::lock_guard lock(mutex_);
  return profiles_.contains(package);
}

std::optional<std::string> GameControls::save_markers(const std::vector<ControlMarker>& markers) {
  std::lock_guard lock(mutex_);
  const std::string package = edit_package_locked();
  auto it = profiles_.find(package);
  if (it == profiles_.end()) return "no game controls to edit";
  apply_edited_markers(it->second, markers);
  std::ofstream out(paths_[package], std::ios::trunc);
  out << profile_to_json(it->second);
  if (!out) return "couldn't save " + paths_[package];
  out.close();
  files_signature_ = files_signature_locked();
  if (package == foreground_) activate_locked(foreground_);  // use the new positions now
  return std::nullopt;
}

void GameControls::sync_capture_locked() {
  const bool want = active_ && active_->aim_mode();
  if (want != captured_) {
    captured_ = want;
    if (cb_.capture_mouse) cb_.capture_mouse(want);
  }
}

void GameControls::activate_locked(const std::string& package) {
  if (active_) {
    cb_.touch(active_->release_all());
    active_.reset();
  }
  ++generation_;
  auto it = profiles_.find(package);
  if (enabled_ && it != profiles_.end()) {
    KeymapProfile p = it->second;
    if (p.aim) p.aim->sensitivity *= sensitivity_;
    active_.emplace(std::move(p), touch_);
    cb_.status(std::format("Game controls: {} — {} to aim", it->second.name,
                           it->second.aim_toggle ? key_label(it->second.aim_toggle) : "no key"));
    if (cb_.hints) cb_.hints(hint_markers(it->second));
  } else if (it != profiles_.end()) {
    cb_.status("Game controls off (⌘K)");
    if (cb_.hints) cb_.hints({});
  } else {
    cb_.status("");
    if (cb_.hints) cb_.hints({});
  }
  sync_capture_locked();
}

void GameControls::foreground_changed(const std::string& package) {
  std::lock_guard lock(mutex_);
  if (package == foreground_) return;
  foreground_ = package;
  activate_locked(package);
}

void GameControls::set_sensitivity(double factor) {
  std::lock_guard lock(mutex_);
  sensitivity_ = factor;
  activate_locked(foreground_);
}

void GameControls::toggle_enabled() {
  std::lock_guard lock(mutex_);
  enabled_ = !enabled_;
  activate_locked(foreground_);
}

void GameControls::key(uint16_t code, bool pressed) {
  std::lock_guard lock(mutex_);
  if (pressed) held_keys_.insert(code);
  else held_keys_.erase(code);
  if (active_) {
    if (typing_) {
      // Chat or another text box: every key types, none moves the character.
      cb_.keyboard(code, pressed);
      return;
    }
    if (pressed && switch_layout_locked(code)) {
      switch_down_.insert(code);
      return;
    }
    if (!pressed && switch_down_.erase(code)) return;
    auto r = active_->key(code, pressed);
    if (!r.events.empty()) cb_.touch(r.events);
    drain_timers_locked();
    sync_capture_locked();
    if (r.consumed) return;
    // Keys the layout doesn't use never reach the game: games read real key presses
    // themselves (Unity moves the player on keyboard input) and fight the joystick.
    // Esc still goes through as Back.
    constexpr uint16_t kKeyEsc = 1;
    if (code != kKeyEsc) return;
  }
  cb_.keyboard(code, pressed);
}

void GameControls::set_text_input(bool typing) {
  std::lock_guard lock(mutex_);
  if (typing == typing_) return;
  typing_ = typing;
  // Lift held fingers so a key held while the chat opened doesn't stick.
  if (typing && active_) {
    cb_.touch(active_->release_all());
    sync_capture_locked();
  }
}

void GameControls::mouse_button(uint16_t button, bool pressed) {
  std::lock_guard lock(mutex_);
  if (!active_) return;
  if (pressed && switch_layout_locked(button)) {  // a mouse button can switch layouts too
    switch_down_.insert(button);
    return;
  }
  if (!pressed && switch_down_.erase(button)) return;
  auto ev = active_->mouse_button(button, pressed);
  if (!ev.empty()) cb_.touch(ev);
  drain_timers_locked();
}

void GameControls::mouse_delta(double dx, double dy) {
  std::lock_guard lock(mutex_);
  if (!active_) return;
  auto ev = active_->mouse_move(dx, dy);
  if (!ev.empty()) cb_.touch(ev);
}

bool GameControls::switch_layout_locked(uint16_t key) {
  auto layouts = layouts_.find(foreground_);
  if (layouts == layouts_.end() || !active_) return false;
  std::string target;
  for (const auto& [file, name] : layouts->second) {
    if (auto it = all_layouts_.find(file); it != all_layouts_.end() && it->second.switch_key == key) target = file;
  }
  if (target.empty()) return false;
  const KeymapProfile& next = all_layouts_.at(target);
  if (paths_[foreground_] != target) {
    // Getting into a vehicle shouldn't stop the camera or the running: aim mode and
    // the movement keys still held carry over to the new layout.
    const bool aiming = active_->aim_mode();
    paths_[foreground_] = target;
    active_file_[foreground_] = fs::path(target).filename().string();
    profiles_[foreground_] = next;
    save_active_choice_locked();
    activate_locked(foreground_);
    if (active_ && aiming) {
      if (auto ev = active_->set_aim_mode(true); !ev.empty()) cb_.touch(ev);
      if (next.joystick) {
        const auto& js = *next.joystick;
        for (uint16_t k : {js.up, js.left, js.down, js.right}) {
          if (!held_keys_.contains(k)) continue;
          if (auto ev = active_->key(k, true).events; !ev.empty()) cb_.touch(ev);
        }
      }
    }
    sync_capture_locked();
  }
  if (cb_.layout_shown) cb_.layout_shown(next.layout);
  return true;
}

uint16_t GameControls::switch_key() const {
  std::lock_guard lock(mutex_);
  auto it = profiles_.find(edit_package_locked());
  return it == profiles_.end() ? 0 : it->second.switch_key;
}

std::optional<std::string> GameControls::set_switch_key(uint16_t key) {
  std::lock_guard lock(mutex_);
  const std::string package = edit_package_locked();
  auto it = profiles_.find(package);
  if (it == profiles_.end()) return "no game controls to edit";
  // One key per layout: take it away from the game's other layouts.
  if (key) {
    for (const auto& [file, name] : layouts_[package]) {
      if (file == paths_[package]) continue;
      auto other = all_layouts_.find(file);
      if (other == all_layouts_.end() || other->second.switch_key != key) continue;
      other->second.switch_key = 0;
      std::ofstream(file, std::ios::trunc) << profile_to_json(other->second);
    }
  }
  it->second.switch_key = key;
  std::ofstream out(paths_[package], std::ios::trunc);
  out << profile_to_json(it->second);
  if (!out) return "couldn't save " + paths_[package];
  out.close();
  load_locked();
  return std::nullopt;
}

std::optional<std::string> GameControls::export_layout(const std::string& path) const {
  std::lock_guard lock(mutex_);
  auto it = profiles_.find(edit_package_locked());
  if (it == profiles_.end()) return "no game controls to export";
  std::ofstream out(path, std::ios::trunc);
  out << profile_to_json(it->second);
  if (!out) return "couldn't write " + path;
  return std::nullopt;
}

std::optional<std::string> GameControls::import_layout(const std::string& path) {
  std::ifstream in(path);
  if (!in) return "couldn't open " + path;
  std::stringstream text;
  text << in.rdbuf();
  if (text.str().size() > (1u << 20)) return "not a Kestrel controls file (too large)";
  auto p = parse_profile(text.str());
  if (!p) return "not a Kestrel controls file: " + p.error().message;
  if (p->package.empty()) return "the file doesn't say which game it's for";
  std::lock_guard lock(mutex_);
  // Keep names apart from the layouts already there.
  std::set<std::string> names;
  for (const auto& [file, name] : layouts_[p->package]) names.insert(name);
  const std::string base = p->layout.empty() ? "Imported" : p->layout;
  p->layout = base;
  for (int n = 2; names.contains(p->layout) && n < 1000; ++n) p->layout = std::format("{} {}", base, n);
  std::string stem = p->package.substr(p->package.rfind('.') + 1);
  std::erase_if(stem, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); });
  if (stem.empty()) stem = "game";
  fs::path file;
  for (int n = 1; n < 1000; ++n) {
    file = fs::path(dir_) / std::format("{}-imported-{}.json", stem, n);
    if (!fs::exists(file)) break;
  }
  std::ofstream out(file, std::ios::trunc);
  out << profile_to_json(*p);
  if (!out) return "couldn't save " + file.string();
  out.close();
  active_file_[p->package] = file.filename().string();
  paths_[p->package] = file.string();
  save_active_choice_locked();
  load_locked();
  return std::nullopt;
}

void GameControls::focus_lost() {
  std::lock_guard lock(mutex_);
  if (!active_) return;
  cb_.touch(active_->release_all());
  sync_capture_locked();
}

}  // namespace kestrel
