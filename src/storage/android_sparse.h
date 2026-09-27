#pragma once

#include <string>

#include "core/error.h"

namespace kestrel {

// Expands an Android sparse image (system/core/libsparse/sparse_format.h) into a
// raw image. "Don't care" and zero-fill chunks become holes, so the output stays
// sparse on disk. A raw input is copied as-is.
Result<void> unsparse_image(const std::string& in_path, const std::string& out_path);

bool is_android_sparse(const std::string& path);

}  // namespace kestrel
