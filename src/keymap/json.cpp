#include "keymap/json.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>

namespace kestrel {
namespace {

const Json kNull;
constexpr int kMaxDepth = 32;

class Parser {
 public:
  explicit Parser(std::string_view t) : t_(t) {}

  Result<Json> document() {
    auto v = value(0);
    if (!v) return v;
    skip_ws();
    if (pos_ != t_.size()) return fail("trailing characters");
    return v;
  }

 private:
  std::unexpected<Error> fail(std::string_view what) {
    return make_error(std::format("JSON: {} at offset {}", what, pos_));
  }
  void skip_ws() {
    while (pos_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[pos_]))) ++pos_;
  }
  bool consume(std::string_view lit) {
    if (t_.substr(pos_, lit.size()) != lit) return false;
    pos_ += lit.size();
    return true;
  }

  Result<Json> value(int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    skip_ws();
    if (pos_ >= t_.size()) return fail("unexpected end");
    const char c = t_[pos_];
    if (c == '{') return object(depth);
    if (c == '[') return array(depth);
    if (c == '"') {
      auto s = str();
      if (!s) return std::unexpected(s.error());
      return Json(std::move(*s));
    }
    if (consume("true")) return Json(true);
    if (consume("false")) return Json(false);
    if (consume("null")) return Json();
    return number();
  }

  Result<std::string> str() {
    ++pos_;  // opening quote
    std::string out;
    while (pos_ < t_.size()) {
      const char c = t_[pos_++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= t_.size()) break;
      const char e = t_[pos_++];
      switch (e) {
        case '"': case '\\': case '/': out.push_back(e); break;
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        default: return fail("unsupported escape");
      }
    }
    return fail("unterminated string");
  }

  Result<Json> number() {
    const size_t start = pos_;
    while (pos_ < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[pos_])) ||
                                t_[pos_] == '-' || t_[pos_] == '+' || t_[pos_] == '.' ||
                                t_[pos_] == 'e' || t_[pos_] == 'E')) {
      ++pos_;
    }
    if (start == pos_) return fail("unexpected character");
    const std::string s(t_.substr(start, pos_ - start));
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(d)) return fail("bad number");
    return Json(d);
  }

  Result<Json> array(int depth) {
    ++pos_;
    Json::Array a;
    skip_ws();
    if (consume("]")) return Json(std::move(a));
    while (true) {
      auto v = value(depth + 1);
      if (!v) return v;
      a.push_back(std::move(*v));
      skip_ws();
      if (consume("]")) return Json(std::move(a));
      if (!consume(",")) return fail("expected , or ]");
    }
  }

  Result<Json> object(int depth) {
    ++pos_;
    Json::Object o;
    skip_ws();
    if (consume("}")) return Json(std::move(o));
    while (true) {
      skip_ws();
      if (pos_ >= t_.size() || t_[pos_] != '"') return fail("expected key");
      auto key = str();
      if (!key) return std::unexpected(key.error());
      skip_ws();
      if (!consume(":")) return fail("expected :");
      auto v = value(depth + 1);
      if (!v) return v;
      o[std::move(*key)] = std::move(*v);
      skip_ws();
      if (consume("}")) return Json(std::move(o));
      if (!consume(",")) return fail("expected , or }");
    }
  }

  std::string_view t_;
  size_t pos_ = 0;
};

std::string escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace

Result<Json> Json::parse(std::string_view text) { return Parser(text).document(); }

std::optional<double> Json::number() const {
  if (auto* d = std::get_if<double>(&v_)) return *d;
  return std::nullopt;
}
std::optional<bool> Json::boolean() const {
  if (auto* b = std::get_if<bool>(&v_)) return *b;
  return std::nullopt;
}
std::optional<std::string> Json::string() const {
  if (auto* s = std::get_if<std::string>(&v_)) return *s;
  return std::nullopt;
}
const Json::Array* Json::array() const {
  if (auto* a = std::get_if<std::shared_ptr<Array>>(&v_)) return a->get();
  return nullptr;
}
const Json::Object* Json::object() const {
  if (auto* o = std::get_if<std::shared_ptr<Object>>(&v_)) return o->get();
  return nullptr;
}
const Json& Json::operator[](std::string_view key) const {
  if (const Object* o = object()) {
    if (auto it = o->find(key); it != o->end()) return it->second;
  }
  return kNull;
}

std::string Json::dump(int indent) const {
  const std::string pad(indent, ' '), inner(indent + 2, ' ');
  if (is_null()) return "null";
  if (auto n = number()) {
    std::string s = std::format("{}", *n);
    return s;
  }
  if (auto b = boolean()) return *b ? "true" : "false";
  if (auto s = string()) return "\"" + escape(*s) + "\"";
  if (const Array* a = array()) {
    if (a->empty()) return "[]";
    std::string out = "[\n";
    for (size_t i = 0; i < a->size(); ++i) {
      out += inner + (*a)[i].dump(indent + 2) + (i + 1 < a->size() ? ",\n" : "\n");
    }
    return out + pad + "]";
  }
  const Object* o = object();
  if (o->empty()) return "{}";
  std::string out = "{\n";
  size_t i = 0;
  for (const auto& [k, v] : *o) {
    out += inner + "\"" + escape(k) + "\": " + v.dump(indent + 2) + (++i < o->size() ? ",\n" : "\n");
  }
  return out + pad + "}";
}

}  // namespace kestrel
