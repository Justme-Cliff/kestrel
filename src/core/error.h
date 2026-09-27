#pragma once

#include <expected>
#include <string>
#include <utility>

namespace kestrel {

struct Error {
  std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> make_error(std::string message) {
  return std::unexpected<Error>(Error{std::move(message)});
}

}  // namespace kestrel
