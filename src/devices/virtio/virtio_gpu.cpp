#include "devices/virtio/virtio_gpu.h"

#include <cstring>

#include "core/log.h"

namespace kestrel {
namespace {

// Command and response types (§5.7.6.7).
enum : uint32_t {
  kCmdGetDisplayInfo = 0x0100,
  kCmdResourceCreate2d = 0x0101,
  kCmdResourceUnref = 0x0102,
  kCmdSetScanout = 0x0103,
  kCmdResourceFlush = 0x0104,
  kCmdTransferToHost2d = 0x0105,
  kCmdResourceAttachBacking = 0x0106,
  kCmdResourceDetachBacking = 0x0107,
  kCmdUpdateCursor = 0x0300,
  kCmdMoveCursor = 0x0301,

  kRespOkNodata = 0x1100,
  kRespOkDisplayInfo = 0x1101,
  kRespErrUnspec = 0x1200,
  kRespErrOutOfMemory = 0x1201,
  kRespErrInvalidScanoutId = 0x1202,
  kRespErrInvalidResourceId = 0x1203,
  kRespErrInvalidParameter = 0x1205,
};

constexpr uint32_t kFlagFence = 1;
constexpr size_t kHdrSize = 24;  // struct virtio_gpu_ctrl_hdr
constexpr uint32_t kMaxScanouts = 16;
constexpr uint32_t kMaxBackingEntries = 16384;
constexpr uint32_t kMaxDimension = 16384;

// Formats we accept (all 32 bits per pixel). BGRA-order ones map to Frame::bgra.
bool format_supported(uint32_t f, bool& bgra) {
  switch (f) {
    case 1: case 2: case 3: case 4: bgra = true; return true;           // B8G8R8A8/X8, A8R8G8B8/X8
    case 67: case 68: case 121: case 134: bgra = false; return true;    // R8G8B8A8 family
    default: return false;
  }
}

uint32_t rd32(std::span<const uint8_t> b, size_t off) {
  uint32_t v = 0;
  if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
  return v;
}
uint64_t rd64(std::span<const uint8_t> b, size_t off) {
  uint64_t v = 0;
  if (off + 8 <= b.size()) std::memcpy(&v, b.data() + off, 8);
  return v;
}
void wr32(std::vector<uint8_t>& b, size_t off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); }

struct Rect {
  uint32_t x, y, w, h;
  bool inside(uint32_t width, uint32_t height) const {
    return x <= width && y <= height && w <= width - x && h <= height - y;
  }
};
Rect rect_at(std::span<const uint8_t> b, size_t off) {
  return {rd32(b, off), rd32(b, off + 4), rd32(b, off + 8), rd32(b, off + 12)};
}

// Request sizes, including the header (§5.7.6.7).
size_t request_size(uint32_t type) {
  switch (type) {
    case kCmdGetDisplayInfo: return kHdrSize;
    case kCmdResourceCreate2d: return kHdrSize + 16;
    case kCmdResourceUnref: return kHdrSize + 8;
    case kCmdSetScanout: return kHdrSize + 24;
    case kCmdResourceFlush: return kHdrSize + 24;
    case kCmdTransferToHost2d: return kHdrSize + 32;
    case kCmdResourceAttachBacking: return kHdrSize + 8;
    case kCmdResourceDetachBacking: return kHdrSize + 8;
    case kCmdUpdateCursor: case kCmdMoveCursor: return kHdrSize + 32;
    default: return kHdrSize;
  }
}

}  // namespace

VirtioGpu::VirtioGpu(const GuestMemory& memory, uint32_t width, uint32_t height, DisplaySink* sink)
    : memory_(memory), width_(width), height_(height), sink_(sink) {}

void VirtioGpu::read_config(uint64_t offset, std::span<uint8_t> out) {
  // events_read, events_clear, num_scanouts, num_capsets (§5.7.4)
  uint32_t config[4] = {events_read_, 0, 1, 0};
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = offset + i < sizeof(config) ? reinterpret_cast<uint8_t*>(config)[offset + i] : 0;
  }
}

void VirtioGpu::write_config(uint64_t offset, std::span<const uint8_t> in) {
  if (offset == 4 && in.size() == 4) events_read_ &= ~rd32(in, 0);  // events_clear
}

void VirtioGpu::reset() {
  std::lock_guard lock(mutex_);
  resources_.clear();
  total_bytes_ = 0;
  scanout_resource_ = 0;
}

void VirtioGpu::queue_notify(unsigned queue) {
  bool used = false, broken = false;
  access_->with_queue(queue, [&](Virtqueue& vq) {
    while (true) {
      auto chain = vq.pop();
      if (!chain) {
        log(LogLevel::kWarn, "virtio-gpu: {}", chain.error().message);
        broken = true;
        return;
      }
      if (!*chain) return;
      ChainReader in(**chain);
      ChainWriter out(**chain);
      std::vector<uint8_t> hdr(kHdrSize);
      std::vector<uint8_t> response;
      if (in.read(hdr)) {
        const uint32_t type = rd32(hdr, 0);
        std::vector<uint8_t> req(hdr);
        req.resize(std::max(request_size(type), kHdrSize));
        if (!in.read(std::span(req).subspan(kHdrSize))) {
          response.assign(kHdrSize, 0);
          wr32(response, 0, kRespErrInvalidParameter);
        } else if (queue == 1) {
          response.clear();  // cursor queue: no hardware cursor yet, nothing to answer
        } else {
          response = handle(req, in);
        }
        if (!response.empty()) {
          // Echo the fence so the guest's waiters complete (we finish synchronously).
          if (rd32(hdr, 4) & kFlagFence) {
            wr32(response, 4, kFlagFence);
            std::memcpy(response.data() + 8, hdr.data() + 8, 8);   // fence_id
            std::memcpy(response.data() + 16, hdr.data() + 16, 8); // ctx_id, ring_idx
          }
          out.write(std::span(response).first(std::min(response.size(), out.remaining())));
        }
      }
      vq.push_used((*chain)->head, out.written());
      used = true;
    }
  });
  if (broken) access_->set_needs_reset();
  if (used) access_->signal_used();
}

std::vector<uint8_t> VirtioGpu::handle(std::span<const uint8_t> req, ChainReader& extra) {
  std::vector<uint8_t> resp(kHdrSize, 0);
  uint32_t result = kRespOkNodata;
  const uint32_t type = rd32(req, 0);
  std::lock_guard lock(mutex_);
  switch (type) {
    case kCmdGetDisplayInfo: {
      resp.resize(kHdrSize + kMaxScanouts * 24, 0);
      wr32(resp, kHdrSize + 8, width_);
      wr32(resp, kHdrSize + 12, height_);
      wr32(resp, kHdrSize + 16, 1);  // enabled
      result = kRespOkDisplayInfo;
      break;
    }
    case kCmdResourceCreate2d: {
      const uint32_t id = rd32(req, kHdrSize), format = rd32(req, kHdrSize + 4);
      const uint32_t w = rd32(req, kHdrSize + 8), h = rd32(req, kHdrSize + 12);
      bool bgra;
      const uint64_t bytes = uint64_t(w) * h * 4;
      if (id == 0 || resources_.contains(id)) {
        result = kRespErrInvalidResourceId;
      } else if (!format_supported(format, bgra) || w == 0 || h == 0 || w > kMaxDimension ||
                 h > kMaxDimension) {
        result = kRespErrInvalidParameter;
      } else if (bytes > kMaxResourceBytes || total_bytes_ + bytes > kMaxTotalBytes) {
        result = kRespErrOutOfMemory;
      } else {
        Resource r{format, w, h, std::vector<uint8_t>(bytes, 0), {}, 0};
        resources_.emplace(id, std::move(r));
        total_bytes_ += bytes;
      }
      break;
    }
    case kCmdResourceUnref: {
      const uint32_t id = rd32(req, kHdrSize);
      auto it = resources_.find(id);
      if (it == resources_.end()) {
        result = kRespErrInvalidResourceId;
      } else {
        total_bytes_ -= it->second.pixels.size();
        if (scanout_resource_ == id) scanout_resource_ = 0;
        resources_.erase(it);
      }
      break;
    }
    case kCmdSetScanout: {
      const uint32_t scanout = rd32(req, kHdrSize + 16), id = rd32(req, kHdrSize + 20);
      if (scanout != 0) {
        result = kRespErrInvalidScanoutId;
      } else if (id != 0 && !resources_.contains(id)) {
        result = kRespErrInvalidResourceId;
      } else {
        const Rect r = rect_at(req, kHdrSize);
        auto res = resources_.find(id);
        if (id != 0 && (!r.inside(res->second.width, res->second.height) || r.w == 0 || r.h == 0)) {
          result = kRespErrInvalidParameter;
          break;
        }
        scanout_resource_ = id;
        scanout_x_ = r.x;
        scanout_y_ = r.y;
        scanout_w_ = r.w;
        scanout_h_ = r.h;
      }
      break;
    }
    case kCmdResourceFlush: result = flush(req); break;
    case kCmdTransferToHost2d: result = transfer_to_host(req); break;
    case kCmdResourceAttachBacking: result = attach_backing(req, extra); break;
    case kCmdResourceDetachBacking: {
      auto it = resources_.find(rd32(req, kHdrSize));
      if (it == resources_.end()) {
        result = kRespErrInvalidResourceId;
      } else {
        it->second.backing.clear();
        it->second.backing_size = 0;
      }
      break;
    }
    default:
      log(LogLevel::kDebug, "virtio-gpu: unsupported command {:#x}", type);
      result = kRespErrUnspec;
      break;
  }
  wr32(resp, 0, result);
  return resp;
}

uint32_t VirtioGpu::attach_backing(std::span<const uint8_t> req, ChainReader& extra) {
  auto it = resources_.find(rd32(req, kHdrSize));
  if (it == resources_.end()) return kRespErrInvalidResourceId;
  const uint32_t n = rd32(req, kHdrSize + 4);
  if (n == 0 || n > kMaxBackingEntries) return kRespErrInvalidParameter;
  std::vector<std::span<uint8_t>> backing;
  uint64_t total = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t entry[16];  // struct virtio_gpu_mem_entry: addr u64, length u32, padding u32
    if (!extra.read(entry)) return kRespErrInvalidParameter;
    uint64_t addr;
    uint32_t len;
    std::memcpy(&addr, entry, 8);
    std::memcpy(&len, entry + 8, 4);
    auto span = memory_.translate(Gpa(addr), len);
    if (!span) return kRespErrInvalidParameter;
    backing.push_back(*span);
    total += len;
  }
  it->second.backing = std::move(backing);
  it->second.backing_size = total;
  return kRespOkNodata;
}

uint32_t VirtioGpu::transfer_to_host(std::span<const uint8_t> req) {
  const Rect r = rect_at(req, kHdrSize);
  const uint64_t offset = rd64(req, kHdrSize + 16);
  auto it = resources_.find(rd32(req, kHdrSize + 24));
  if (it == resources_.end()) return kRespErrInvalidResourceId;
  Resource& res = it->second;
  if (!r.inside(res.width, res.height) || res.backing.empty()) return kRespErrInvalidParameter;
  const uint64_t stride = uint64_t(res.width) * 4;

  // Walk the scatter list once per row; rows are usually page-sized pieces.
  auto copy_from_backing = [&](uint64_t src, uint8_t* dst, uint64_t len) -> bool {
    if (src > res.backing_size || len > res.backing_size - src) return false;
    uint64_t base = 0;
    for (const auto& seg : res.backing) {
      if (len == 0) break;
      if (src < base + seg.size()) {
        const uint64_t in_seg = src - base;
        const uint64_t n = std::min<uint64_t>(len, seg.size() - in_seg);
        std::memcpy(dst, seg.data() + in_seg, n);
        dst += n;
        src += n;
        len -= n;
      }
      base += seg.size();
    }
    return len == 0;
  };

  for (uint32_t row = 0; row < r.h; ++row) {
    const uint64_t src = offset + uint64_t(row) * stride;
    uint8_t* dst = res.pixels.data() + (uint64_t(r.y) + row) * stride + uint64_t(r.x) * 4;
    if (!copy_from_backing(src, dst, uint64_t(r.w) * 4)) return kRespErrInvalidParameter;
  }
  return kRespOkNodata;
}

uint32_t VirtioGpu::flush(std::span<const uint8_t> req) {
  const uint32_t id = rd32(req, kHdrSize + 16);
  auto it = resources_.find(id);
  if (it == resources_.end()) return kRespErrInvalidResourceId;
  if (id == scanout_resource_ && sink_) {
    const Resource& res = it->second;
    bool bgra = true;
    format_supported(res.format, bgra);
    // Only the scanout rectangle is visible; the resource can be larger (e.g. aligned height).
    const size_t stride = size_t(res.width) * 4;
    const size_t start = size_t(scanout_y_) * stride + size_t(scanout_x_) * 4;
    const size_t len = size_t(scanout_h_ - 1) * stride + size_t(scanout_w_) * 4;
    sink_->present(Frame{scanout_w_, scanout_h_, static_cast<uint32_t>(stride), bgra,
                         std::span<const uint8_t>(res.pixels).subspan(start, len)});
  }
  return kRespOkNodata;
}

}  // namespace kestrel
