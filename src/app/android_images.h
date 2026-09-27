#pragma once

#include <functional>
#include <string>

#include "core/error.h"

namespace kestrel {

// The pinned Cuttlefish build the app downloads on first launch (docs/DESIGN.md §5.10).
inline constexpr const char* kAndroidBuildId = "16373615";
inline constexpr const char* kAndroidTarget = "aosp_cf_arm64_only_phone-userdebug";

bool android_images_present(const std::string& dir);

// Downloads and unpacks the Android images into `dir` and expands super.img.
// `report(status, progress)` with progress 0..1, or <0 when unknown.
Result<void> install_android_images(const std::string& dir,
                                    const std::function<void(const std::string&, double)>& report);

}  // namespace kestrel
