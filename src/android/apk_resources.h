#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kestrel {

// Just enough of Android's binary resource formats (frameworks/base ResourceTypes.h)
// to find an app's name and launcher icon from its APK. All input is untrusted:
// every offset and length is bounds-checked, malformed data yields nullopt.

// What the manifest's <application> element says.
struct ManifestApp {
  std::optional<uint32_t> icon;        // resource id of android:icon (or roundIcon)
  std::optional<uint32_t> label_res;   // android:label as a resource id...
  std::optional<std::string> label;    // ...or as a literal string
};
std::optional<ManifestApp> parse_manifest(std::span<const uint8_t> axml);

// A drawable an icon is made of.
struct ResolvedValue {
  std::optional<std::string> path;   // file in the APK, e.g. "res/mipmap-xxxhdpi-v4/ic_launcher.png"
  std::optional<uint32_t> color;     // ARGB, for color resources
  std::optional<std::string> text;   // for string resources
};

class ResourceTable {
 public:
  static std::optional<ResourceTable> parse(std::span<const uint8_t> arsc);
  // The value for `id` in the best configuration: highest density for files,
  // the default locale for strings. Follows references (a few levels).
  std::optional<ResolvedValue> resolve(uint32_t id, bool prefer_raster = true) const;

 private:
  struct Entry {
    uint16_t density;   // ResTable_config density (0 = any, 0xfffe = anydpi)
    bool default_locale;
    uint8_t type;       // Res_value dataType
    uint32_t data;
  };
  std::vector<std::string> strings_;              // global value strings
  std::vector<std::pair<uint32_t, Entry>> entries_;  // resource id -> value, one per config
};

// Adaptive icon XML (<adaptive-icon>): drawable resource ids of its layers.
struct AdaptiveIcon {
  std::optional<uint32_t> background, foreground;
};
std::optional<AdaptiveIcon> parse_adaptive_icon(std::span<const uint8_t> axml);

}  // namespace kestrel
