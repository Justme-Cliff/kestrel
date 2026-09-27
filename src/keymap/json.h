#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/error.h"

namespace kestrel {

// A small JSON value and parser (RFC 8259 subset: no \u escapes beyond ASCII)
// for keymap profiles. Profiles are user files: parsing never trusts input.
class Json {
 public:
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json, std::less<>>;

  Json() = default;
  explicit Json(double n) : v_(n) {}
  explicit Json(bool b) : v_(b) {}
  explicit Json(std::string s) : v_(std::move(s)) {}
  explicit Json(Array a) : v_(std::make_shared<Array>(std::move(a))) {}
  explicit Json(Object o) : v_(std::make_shared<Object>(std::move(o))) {}

  static Result<Json> parse(std::string_view text);

  bool is_null() const { return std::holds_alternative<std::monostate>(v_); }
  std::optional<double> number() const;
  std::optional<bool> boolean() const;
  std::optional<std::string> string() const;
  const Array* array() const;
  const Object* object() const;

  // Object member or null.
  const Json& operator[](std::string_view key) const;

  std::string dump(int indent = 0) const;

 private:
  std::variant<std::monostate, double, bool, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v_;
};

}  // namespace kestrel
