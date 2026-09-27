#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/settings.h"
#include "core/error.h"
#include "host/display.h"
#include "keymap/keymapper.h"

namespace kestrel {

// Toolbar, menu and gesture commands the window forwards to the emulator.
enum class WindowAction {
  kHome, kBack, kRecents, kCloseApp, kVolumeUp, kVolumeDown,
  kInstallApk,   // arg: .apk path
  kShareFile,    // arg: any other file -> Android's Download folder
  kPaste,        // arg: text from the Mac clipboard
  kAppStore, kScreenshot, kFactoryReset, kToggleGameControls, kEditGameControls, kSettings,
  kShowLibrary,  // open the game Library
  kLaunchApp,    // arg: "package/.Activity" chosen in the Library
  kReportProblem,
  kSidebarChanged,  // arg: "1" shown, "0" hidden (the user toggled the toolbar)
  kExportControls,  // arg: file to write the current layout to
  kImportControls,  // arg: controls file the user picked
  kToggleRecording, // start or stop recording the screen to a video
  kNewInstance,     // arg: name for a new Android instance
  kOpenInstance,    // arg: instance to open in another window ("" = the main one)
};

// What the user did in the controls editor.
struct ControlsEditorResult {
  enum class Action { kCancel, kSave, kSelectLayout, kNewLayout, kRenameLayout, kDeleteLayout, kSetSwitchKey };
  Action action = Action::kCancel;
  std::vector<ControlMarker> markers;  // kSave, kNewLayout, kSetSwitchKey: the edited controls
  size_t layout = 0;                   // kSelectLayout
  std::string name;                    // kNewLayout, kRenameLayout
  uint16_t key = 0;                    // kSetSwitchKey (0: none)
};

// One app tile in the Library.
struct LibraryItem {
  std::string component, label;
  std::string icon_file, foreground_file, background_file;  // images on the Mac
  std::optional<uint32_t> background_color;                 // ARGB
  bool has_controls = false;                                // Kestrel has a keymap for it
};

// What the window reports back: touches in guest display pixels, keys as Linux codes.
struct WindowInput {
  std::function<void(bool down, bool up, int32_t x, int32_t y)> pointer;  // down/move/up
  std::function<void(uint16_t linux_key, bool pressed)> key;
  std::function<void()> close_requested;
  std::function<void(WindowAction, const std::string& arg)> action;
  // While the mouse is captured (game aim mode): buttons and raw movement.
  std::function<void(uint16_t button, bool pressed)> mouse_button;  // kMouseLeft/kMouseRight
  std::function<void(double dx, double dy)> mouse_delta;
  std::function<void()> focus_lost;
  // The window was minimized or the app hidden (false), or is back (true).
  std::function<void(bool visible)> visibility;
  // Trackpad/wheel scrolling as a one-finger drag, and pinch as two fingers, in
  // guest pixels. stage: 0 begin, 1 move, 2 end.
  std::function<void(int stage, int32_t x, int32_t y)> scroll;
  std::function<void(int stage, double scale, int32_t x, int32_t y)> pinch;
};

// A native macOS window showing the guest display (AppKit + Core Animation), with
// a BlueStacks-style toolbar. Create and run on the main thread; the other
// methods may be called from any thread.
class MacWindow : public DisplaySink {
 public:
  static std::unique_ptr<MacWindow> create(const std::string& title, uint32_t width,
                                           uint32_t height, WindowInput input, bool show_sidebar = true);
  ~MacWindow() override = default;

  // Short status in the title bar, e.g. "Starting Android…"; empty clears it.
  virtual void set_status(const std::string& status) = 0;
  // Writes the last frame as a PNG.
  virtual bool save_screenshot(const std::string& path) = 0;
  // Game aim mode: hides and locks the cursor; mouse input goes to mouse_* callbacks.
  virtual void set_mouse_captured(bool captured) = 0;
  // Key hints over the game (BlueStacks-style): shown for a few seconds when a game
  // with controls comes to the front, or all the time if the user chose so. Empty
  // hides them.
  virtual void show_key_hints(std::vector<ControlMarker> markers) = 0;
  // The startup screen: app icon, `text` and a progress bar. 1.0 fades it out.
  virtual void set_boot_progress(double fraction, const std::string& text) = 0;
  // The Library over the game. `loading` shows a spinner until the apps arrive.
  virtual void show_library(std::vector<LibraryItem> items, bool loading) = 0;
  virtual void hide_library() = 0;
  // Android finished starting: shows the one-time welcome on first launch.
  virtual void android_ready() = 0;
  // Lets the GPU draw straight into this window. `attach(nswindow, x, y, w, h, dpr)`
  // gets the game area (points, top-left origin) now and after every resize; once it
  // returns true, frames no longer go through present().
  virtual void set_direct_display(
      std::function<bool(void* native_window, int x, int y, int w, int h, float dpr)> attach) = 0;
  // Shows the control editor over the game: markers the user drags into place, keys
  // they rebind, buttons they add or remove, and the game's layouts. `done` runs on
  // the main thread with what the user chose.
  virtual void edit_controls(const std::string& title, std::vector<ControlMarker> markers,
                             std::vector<std::string> layouts, size_t active_layout,
                             std::function<void(ControlsEditorResult)> done) = 0;
  // The Settings sheet. `done(settings, restart)` runs on the main thread after Save.
  virtual void show_settings(AppSettings current,
                             std::function<void(const AppSettings&, bool restart)> done) = 0;
  // A non-blocking message sheet on the window.
  virtual void show_message(const std::string& title, const std::string& text) = 0;
  // A short notice over the game that fades by itself (e.g. the layout just chosen).
  virtual void show_toast(const std::string& text, double seconds = 1.2) = 0;
  // A question with one action button and "Wait": `on_action` runs on the main
  // thread if the user picks the action. Only one at a time.
  virtual void ask(const std::string& title, const std::string& text, const std::string& action,
                   std::function<void()> on_action) = 0;
  // The red "REC" badge with a running clock while recording.
  virtual void set_recording(bool on) = 0;
  // Closes the question from ask() if it's still open (the problem went away).
  virtual void dismiss_ask() = 0;
};

// Runs the macOS event loop on the calling (main) thread until quit_app_loop().
void run_app_loop();
// Thread-safe: ends run_app_loop().
void quit_app_loop();

// Files opened with the app (Finder "Open With", drag onto the Dock icon) before
// or while it runs are passed here; set it before run_app_loop().
void set_open_files_handler(std::function<void(const std::string& path)> handler);

// First-launch setup: shows a small progress window while `task` runs on a
// background thread. `task` reports (status text, progress 0..1, or <0 for
// indeterminate). Returns the task's result after the window closes.
Result<void> run_setup_window(
    const std::string& title,
    std::function<Result<void>(const std::function<void(const std::string&, double)>&)> task);

// Downloads `url` to `path` (follows redirects), reporting progress 0..1.
Result<void> download_file(const std::string& url, const std::string& path,
                           const std::function<void(double)>& progress);

// Shows a modal error dialog (main thread).
void show_fatal_error(const std::string& title, const std::string& text);

// ~/Library/Application Support/Kestrel and ~/Library/Logs/Kestrel.
std::string app_support_dir();
std::string app_logs_dir();

// The main display's size in pixels (for "match my screen" resolutions).
std::pair<uint32_t, uint32_t> main_screen_pixels();

}  // namespace kestrel
