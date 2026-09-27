#pragma once

#include <array>
#include <cstdint>

namespace kestrel {

// A 128-byte EDID 1.3 block (VESA E-EDID) describing one display mode. The guest's
// virtio-gpu driver builds its mode list from it, so the refresh rate here is the
// rate Android renders at: our FPS cap.
std::array<uint8_t, 128> make_edid(uint32_t width, uint32_t height, uint32_t refresh_hz);

}  // namespace kestrel
