#include "app/android_assistant.h"
#include <algorithm>
#include "android/apk_resources.h"
#include <cctype>
#include <sstream>
#include <array>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>

#include "core/log.h"

namespace kestrel {

std::string host_time_zone() {
  std::error_code ec;
  const std::string target = std::filesystem::read_symlink("/etc/localtime", ec).string();
  const size_t pos = target.find("zoneinfo/");
  return pos == std::string::npos ? "" : target.substr(pos + 9);
}

AndroidAssistant::AndroidAssistant(uint16_t adb_port, StatusFn status, MessageFn message)
    : adb_(adb_port), status_(std::move(status)), message_(std::move(message)) {
  worker_ = std::thread([this] { run(); });
}

AndroidAssistant::~AndroidAssistant() {
  quit_ = true;
  cv_.notify_all();
  worker_.join();
}

void AndroidAssistant::set_text_input_listener(std::function<void(bool)> listener) {
  std::lock_guard lock(mutex_);
  text_input_listener_ = std::move(listener);
}

void AndroidAssistant::set_foreground_listener(std::function<void(const std::string&)> listener) {
  std::lock_guard lock(mutex_);
  foreground_listener_ = std::move(listener);
}

void AndroidAssistant::poll_foreground() {
  // "topResumedActivity=ActivityRecord{... u0 com.dts.freefireth/.MainActivity t12}",
  // then whether a text field has the keyboard (typing in chat, a login box...).
  auto out = adb_.shell(
      "dumpsys activity activities | grep -m1 topResumedActivity; "
      "dumpsys input_method | grep -m2 -oE 'mInputShown=[a-z]+|mServedInputConnection=[^ ]+'");
  if (!out) return;
  const bool typing = out->find("mInputShown=true") != std::string::npos ||
                      (out->find("mServedInputConnection=") != std::string::npos &&
                       out->find("mServedInputConnection=null") == std::string::npos);
  {
    std::function<void(bool)> text_listener;
    {
      std::lock_guard lock(mutex_);
      if (typing != typing_) {
        typing_ = typing;
        text_listener = text_input_listener_;
      }
    }
    if (text_listener) text_listener(typing);
  }
  const size_t u0 = out->find(" u0 ");
  const size_t slash = out->find('/', u0 == std::string::npos ? 0 : u0);
  if (u0 == std::string::npos || slash == std::string::npos) return;
  const std::string package = out->substr(u0 + 4, slash - u0 - 4);
  std::function<void(const std::string&)> listener;
  {
    std::lock_guard lock(mutex_);
    if (package == foreground_) return;
    foreground_ = package;
    listener = foreground_listener_;
  }
  if (listener) listener(package);
}

void AndroidAssistant::set_downloader(
    std::function<Result<void>(const std::string&, const std::string&)> download, std::string cache_dir) {
  std::lock_guard lock(mutex_);
  download_ = std::move(download);
  cache_dir_ = std::move(cache_dir);
}

bool AndroidAssistant::installed(const std::string& package) {
  auto r = adb_.shell("pm path " + package);
  return r && r->find("package:") != std::string::npos;
}

void AndroidAssistant::ensure_app_stores() {
  std::function<Result<void>(const std::string&, const std::string&)> download;
  std::string dir;
  {
    std::lock_guard lock(mutex_);
    download = download_;
    dir = cache_dir_;
  }
  if (!download) return;
  // F-Droid: open-source apps. Aurora Store: an open-source client for the Google
  // Play catalog (free apps, anonymous or with your own account). Both come from
  // f-droid.org, the official source.
  struct Store {
    const char* package;
    const char* name;
  };
  for (const Store& s : {Store{"org.fdroid.fdroid", "F-Droid"}, Store{"com.aurora.store", "Aurora Store"}}) {
    if (quit_ || installed(s.package)) continue;
    status_(std::format("Installing {}…", s.name));
    std::string url;
    if (std::string_view(s.package) == "org.fdroid.fdroid") {
      url = "https://f-droid.org/F-Droid.apk";
    } else {
      // The current version from F-Droid's public package API.
      const std::string meta = dir + "/aurora.json";
      if (!download("https://f-droid.org/api/v1/packages/com.aurora.store", meta)) continue;
      std::ifstream in(meta);
      std::string json((std::istreambuf_iterator<char>(in)), {});
      const size_t k = json.find("\"suggestedVersionCode\":");
      if (k == std::string::npos) continue;
      url = std::format("https://f-droid.org/repo/com.aurora.store_{}.apk", std::atoll(json.c_str() + k + 23));
    }
    const std::string apk = dir + "/" + s.package + ".apk";
    if (auto r = download(url, apk); !r) {
      log(LogLevel::kWarn, "app store download failed: {}", r.error().message);
      continue;
    }
    if (auto r = adb_.install(apk); !r) log(LogLevel::kWarn, "{}: {}", s.name, r.error().message);
  }
  status_("");
}

void AndroidAssistant::open_app_store() {
  std::lock_guard lock(mutex_);
  jobs_.push_back([this] {
    const char* package = installed("com.aurora.store") ? "com.aurora.store" : "org.fdroid.fdroid";
    if (!installed(package)) {
      ensure_app_stores();
      if (!installed(package)) {
        message_("App Store isn't ready", "Kestrel couldn't download the app store. Check your internet connection.");
        return;
      }
    }
    launch_package(package);
  });
  cv_.notify_all();
}

void AndroidAssistant::push_file(const std::string& path) {
  std::lock_guard lock(mutex_);
  jobs_.push_back([this, path] {
    const std::string name = std::filesystem::path(path).filename().string();
    status_(std::format("Copying {}…", name));
    const std::string remote = "/sdcard/Download/" + name;
    auto r = adb_.push(path, remote);
    status_("");
    if (!r) {
      message_("Couldn't copy " + name, r.error().message);
      return;
    }
    // Let gallery/music apps see it right away.
    (void)adb_.shell("am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://" + remote + "'");
    message_("File copied", name + " is in Android's Download folder.");
  });
  cv_.notify_all();
}

void AndroidAssistant::paste_text(const std::string& text) {
  // `input text` takes one shell word; spaces become %s. Quote everything else.
  std::string arg;
  for (char c : text.substr(0, 4000)) {
    if (c == ' ') arg += "%s";
    else if (c == '\'' ) arg += "'\\''";
    else if (c == '\n' || c == '\r') arg += "%s";
    else arg += c;
  }
  std::lock_guard lock(mutex_);
  jobs_.push_back([this, arg] { (void)adb_.shell("input text '" + arg + "'"); });
  cv_.notify_all();
}

void AndroidAssistant::close_foreground_app() {
  std::lock_guard lock(mutex_);
  jobs_.push_back([this] {
    std::string package;
    {
      std::lock_guard l(mutex_);
      package = foreground_;
    }
    if (package.empty() || package.find("launcher") != std::string::npos) return;
    (void)adb_.shell("am force-stop " + package + " && input keyevent KEYCODE_HOME");
  });
  cv_.notify_all();
}

void AndroidAssistant::install_apk(const std::string& path) {
  std::lock_guard lock(mutex_);
  jobs_.push_back([this, path] { do_install(path); });
  cv_.notify_all();
}

void AndroidAssistant::shell(const std::string& command) {
  std::lock_guard lock(mutex_);
  jobs_.push_back([this, command] { (void)adb_.shell(command); });
  cv_.notify_all();
}

bool AndroidAssistant::wait_for_boot() {
  // adbd restarts once shortly after boot; keep asking until the answer sticks.
  while (!quit_) {
    auto r = adb_.shell("getprop sys.boot_completed");
    if (r && r->starts_with("1")) return true;
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, std::chrono::seconds(1), [&] { return quit_.load(); });
  }
  return false;
}

// Apps and services this VM has no use for (no SIM, NFC, camera, printers...). Each
// was checked to boot and run games without. Never Bluetooth: system_server won't
// start without its package. Disabled per user, so a factory reset brings them back.
void AndroidAssistant::slim_down() {
  static const char* kUnneeded[] = {
      "com.android.nfc", "com.google.android.telephony.satellite", "com.android.cellbroadcastreceiver.module",
      "com.android.messaging", "com.android.contacts", "com.android.dialer", "com.android.se",
      "com.android.healthconnect.controller", "com.android.statementservice", "com.android.camera2",
      "com.android.calendar", "com.android.deskclock", "com.android.music", "com.android.printspooler",
      "com.android.bips", "com.android.dreams.basic", "com.android.dreams.phototable",
      "com.android.wallpaper.livepicker", "com.android.traceur", "com.android.egg", "com.android.stk",
      "com.android.emergency", "com.android.bookmarkprovider", "com.android.devicediagnostics",
  };
  auto done = adb_.shell("settings get global kestrel_slim");
  if (done && done->find("1") != std::string::npos) return;
  std::string cmd;
  for (const char* p : kUnneeded) cmd += std::string("pm disable-user --user 0 ") + p + " >/dev/null 2>&1; ";
  cmd += "settings put global kestrel_slim 1";
  (void)adb_.shell(cmd);
  log(LogLevel::kInfo, "android: switched off {} unneeded system apps", std::size(kUnneeded));
}

void AndroidAssistant::configure() {
  // Idempotent: runs on every boot.
  const char* commands[] = {
      "locksettings set-disabled true",
      "settings put system screen_off_timeout 2147483647",
      "svc power stayon true",
      "input keyevent KEYCODE_WAKEUP",
      "wm dismiss-keyguard",
      "svc wifi enable",
      // No Bluetooth hardware in the VM: keep the stack off so it can't crash-loop.
      "cmd bluetooth_manager disable",
      "settings put global bluetooth_on 0",
      // Back / Home / Recents buttons at the bottom, like a normal phone.
      "cmd overlay enable-exclusive --category com.android.internal.systemui.navbar.threebutton",
  };
  for (const char* c : commands) (void)adb_.shell(c);
  slim_down();
  if (const std::string tz = host_time_zone(); !tz.empty()) {
    (void)adb_.shell("cmd alarm set-timezone " + tz);
  }
  // Cuttlefish's virtual wifi network is backed by Kestrel's NAT.
  for (int i = 0; i < 20 && !quit_; ++i) {
    (void)adb_.shell("cmd wifi connect-network VirtWifi open");
    auto ip = adb_.shell("ip -4 addr show wlan0");
    if (ip && ip->find("inet ") != std::string::npos) break;
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}

void AndroidAssistant::do_install(const std::string& path) {
  const std::string name = std::filesystem::path(path).filename().string();
  status_(std::format("Installing {}…", name));
  auto r = adb_.install(path, [&](double p) {
    status_(std::format("Installing {}… {}%", name, int(p * 100)));
  });
  status_("");
  if (r) {
    message_("App installed", name + " is ready. Open it from the Android home screen.");
  } else {
    message_("Couldn't install " + name, r.error().message);
  }
}

void AndroidAssistant::set_boot_listener(std::function<void()> booted, std::function<void()> ready) {
  std::lock_guard lock(mutex_);
  on_booted_ = std::move(booted);
  on_ready_ = std::move(ready);
}

void AndroidAssistant::run() {
  status_("Starting Android…");
  if (!wait_for_boot()) return;
  std::function<void()> booted, ready;
  {
    std::lock_guard lock(mutex_);
    booted = on_booted_;
    ready = on_ready_;
  }
  if (booted) booted();
  status_("Setting up…");
  configure();
  booted_ = true;
  if (ready) ready();
  status_("");
  log(LogLevel::kInfo, "Android is ready");
  ensure_app_stores();
  while (!quit_) {
    std::function<void()> job;
    {
      std::unique_lock lock(mutex_);
      // Idle: check the foreground app and text input every second (per-game controls).
      if (!cv_.wait_for(lock, std::chrono::seconds(1), [&] { return quit_ || !jobs_.empty(); })) {
        lock.unlock();
        poll_foreground();
        continue;
      }
      if (quit_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job();
  }
}

}  // namespace kestrel

// ---- Library ----

namespace kestrel {

namespace {

std::vector<uint8_t> base64_decode(std::string_view text) {
  static constexpr auto table = [] {
    std::array<int8_t, 256> t{};
    t.fill(-1);
    const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
    return t;
  }();
  std::vector<uint8_t> out;
  out.reserve(text.size() * 3 / 4);
  uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    const int8_t v = table[static_cast<uint8_t>(c)];
    if (v < 0) continue;  // newlines, padding
    acc = acc << 6 | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>(acc >> bits));
    }
  }
  return out;
}

// Shell-safe single quoting (APK paths contain '=' and '~').
std::string quote(const std::string& s) {
  std::string q = "'";
  for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return q + "'";
}

bool write_bytes(const std::filesystem::path& path, std::span<const uint8_t> data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return bool(out);
}

// Readable name from a package when the APK doesn't say: "com.dts.freefireth" -> "Freefireth".
std::string name_from_package(const std::string& package) {
  std::string last = package.substr(package.rfind('.') + 1);
  if (!last.empty()) last[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(last[0])));
  return last;
}

}  // namespace

void AndroidAssistant::launch_package(const std::string& package) {
  // `am start` of the launcher activity: exactly what tapping the icon does.
  (void)adb_.shell(std::format("am start -n $(cmd package resolve-activity --brief {} | tail -1)", quote(package)));
}

void AndroidAssistant::launch(const std::string& component) {
  std::lock_guard lock(mutex_);
  jobs_.push_back([this, component] { (void)adb_.shell("am start -n " + quote(component)); });
  cv_.notify_one();
}

std::optional<std::vector<uint8_t>> AndroidAssistant::fetch_apk_entry(const std::string& apk,
                                                                      const std::string& entry) {
  auto out = adb_.shell(std::format("unzip -p {} {} | base64", quote(apk), quote(entry)));
  if (!out) return std::nullopt;
  auto data = base64_decode(*out);
  if (data.empty()) return std::nullopt;
  return data;
}

AndroidAssistant::LibraryApp AndroidAssistant::describe_app(const std::string& package,
                                                            const std::string& component, bool user_installed) {
  LibraryApp app{package, component, name_from_package(package), user_installed, {}, {}, {}, {}};
  std::string version = "0";
  if (auto d = adb_.shell(std::format("dumpsys package {} | grep -m1 versionCode", quote(package)))) {
    if (auto p = d->find("versionCode="); p != std::string::npos) {
      version = d->substr(p + 12, d->find_first_of(" \r\n", p + 12) - (p + 12));
    }
  }
  // Cached per app version: <cache>/icons/<package>@<version>/
  const auto dir = std::filesystem::path(cache_dir_.empty() ? "/tmp" : cache_dir_) / "icons" / (package + "@" + version);
  std::error_code ec;
  auto load_cache = [&] {
    std::ifstream meta(dir / "meta.txt");
    if (!meta) return false;
    std::string key, value;
    while (std::getline(meta, key, '=') && std::getline(meta, value)) {
      if (key == "label" && !value.empty()) app.label = value;
      if (key == "color") app.background_color = static_cast<uint32_t>(std::stoul(value, nullptr, 16));
    }
    for (const char* f : {"icon", "fg", "bg"}) {
      if (std::filesystem::exists(dir / f)) {
        (std::string(f) == "icon" ? app.icon_file : std::string(f) == "fg" ? app.foreground_file : app.background_file) =
            (dir / f).string();
      }
    }
    return true;
  };
  if (load_cache()) return app;

  std::filesystem::create_directories(dir, ec);
  auto apk_line = adb_.shell(std::format("pm path {} | head -1", quote(package)));
  std::string apk = apk_line ? *apk_line : "";
  if (auto p = apk.find("package:"); p != std::string::npos) apk = apk.substr(p + 8);
  while (!apk.empty() && std::isspace(static_cast<unsigned char>(apk.back()))) apk.pop_back();

  std::optional<ResourceTable> table;
  std::optional<ManifestApp> manifest;
  if (!apk.empty()) {
    if (auto m = fetch_apk_entry(apk, "AndroidManifest.xml")) manifest = parse_manifest(*m);
    if (auto r = fetch_apk_entry(apk, "resources.arsc")) table = ResourceTable::parse(*r);
  }
  if (manifest && table) {
    if (manifest->label) app.label = *manifest->label;
    if (manifest->label_res) {
      if (auto v = table->resolve(*manifest->label_res); v && v->text && !v->text->empty()) app.label = *v->text;
    }
    // A layer or whole icon: an image file (saved to `name`), or a color.
    auto save_layer = [&](uint32_t id, const char* name, std::string& file, std::optional<uint32_t>* color) {
      auto v = table->resolve(id);
      if (!v) return false;
      if (v->color && color) {
        *color = *v->color;
        return true;
      }
      if (!v->path || v->path->ends_with(".xml")) return false;  // vector drawables: not drawn here
      auto data = fetch_apk_entry(apk, *v->path);
      if (!data || !write_bytes(dir / name, *data)) return false;
      file = (dir / name).string();
      return true;
    };
    if (manifest->icon) {
      auto v = table->resolve(*manifest->icon);
      if (v && v->path && v->path->ends_with(".xml")) {
        // Adaptive icon: background and foreground layers.
        if (auto xml = fetch_apk_entry(apk, *v->path)) {
          if (auto adaptive = parse_adaptive_icon(*xml)) {
            if (adaptive->background) save_layer(*adaptive->background, "bg", app.background_file, &app.background_color);
            if (adaptive->foreground) save_layer(*adaptive->foreground, "fg", app.foreground_file, nullptr);
          }
        }
      } else if (v && v->path) {
        save_layer(*manifest->icon, "icon", app.icon_file, nullptr);
      }
    }
  }
  std::ofstream meta(dir / "meta.txt", std::ios::trunc);
  meta << "label=" << app.label << "\n";
  if (app.background_color) meta << "color=" << std::format("{:08x}", *app.background_color) << "\n";
  return app;
}

void AndroidAssistant::list_library(std::function<void(std::vector<LibraryApp>)> done) {
  std::lock_guard lock(mutex_);
  jobs_.push_back([this, done = std::move(done)] {
    std::vector<LibraryApp> apps;
    auto third = adb_.shell("cmd package list packages -3");
    auto launchable = adb_.shell(
        "cmd package query-activities --brief -a android.intent.action.MAIN -c android.intent.category.LAUNCHER");
    if (!third || !launchable) {
      done(std::move(apps));
      return;
    }
    // Useful system apps; the rest (search box, demos, WebView shell) stay hidden.
    static const char* kSystemApps[] = {"com.android.settings", "com.android.documentsui", "com.android.gallery3d"};
    std::istringstream lines(*launchable);
    std::string line;
    std::vector<std::pair<std::string, std::string>> found;  // package, component
    while (std::getline(lines, line)) {
      const auto slash = line.find('/');
      if (slash == std::string::npos) continue;
      auto begin = line.find_first_not_of(" \t");
      std::string component = line.substr(begin);
      while (!component.empty() && std::isspace(static_cast<unsigned char>(component.back()))) component.pop_back();
      found.emplace_back(component.substr(0, component.find('/')), component);
    }
    for (const auto& [package, component] : found) {
      const bool user = third->find("package:" + package + "\n") != std::string::npos ||
                        third->ends_with("package:" + package);
      const bool wanted = user || std::ranges::any_of(kSystemApps, [&](const char* s) { return package == s; });
      if (!wanted || quit_) continue;
      apps.push_back(describe_app(package, component, user));
    }
    std::stable_sort(apps.begin(), apps.end(), [](const LibraryApp& a, const LibraryApp& b) {
      return a.user_installed > b.user_installed;
    });
    done(std::move(apps));
  });
  cv_.notify_one();
}

}  // namespace kestrel
