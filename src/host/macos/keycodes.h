#pragma once

#include <cstdint>
#include <optional>

namespace kestrel {

// macOS virtual key code (NSEvent.keyCode, ANSI layout) -> Linux KEY_* code.
std::optional<uint16_t> mac_keycode_to_linux(uint16_t mac_keycode);

}  // namespace kestrel
