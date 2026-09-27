#include "host/macos/raw_mouse.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

#include "core/log.h"

namespace kestrel::raw_mouse {
namespace {

NSString* const kScaleKey = @"RawMouseScale";
NSString* const kSignXKey = @"RawMouseSignX";
NSString* const kSignYKey = @"RawMouseSignY";

// Raw counts needed before the scale is trusted: a few seconds of normal aiming.
constexpr double kCalibrationCounts = 20000;

std::function<void(double, double)> g_deliver;
std::atomic<bool> g_captured{false};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_have_raw{false};  // a GCMouse has reported movement

std::mutex g_mutex;  // calibration and scale
double g_scale = 0;  // 0: not calibrated yet
double g_sign_x = 1, g_sign_y = 1;
// Calibration sums: magnitudes over the whole run, and signed per-window sums whose
// products vote on each axis's direction.
double g_abs_sys = 0, g_abs_raw = 0;
double g_win_sys_x = 0, g_win_sys_y = 0, g_win_raw_x = 0, g_win_raw_y = 0;
double g_vote_x = 0, g_vote_y = 0;
std::chrono::steady_clock::time_point g_win_start;

void close_window_locked() {
  g_vote_x += g_win_sys_x * g_win_raw_x;
  g_vote_y += g_win_sys_y * g_win_raw_y;
  g_win_sys_x = g_win_sys_y = g_win_raw_x = g_win_raw_y = 0;
  g_win_start = std::chrono::steady_clock::now();
}

void maybe_finish_calibration_locked() {
  if (std::chrono::steady_clock::now() - g_win_start > std::chrono::milliseconds(100)) close_window_locked();
  if (g_abs_raw < kCalibrationCounts || g_abs_sys <= 0) return;
  close_window_locked();
  g_scale = g_abs_sys / g_abs_raw;
  g_sign_x = g_vote_x < 0 ? -1 : 1;
  g_sign_y = g_vote_y < 0 ? -1 : 1;
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  [d setDouble:g_scale forKey:kScaleKey];
  [d setDouble:g_sign_x forKey:kSignXKey];
  [d setDouble:g_sign_y forKey:kSignYKey];
  log(LogLevel::kInfo, "raw mouse: calibrated, {:.4f} points per count (x {:+.0f}, y {:+.0f})", g_scale, g_sign_x,
      g_sign_y);
}

void on_raw(float dx, float dy) {
  if (!g_captured || !g_enabled) return;
  g_have_raw = true;
  double sx, sy;
  {
    std::lock_guard lock(g_mutex);
    if (g_scale <= 0) {  // still measuring: the system deltas move the camera meanwhile
      g_abs_raw += std::fabs(dx) + std::fabs(dy);
      g_win_raw_x += dx;
      g_win_raw_y += dy;
      maybe_finish_calibration_locked();
      return;
    }
    sx = g_scale * g_sign_x;
    sy = g_scale * g_sign_y;
  }
  if (g_deliver) g_deliver(dx * sx, dy * sy);
}

void attach(GCMouse* mouse) {
  static dispatch_queue_t queue = dispatch_queue_create(
      "kestrel.raw-mouse", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
  mouse.handlerQueue = queue;
  mouse.mouseInput.mouseMovedHandler = ^(GCMouseInput*, float dx, float dy) { on_raw(dx, dy); };
}

}  // namespace

void start(std::function<void(double dx, double dy)> deliver) {
  g_deliver = std::move(deliver);
  NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
  if (double s = [d doubleForKey:kScaleKey]; s > 0 && s < 100) {
    g_scale = s;
    g_sign_x = [d doubleForKey:kSignXKey] < 0 ? -1 : 1;
    g_sign_y = [d doubleForKey:kSignYKey] < 0 ? -1 : 1;
  }
  g_win_start = std::chrono::steady_clock::now();
  for (GCMouse* m in GCMouse.mice) attach(m);
  [NSNotificationCenter.defaultCenter addObserverForName:GCMouseDidConnectNotification
                                                  object:nil
                                                   queue:NSOperationQueue.mainQueue
                                              usingBlock:^(NSNotification* n) { attach(n.object); }];
}

void set_captured(bool captured) { g_captured = captured; }

void set_enabled(bool enabled) { g_enabled = enabled; }

void recalibrate() {
  std::lock_guard lock(g_mutex);
  g_scale = 0;
  g_abs_sys = g_abs_raw = g_vote_x = g_vote_y = 0;
  close_window_locked();
  [NSUserDefaults.standardUserDefaults removeObjectForKey:kScaleKey];
}

bool system_delta(double dx, double dy) {
  if (!g_enabled || !g_have_raw) return false;
  std::lock_guard lock(g_mutex);
  if (g_scale > 0) return true;  // raw path is live
  g_abs_sys += std::fabs(dx) + std::fabs(dy);
  g_win_sys_x += dx;
  g_win_sys_y += dy;
  maybe_finish_calibration_locked();
  return false;
}

}  // namespace kestrel::raw_mouse
