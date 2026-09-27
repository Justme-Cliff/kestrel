#include "devices/virtio/edid.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kestrel {

std::array<uint8_t, 128> make_edid(uint32_t width, uint32_t height, uint32_t refresh_hz) {
  std::array<uint8_t, 128> e{};
  width = std::clamp<uint32_t>(width, 64, 4095);  // detailed timings hold 12-bit sizes
  height = std::clamp<uint32_t>(height, 64, 4095);
  refresh_hz = std::clamp<uint32_t>(refresh_hz, 20, 240);

  const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  std::memcpy(e.data(), header, 8);
  // Manufacturer "KST": three 5-bit letters, big-endian (EDID 1.3 §3.4).
  const uint16_t mfg = static_cast<uint16_t>(('K' - '@') << 10 | ('S' - '@') << 5 | ('T' - '@'));
  e[8] = static_cast<uint8_t>(mfg >> 8);
  e[9] = static_cast<uint8_t>(mfg);
  e[10] = 0x01;  // product code
  e[16] = 1;     // week
  e[17] = 36;    // 2026 - 1990
  e[18] = 1;     // EDID 1.3
  e[19] = 3;
  e[20] = 0x80;  // digital input
  e[21] = 0;     // screen size unknown: density comes from Android's settings
  e[22] = 0;
  e[23] = 120;   // gamma 2.2
  e[24] = 0x0a;  // RGB, first detailed timing is the preferred mode
  const uint8_t srgb[10] = {0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54};
  std::memcpy(e.data() + 25, srgb, 10);
  for (int i = 38; i < 54; ++i) e[i] = 0x01;  // no standard timings

  // Detailed timing: reduced blanking (CVT-RB style): 160 pixels of horizontal
  // blanking, and enough lines for >= 460 us of vertical blanking.
  const uint32_t hblank = 160, hfp = 48, hsync = 32;
  const double vb_time = 460e-6 * refresh_hz;
  uint32_t vblank = static_cast<uint32_t>(std::ceil(vb_time * height / (1.0 - vb_time))) + 1;
  vblank = std::clamp<uint32_t>(vblank, 22, 4095);
  const uint32_t vfp = 3, vsync = 5;
  const uint64_t htotal = width + hblank, vtotal = height + vblank;
  uint64_t clock_10khz = (htotal * vtotal * refresh_hz + 5000) / 10000;
  clock_10khz = std::min<uint64_t>(clock_10khz, 0xffff);  // 655 MHz: EDID's limit
  uint8_t* d = e.data() + 54;
  d[0] = static_cast<uint8_t>(clock_10khz);
  d[1] = static_cast<uint8_t>(clock_10khz >> 8);
  d[2] = static_cast<uint8_t>(width);
  d[3] = static_cast<uint8_t>(hblank);
  d[4] = static_cast<uint8_t>((width >> 8) << 4 | (hblank >> 8));
  d[5] = static_cast<uint8_t>(height);
  d[6] = static_cast<uint8_t>(vblank);
  d[7] = static_cast<uint8_t>((height >> 8) << 4 | (vblank >> 8));
  d[8] = static_cast<uint8_t>(hfp);
  d[9] = static_cast<uint8_t>(hsync);
  d[10] = static_cast<uint8_t>((vfp & 0xf) << 4 | (vsync & 0xf));
  d[11] = 0;
  d[17] = 0x1e;  // digital separate sync, +hsync +vsync

  // Monitor name "Kestrel" (descriptor 0xfc).
  uint8_t* n = e.data() + 72;
  n[3] = 0xfc;
  const char name[] = "Kestrel";
  std::memset(n + 5, ' ', 13);
  std::memcpy(n + 5, name, sizeof(name) - 1);
  n[5 + sizeof(name) - 1] = 0x0a;
  // Two unused descriptors (0x10).
  e[90 + 3] = 0x10;
  e[108 + 3] = 0x10;

  e[126] = 0;  // no extensions
  uint8_t sum = 0;
  for (int i = 0; i < 127; ++i) sum = static_cast<uint8_t>(sum + e[i]);
  e[127] = static_cast<uint8_t>(0x100 - sum);
  return e;
}

}  // namespace kestrel
