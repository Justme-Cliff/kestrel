#include "devices/virtio/virtio_input.h"

#include <array>
#include <cstring>
#include <vector>

#include "core/log.h"

namespace kestrel {
namespace {

enum : uint8_t {
  kCfgUnset = 0x00,
  kCfgIdName = 0x01,
  kCfgIdSerial = 0x02,
  kCfgIdDevids = 0x03,
  kCfgPropBits = 0x10,
  kCfgEvBits = 0x11,
  kCfgAbsInfo = 0x12,
};
// Past this backlog, frames that only move a finger are merged (latest position wins).
constexpr size_t kCompactAt = 256;
// Hard cap for a guest that doesn't read at all (driver not loaded yet).
constexpr size_t kMaxPending = 1 << 16;

std::vector<uint8_t> bitmap(const std::vector<uint32_t>& bits) {
  std::vector<uint8_t> b;
  for (uint32_t bit : bits) {
    if (bit / 8 >= 128) continue;
    if (b.size() <= bit / 8) b.resize(bit / 8 + 1, 0);
    b[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
  }
  return b;
}

}  // namespace

VirtioInput::Identity VirtioInput::touchscreen(uint32_t width, uint32_t height, unsigned slots) {
  using namespace input;
  Identity id;
  id.name = "Kestrel Touchscreen";
  id.product = 1;
  id.props = {kPropDirect};
  id.events[kEvKey] = {kBtnTouch};
  id.events[kEvAbs] = {kAbsX, kAbsY, kAbsMtSlot, kAbsMtPositionX, kAbsMtPositionY, kAbsMtTrackingId};
  const int32_t xmax = static_cast<int32_t>(width) - 1, ymax = static_cast<int32_t>(height) - 1;
  id.abs[kAbsX] = {0, xmax, 0, 0, 0};
  id.abs[kAbsY] = {0, ymax, 0, 0, 0};
  id.abs[kAbsMtPositionX] = {0, xmax, 0, 0, 0};
  id.abs[kAbsMtPositionY] = {0, ymax, 0, 0, 0};
  id.abs[kAbsMtSlot] = {0, static_cast<int32_t>(slots) - 1, 0, 0, 0};
  id.abs[kAbsMtTrackingId] = {0, 65535, 0, 0, 0};
  return id;
}

VirtioInput::Identity VirtioInput::keyboard() {
  Identity id;
  id.name = "Kestrel Keyboard";
  id.product = 2;
  auto& keys = id.events[input::kEvKey];
  for (uint16_t k = 1; k < 256; ++k) keys.push_back(k);
  return id;
}

VirtioInput::VirtioInput(Identity identity) : id_(std::move(identity)) {}

void VirtioInput::refresh_config_locked() {
  payload_.clear();
  switch (select_) {
    case kCfgIdName: payload_.assign(id_.name.begin(), id_.name.end()); break;
    case kCfgIdSerial: payload_ = {'k', 's', 't', 'l'}; break;
    case kCfgIdDevids: {
      const uint16_t ids[4] = {0x06 /*BUS_VIRTUAL*/, 0x4b53, id_.product, 1};
      payload_.resize(8);
      std::memcpy(payload_.data(), ids, 8);
      break;
    }
    case kCfgPropBits: payload_ = bitmap(id_.props); break;
    case kCfgEvBits:
      if (auto it = id_.events.find(subsel_); it != id_.events.end()) {
        payload_ = bitmap(std::vector<uint32_t>(it->second.begin(), it->second.end()));
      }
      break;
    case kCfgAbsInfo:
      if (auto it = id_.abs.find(subsel_); it != id_.abs.end()) {
        payload_.resize(20);
        std::memcpy(payload_.data(), &it->second, 20);
      }
      break;
    default: break;
  }
  if (payload_.size() > 128) payload_.resize(128);
}

void VirtioInput::read_config(uint64_t offset, std::span<uint8_t> out) {
  // struct virtio_input_config: select u8, subsel u8, size u8, reserved[5], u[128]
  std::lock_guard lock(mutex_);
  for (size_t i = 0; i < out.size(); ++i) {
    const uint64_t o = offset + i;
    uint8_t v = 0;
    if (o == 0) v = select_;
    else if (o == 1) v = subsel_;
    else if (o == 2) v = static_cast<uint8_t>(payload_.size());
    else if (o >= 8 && o - 8 < payload_.size()) v = payload_[o - 8];
    out[i] = v;
  }
}

void VirtioInput::write_config(uint64_t offset, std::span<const uint8_t> in) {
  std::lock_guard lock(mutex_);
  for (size_t i = 0; i < in.size(); ++i) {
    if (offset + i == 0) select_ = in[i];
    if (offset + i == 1) subsel_ = in[i];
  }
  refresh_config_locked();
}

void VirtioInput::reset() {
  std::lock_guard lock(mutex_);
  pending_.clear();
}

void VirtioInput::send(std::span<const InputEvent> events) {
  {
    std::lock_guard lock(mutex_);
    if (pending_.size() + events.size() > kCompactAt) compact_backlog(pending_);
    if (pending_.size() + events.size() > kMaxPending) return;  // guest isn't reading
    pending_.insert(pending_.end(), events.begin(), events.end());
  }
  flush();
}

void VirtioInput::compact_backlog(std::deque<InputEvent>& q) {
  using namespace input;
  // Split into frames (each ends with SYN_REPORT); a trailing unfinished frame stays.
  struct Frame {
    size_t begin, end;
  };
  std::vector<Frame> frames;
  size_t start = 0;
  for (size_t i = 0; i < q.size(); ++i) {
    if (q[i].type == kEvSyn && q[i].code == kSynReport) {
      frames.push_back({start, i + 1});
      start = i + 1;
    }
  }
  if (frames.size() < 3) return;
  // Walk from the newest frame back. A frame that only moves one finger can go if
  // that same finger (same contact) moves again later.
  std::vector<bool> keep(frames.size(), true);
  std::array<bool, 64> moved_later{};
  for (size_t f = frames.size(); f-- > 1;) {  // frame 0 may be half sent: never drop
    int slot = 0, slots = 0;
    bool moves_only = true, moves = false;
    for (size_t i = frames[f].begin; i < frames[f].end; ++i) {
      const InputEvent& e = q[i];
      if (e.type == kEvSyn) continue;
      if (e.type == kEvAbs && e.code == kAbsMtSlot) {
        slot = e.value;
        ++slots;
      } else if (e.type == kEvAbs && (e.code == kAbsMtPositionX || e.code == kAbsMtPositionY)) {
        moves = true;
      } else if (e.type == kEvAbs && (e.code == kAbsX || e.code == kAbsY)) {
        // single-touch copy of the same move
      } else {
        moves_only = false;
        // A press or lift: older moves belong to a different contact (or are its last
        // position before lifting), so they must stay.
        if (e.type == kEvAbs && e.code == kAbsMtTrackingId && slot >= 0 && slot < 64) moved_later[slot] = false;
      }
    }
    if (!moves_only || !moves || slots != 1 || slot < 0 || slot >= 64) continue;
    if (moved_later[slot]) keep[f] = false;
    moved_later[slot] = true;
  }
  std::deque<InputEvent> out;
  for (size_t f = 0; f < frames.size(); ++f) {
    if (keep[f]) out.insert(out.end(), q.begin() + frames[f].begin, q.begin() + frames[f].end);
  }
  out.insert(out.end(), q.begin() + start, q.end());
  q.swap(out);
}

void VirtioInput::queue_notify(unsigned queue) {
  if (queue == 0) {
    flush();  // new event buffers
    return;
  }
  // Status queue (LEDs etc.): accept and discard.
  access_->with_queue(1, [&](Virtqueue& vq) {
    while (true) {
      auto chain = vq.pop();
      if (!chain || !*chain) return;
      vq.push_used((*chain)->head, 0);
    }
  });
  access_->signal_used();
}

void VirtioInput::flush() {
  std::lock_guard lock(mutex_);
  bool used = false;
  access_->with_queue(0, [&](Virtqueue& vq) {
    while (!pending_.empty()) {
      auto chain = vq.pop();
      if (!chain || !*chain) return;
      ChainWriter out(**chain);
      const InputEvent& e = pending_.front();
      uint8_t buf[8];  // struct virtio_input_event: type le16, code le16, value le32
      std::memcpy(buf, &e.type, 2);
      std::memcpy(buf + 2, &e.code, 2);
      std::memcpy(buf + 4, &e.value, 4);
      out.write(buf);
      vq.push_used((*chain)->head, out.written());
      pending_.pop_front();
      used = true;
    }
  });
  if (used) access_->signal_used();
}

}  // namespace kestrel
