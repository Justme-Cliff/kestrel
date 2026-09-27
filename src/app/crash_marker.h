#pragma once

#include <optional>
#include <string>

namespace kestrel {

// Leaves a note when Kestrel crashes (fatal signal), so the next launch can offer a
// problem report. macOS still writes its own crash report after the note.
void install_crash_marker(const std::string& path);
// The note from the last crash, if any; renames it so it's offered only once.
std::optional<std::string> take_crash_marker(const std::string& path);

}  // namespace kestrel
