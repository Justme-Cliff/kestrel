#include "host/macos/keycodes.h"

#include <array>
#include <utility>

namespace kestrel {
namespace {

// Pairs of (mac keyCode from HIToolbox Events.h, Linux input-event-codes.h).
constexpr std::array<std::pair<uint16_t, uint16_t>, 81> kMap = {{
    {0x00, 30},  {0x01, 31},  {0x02, 32},  {0x03, 33},  {0x04, 35},  {0x05, 34},   // A S D F H G
    {0x06, 44},  {0x07, 45},  {0x08, 46},  {0x09, 47},  {0x0b, 48},  {0x0c, 16},   // Z X C V B Q
    {0x0d, 17},  {0x0e, 18},  {0x0f, 19},  {0x10, 21},  {0x11, 20},  {0x12, 2},    // W E R Y T 1
    {0x13, 3},   {0x14, 4},   {0x15, 5},   {0x16, 7},   {0x17, 6},   {0x18, 13},   // 2 3 4 6 5 =
    {0x19, 10},  {0x1a, 8},   {0x1b, 12},  {0x1c, 9},   {0x1d, 11},  {0x1e, 27},   // 9 7 - 8 0 ]
    {0x1f, 24},  {0x20, 22},  {0x21, 26},  {0x22, 23},  {0x23, 25},  {0x24, 28},   // O U [ I P Return
    {0x25, 38},  {0x26, 36},  {0x27, 40},  {0x28, 37},  {0x29, 39},  {0x2a, 43},   // L J ' K ; backslash
    {0x2b, 51},  {0x2c, 53},  {0x2d, 49},  {0x2e, 50},  {0x2f, 52},  {0x30, 15},   // , / N M . Tab
    {0x31, 57},  {0x32, 41},  {0x33, 14},  {0x35, 1},   {0x37, 125}, {0x38, 42},   // Space ` Delete Esc Cmd Shift
    {0x39, 58},  {0x3a, 56},  {0x3b, 29},  {0x3c, 54},  {0x3d, 100}, {0x3e, 97},   // Caps Opt Ctrl RShift ROpt RCtrl
    {0x7b, 105}, {0x7c, 106}, {0x7d, 108}, {0x7e, 103}, {0x73, 102}, {0x77, 107},  // arrows, Home, End
    {0x74, 104}, {0x79, 109}, {0x75, 111}, {0x7a, 59},  {0x78, 60},  {0x63, 61},   // PgUp PgDn FwdDel F1 F2 F3
    {0x76, 62},  {0x60, 63},  {0x61, 64},  {0x62, 65},  {0x64, 66},  {0x65, 67},   // F4 F5 F6 F7 F8 F9
    {0x6d, 68},  {0x67, 87},  {0x6f, 88},                                          // F10 F11 F12
}};

}  // namespace

std::optional<uint16_t> mac_keycode_to_linux(uint16_t mac_keycode) {
  for (auto [mac, linux_code] : kMap) {
    if (mac == mac_keycode) return linux_code;
  }
  return std::nullopt;
}

}  // namespace kestrel
