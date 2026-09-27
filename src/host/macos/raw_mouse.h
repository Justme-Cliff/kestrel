#pragma once

#include <functional>

namespace kestrel {

// Raw mouse movement for game aiming: no macOS pointer acceleration, every report
// the mouse sends (up to 1000 Hz and more) instead of one event per screen frame, and
// delivered on its own high-priority thread so a busy main thread can't delay it.
//
// macOS acceleration makes the same hand movement turn the camera by different
// amounts depending on speed, which ruins muscle memory. The raw path is scaled so
// that normal-speed movement matches what the player is used to: for the first few
// seconds of aiming it compares raw counts with the system's accelerated deltas and
// saves the ratio (per Mac, in the app's defaults).
namespace raw_mouse {

// Call once on the main thread. `deliver(dx, dy)` gets movement in the same units and
// direction as NSEvent deltas (points, y down), from the input thread.
void start(std::function<void(double dx, double dy)> deliver);
// Aim mode on/off (cursor captured). Main thread.
void set_captured(bool captured);
// User setting: false keeps using the system's accelerated deltas.
void set_enabled(bool enabled);
// Forgets the saved scale; it is measured again the next time the player aims.
void recalibrate();
// Feed each NSEvent mouse delta while captured. Returns true if the raw path is
// delivering movement, so the caller must drop this event (else it would count twice).
bool system_delta(double dx, double dy);

}  // namespace raw_mouse
}  // namespace kestrel
