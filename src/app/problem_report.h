#pragma once

#include <string>

#include "core/error.h"

namespace kestrel {

// Collects what's needed to diagnose a problem (Kestrel's logs, the tail of
// Android's log, settings, game controls, Mac model and macOS version) into a zip
// on the Desktop. Returns the zip's path.
Result<std::string> write_problem_report(const std::string& logs_dir, const std::string& support_dir,
                                         const std::string& android_dir, const std::string& version);

}  // namespace kestrel
