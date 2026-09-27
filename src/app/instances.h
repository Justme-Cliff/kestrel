#pragma once

#include <string>
#include <vector>

#include "core/error.h"

namespace kestrel {

// Several Androids side by side, like BlueStacks' multi-instance: each has its own
// apps and data but shares the (read-only) Android images with the main one through
// APFS clones, so a new instance costs no extra disk space until it's used.
//
// A Kestrel process runs one instance, picked by the KESTREL_INSTANCE environment
// variable ("" = the main one); restarts keep it because the environment carries over.
std::string current_instance();
bool valid_instance_name(const std::string& name);
// ~/Library/Application Support/Kestrel/android or .../android-<name>.
std::string instance_dir(const std::string& support_dir, const std::string& name);
// Names of the extra instances that exist (not the main one, not test copies).
std::vector<std::string> list_instances(const std::string& support_dir);
// Makes a new instance from the main one's images (fresh apps and data).
Result<void> create_instance(const std::string& support_dir, const std::string& name);
// Starts another Kestrel window running `name` ("" = main).
Result<void> launch_instance(const std::string& name);

}  // namespace kestrel
