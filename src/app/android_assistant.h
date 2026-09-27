#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <optional>
#include <mutex>
#include <string>
#include <thread>

#include "android/adb_client.h"

namespace kestrel {

// Background helper for the app: waits for Android to boot, applies Kestrel's
// settings (wifi on, no lock screen, host time zone), and runs user requests
// such as APK installs one at a time. Everything goes through the built-in adb
// client, so no Android SDK is needed.
class AndroidAssistant {
 public:
  using StatusFn = std::function<void(const std::string&)>;  // title-bar status
  using MessageFn = std::function<void(const std::string& title, const std::string& text)>;

  AndroidAssistant(uint16_t adb_port, StatusFn status, MessageFn message);
  ~AndroidAssistant();  // stops and joins the worker

  // Called (on the worker thread) when the app in front changes, e.g.
  // "com.dts.freefireth". Set before Android finishes booting.
  void set_foreground_listener(std::function<void(const std::string& package)> listener);
  // Called (worker thread) when a text field gains or loses the keyboard.
  void set_text_input_listener(std::function<void(bool typing)> listener);
  // Called once (worker thread) when Android's home screen is up, and again when
  // setup (wifi, time zone...) has finished.
  void set_boot_listener(std::function<void()> booted, std::function<void()> ready);

  // An app for the Library: name and icon come from its APK (cached per version).
  struct LibraryApp {
    std::string package, component, label;
    bool user_installed = false;
    std::string icon_file;            // a whole icon image on the Mac, if the app has one
    std::string foreground_file, background_file;  // adaptive icon layers otherwise
    std::optional<uint32_t> background_color;      // ARGB
  };
  // Lists launchable apps (games and user apps first); `done` runs on the worker thread.
  void list_library(std::function<void(std::vector<LibraryApp>)> done);
  void launch(const std::string& component);  // "package/.Activity"

  void install_apk(const std::string& path);
  void push_file(const std::string& path);         // -> /sdcard/Download
  void paste_text(const std::string& text);        // types it into the focused field
  void close_foreground_app();
  void open_app_store();
  // Downloads and installs the app stores (F-Droid, Aurora Store) if missing.
  // `download(url, path)` is the host's downloader. Runs once per boot.
  void set_downloader(std::function<Result<void>(const std::string& url, const std::string& path)> download,
                      std::string cache_dir);
  void shell(const std::string& command);  // fire and forget
  bool booted() const { return booted_; }

 private:
  void run();
  bool wait_for_boot();
  void configure();
  void slim_down();
  void launch_package(const std::string& package);
  std::optional<std::vector<uint8_t>> fetch_apk_entry(const std::string& apk, const std::string& entry);
  LibraryApp describe_app(const std::string& package, const std::string& component, bool user_installed);
  std::function<void()> on_booted_, on_ready_;
  void do_install(const std::string& path);
  void poll_foreground();
  void ensure_app_stores();
  bool installed(const std::string& package);

  AdbClient adb_;
  StatusFn status_;
  MessageFn message_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  std::atomic<bool> quit_{false};
  std::atomic<bool> booted_{false};
  std::function<void(const std::string&)> foreground_listener_;
  std::function<void(bool)> text_input_listener_;
  bool typing_ = false;
  std::string foreground_;
  std::function<Result<void>(const std::string&, const std::string&)> download_;
  std::string cache_dir_;
  std::thread worker_;
};

// The host's IANA time zone (e.g. "Europe/Paris"), from /etc/localtime.
std::string host_time_zone();

}  // namespace kestrel
