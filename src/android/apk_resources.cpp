#include "android/apk_resources.h"

#include <algorithm>
#include <functional>

namespace kestrel {

namespace {

constexpr uint16_t kStringPool = 0x0001, kTable = 0x0002, kXml = 0x0003;
constexpr uint16_t kXmlStartElement = 0x0102, kXmlResourceMap = 0x0180;
constexpr uint16_t kTablePackage = 0x0200, kTableType = 0x0201;
constexpr uint8_t kTypeReference = 0x01, kTypeString = 0x03;
constexpr uint8_t kTypeColorArgb8 = 0x1c, kTypeColorRgb8 = 0x1d, kTypeColorArgb4 = 0x1e, kTypeColorRgb4 = 0x1f;
constexpr uint32_t kAttrLabel = 0x01010001, kAttrIcon = 0x01010002, kAttrRoundIcon = 0x0101052c;
constexpr uint32_t kAttrDrawable = 0x01010199;
constexpr size_t kMaxStrings = 1 << 20, kMaxEntries = 1 << 22, kMaxChunks = 1 << 20;

// Bounds-checked little-endian reads; out-of-range reads fail.
struct Bytes {
  std::span<const uint8_t> d;
  bool has(size_t off, size_t n) const { return off <= d.size() && n <= d.size() - off; }
  std::optional<uint8_t> u8(size_t o) const { return has(o, 1) ? std::optional<uint8_t>(d[o]) : std::nullopt; }
  std::optional<uint16_t> u16(size_t o) const {
    if (!has(o, 2)) return std::nullopt;
    return static_cast<uint16_t>(d[o] | d[o + 1] << 8);
  }
  std::optional<uint32_t> u32(size_t o) const {
    if (!has(o, 4)) return std::nullopt;
    return static_cast<uint32_t>(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t{d[o + 3]} << 24);
  }
};

struct Chunk {
  uint16_t type, header_size;
  uint32_t size;
};
std::optional<Chunk> chunk_at(const Bytes& b, size_t off) {
  auto t = b.u16(off), h = b.u16(off + 2);
  auto s = b.u32(off + 4);
  if (!t || !h || !s || *h < 8 || *s < *h || !b.has(off, *s)) return std::nullopt;
  return Chunk{*t, *h, *s};
}

void append_utf8(std::string& out, uint32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xc0 | c >> 6);
    out += static_cast<char>(0x80 | (c & 0x3f));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xe0 | c >> 12);
    out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
    out += static_cast<char>(0x80 | (c & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | c >> 18);
    out += static_cast<char>(0x80 | (c >> 12 & 0x3f));
    out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
    out += static_cast<char>(0x80 | (c & 0x3f));
  }
}

// ResStringPool at `off` (a chunk of `size` bytes). Strings that don't decode are empty.
std::optional<std::vector<std::string>> string_pool(const Bytes& b, size_t off, const Chunk& c) {
  auto count = b.u32(off + 8), flags = b.u32(off + 16), start = b.u32(off + 20);
  if (!count || !flags || !start || *count > kMaxStrings) return std::nullopt;
  const bool utf8 = *flags & 0x100;
  const size_t end = off + c.size;
  std::vector<std::string> out(*count);
  for (uint32_t i = 0; i < *count; ++i) {
    auto rel = b.u32(off + c.header_size + size_t{i} * 4);
    if (!rel) return std::nullopt;
    size_t p = off + size_t{*start} + *rel;
    if (p >= end) continue;
    std::string s;
    if (utf8) {
      auto skip_len = [&](size_t& q) -> std::optional<size_t> {
        auto a = b.u8(q);
        if (!a) return std::nullopt;
        if (*a & 0x80) {
          auto bb = b.u8(q + 1);
          if (!bb) return std::nullopt;
          q += 2;
          return size_t(*a & 0x7f) << 8 | *bb;
        }
        q += 1;
        return size_t{*a};
      };
      if (!skip_len(p)) continue;         // length in UTF-16 units
      auto n = skip_len(p);               // length in bytes
      if (!n || p + *n > end || !b.has(p, *n)) continue;
      s.assign(reinterpret_cast<const char*>(b.d.data() + p), *n);
    } else {
      auto n16 = b.u16(p);
      if (!n16) continue;
      size_t n = *n16;
      p += 2;
      if (n & 0x8000) {
        auto lo = b.u16(p);
        if (!lo) continue;
        n = (n & 0x7fff) << 16 | *lo;
        p += 2;
      }
      if (p + n * 2 > end) continue;
      for (size_t k = 0; k < n && k < 4096; ++k) {
        uint32_t ch = *b.u16(p + k * 2);
        if (ch >= 0xd800 && ch < 0xdc00 && k + 1 < n) {  // surrogate pair
          const uint32_t lo = *b.u16(p + (k + 1) * 2);
          if (lo >= 0xdc00 && lo < 0xe000) {
            ch = 0x10000 + ((ch - 0xd800) << 10) + (lo - 0xdc00);
            ++k;
          }
        }
        append_utf8(s, ch);
      }
    }
    out[i] = std::move(s);
  }
  return out;
}

// Walks a binary XML document, calling `element(name, attrs)` for each start tag;
// attrs are (attribute resource id, dataType, data, raw string or empty).
struct XmlAttr {
  uint32_t res_id;
  uint8_t type;
  uint32_t data;
  std::string raw;
};
bool walk_xml(std::span<const uint8_t> data,
              const std::function<void(const std::string&, const std::vector<XmlAttr>&)>& element) {
  const Bytes b{data};
  auto root = chunk_at(b, 0);
  if (!root || root->type != kXml) return false;
  std::vector<std::string> strings;
  std::vector<uint32_t> resmap;
  size_t off = root->header_size;
  for (size_t n = 0; off < root->size && n < kMaxChunks; ++n) {
    auto c = chunk_at(b, off);
    if (!c) return false;
    if (c->type == kStringPool) {
      auto pool = string_pool(b, off, *c);
      if (!pool) return false;
      strings = std::move(*pool);
    } else if (c->type == kXmlResourceMap) {
      for (size_t p = off + c->header_size; p + 4 <= off + c->size && resmap.size() < kMaxStrings; p += 4) {
        resmap.push_back(*b.u32(p));
      }
    } else if (c->type == kXmlStartElement) {
      const size_t ext = off + c->header_size;
      auto name = b.u32(ext + 4);
      auto astart = b.u16(ext + 8), asize = b.u16(ext + 10), acount = b.u16(ext + 12);
      if (!name || !astart || !asize || !acount || *asize < 20) return false;
      std::vector<XmlAttr> attrs;
      for (uint16_t i = 0; i < *acount; ++i) {
        const size_t a = ext + *astart + size_t{i} * *asize;
        auto an = b.u32(a + 4), raw = b.u32(a + 8), dv = b.u32(a + 16);
        auto dt = b.u8(a + 15);
        if (!an || !raw || !dt || !dv || a + 20 > off + c->size) break;
        XmlAttr x{*an < resmap.size() ? resmap[*an] : 0, *dt, *dv, ""};
        if (*raw < strings.size()) x.raw = strings[*raw];
        attrs.push_back(std::move(x));
      }
      element(*name < strings.size() ? strings[*name] : std::string(), attrs);
    }
    off += c->size;
  }
  return true;
}

}  // namespace

std::optional<ManifestApp> parse_manifest(std::span<const uint8_t> axml) {
  std::optional<ManifestApp> app;
  const bool ok = walk_xml(axml, [&](const std::string& name, const std::vector<XmlAttr>& attrs) {
    if (name != "application" || app) return;
    ManifestApp a;
    std::optional<uint32_t> round;
    for (const auto& x : attrs) {
      if (x.res_id == kAttrIcon && x.type == kTypeReference) a.icon = x.data;
      if (x.res_id == kAttrRoundIcon && x.type == kTypeReference) round = x.data;
      if (x.res_id == kAttrLabel) {
        if (x.type == kTypeReference) a.label_res = x.data;
        else if (!x.raw.empty()) a.label = x.raw;
      }
    }
    if (!a.icon) a.icon = round;  // square icon first; the round one if that's all
    app = a;
  });
  if (!ok) return std::nullopt;
  return app;
}

std::optional<AdaptiveIcon> parse_adaptive_icon(std::span<const uint8_t> axml) {
  AdaptiveIcon icon;
  bool adaptive = false;
  const bool ok = walk_xml(axml, [&](const std::string& name, const std::vector<XmlAttr>& attrs) {
    if (name == "adaptive-icon") adaptive = true;
    for (const auto& x : attrs) {
      if (x.res_id != kAttrDrawable || x.type != kTypeReference) continue;
      if (name == "background") icon.background = x.data;
      if (name == "foreground") icon.foreground = x.data;
    }
  });
  if (!ok || !adaptive) return std::nullopt;
  return icon;
}

std::optional<ResourceTable> ResourceTable::parse(std::span<const uint8_t> arsc) {
  const Bytes b{arsc};
  auto root = chunk_at(b, 0);
  if (!root || root->type != kTable) return std::nullopt;
  ResourceTable t;
  size_t off = root->header_size;
  for (size_t n = 0; off < root->size && n < kMaxChunks; ++n) {
    auto c = chunk_at(b, off);
    if (!c) return std::nullopt;
    if (c->type == kStringPool && t.strings_.empty()) {
      auto pool = string_pool(b, off, *c);
      if (!pool) return std::nullopt;
      t.strings_ = std::move(*pool);
    } else if (c->type == kTablePackage) {
      auto pkg_id = b.u32(off + 8);
      if (!pkg_id || *pkg_id > 0xff) return std::nullopt;
      size_t p = off + c->header_size;
      for (size_t k = 0; p < off + c->size && k < kMaxChunks; ++k) {
        auto tc = chunk_at(b, p);
        if (!tc) break;
        if (tc->type == kTableType) {
          auto type_id = b.u8(p + 8), flags = b.u8(p + 9);
          auto count = b.u32(p + 12), start = b.u32(p + 16);
          auto density = b.u16(p + 20 + 14);
          auto lang = b.u16(p + 20 + 8);
          if (!type_id || !flags || !count || !start || !density || !lang) break;
          const bool sparse = *flags & 0x01, off16 = *flags & 0x02;
          const size_t table = p + tc->header_size;
          for (uint32_t i = 0; i < *count && t.entries_.size() < kMaxEntries; ++i) {
            uint32_t index = i;
            size_t rel;
            if (sparse) {
              auto idx = b.u16(table + size_t{i} * 4), o = b.u16(table + size_t{i} * 4 + 2);
              if (!idx || !o) break;
              index = *idx;
              rel = size_t{*o} * 4;
            } else if (off16) {
              auto o = b.u16(table + size_t{i} * 2);
              if (!o) break;
              if (*o == 0xffff) continue;
              rel = size_t{*o} * 4;
            } else {
              auto o = b.u32(table + size_t{i} * 4);
              if (!o) break;
              if (*o == 0xffffffff) continue;
              rel = *o;
            }
            const size_t e = p + size_t{*start} + rel;
            if (e >= p + tc->size) continue;
            auto esize = b.u16(e), eflags = b.u16(e + 2);
            if (!esize || !eflags) continue;
            uint8_t dtype;
            uint32_t data;
            if (*eflags & 0x0008) {  // compact entry: key16, flags (type in high byte), data
              auto d = b.u32(e + 4);
              if (!d) continue;
              dtype = static_cast<uint8_t>(*eflags >> 8);
              data = *d;
            } else if (*eflags & 0x0001) {
              continue;  // complex (bag) entries: styles and arrays, not needed
            } else {
              auto dt = b.u8(e + *esize + 3);
              auto d = b.u32(e + *esize + 4);
              if (!dt || !d || e + *esize + 8 > p + tc->size) continue;
              dtype = *dt;
              data = *d;
            }
            const uint32_t id = *pkg_id << 24 | uint32_t{*type_id} << 16 | (index & 0xffff);
            t.entries_.push_back({id, Entry{*density, *lang == 0, dtype, data}});
          }
        }
        p += tc->size;
      }
    }
    off += c->size;
  }
  std::stable_sort(t.entries_.begin(), t.entries_.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  return t;
}

std::optional<ResolvedValue> ResourceTable::resolve(uint32_t id, bool prefer_raster) const {
  for (int depth = 0; depth < 6; ++depth) {
    auto [lo, hi] = std::equal_range(entries_.begin(), entries_.end(), std::pair<uint32_t, Entry>{id, {}},
                                     [](const auto& a, const auto& b) { return a.first < b.first; });
    if (lo == hi) return std::nullopt;
    // Rank this id's configurations.
    auto score = [&](const Entry& e) {
      int s = e.default_locale ? 1 << 20 : 0;
      if (e.type == kTypeString && e.data < strings_.size()) {
        const std::string& str = strings_[e.data];
        const bool xml = str.ends_with(".xml");
        if (prefer_raster && !xml) s += 1 << 18;  // a real image beats an XML drawable
        const int dens = e.density == 0xffff || e.density == 0xfffe ? 0 : e.density;
        s += dens;
      }
      return s;
    };
    const Entry* best = &lo->second;
    for (auto it = lo; it != hi; ++it) {
      if (score(it->second) > score(*best)) best = &it->second;
    }
    ResolvedValue v;
    switch (best->type) {
      case kTypeReference:
        id = best->data;
        continue;
      case kTypeString:
        if (best->data >= strings_.size()) return std::nullopt;
        if (strings_[best->data].starts_with("res/")) v.path = strings_[best->data];
        else v.text = strings_[best->data];
        return v;
      case kTypeColorArgb8: v.color = best->data; return v;
      case kTypeColorRgb8: v.color = 0xff000000 | best->data; return v;
      case kTypeColorArgb4:
      case kTypeColorRgb4: {
        const uint32_t d = best->data;
        auto x = [&](int sh) { return ((d >> sh) & 0xf) * 0x11; };
        const uint32_t a = best->type == kTypeColorRgb4 ? 0xff : x(12);
        v.color = a << 24 | x(8) << 16 | x(4) << 8 | x(0);
        return v;
      }
      default: return std::nullopt;
    }
  }
  return std::nullopt;
}

}  // namespace kestrel
