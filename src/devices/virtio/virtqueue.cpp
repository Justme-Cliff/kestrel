#include "devices/virtio/virtqueue.h"

#include <atomic>
#include <cstring>
#include <format>

namespace kestrel {
namespace {

constexpr uint16_t kDescFNext = 1;
constexpr uint16_t kDescFWrite = 2;
constexpr uint16_t kDescFIndirect = 4;

template <typename T>
T* map_ring(const GuestMemory& mem, Gpa gpa, uint64_t len, uint64_t align) {
  if (gpa.value % align != 0) return nullptr;
  auto span = mem.translate(gpa, len);
  return span ? reinterpret_cast<T*>(span->data()) : nullptr;
}

}  // namespace

Result<void> Virtqueue::configure(const GuestMemory& memory, uint16_t size, Gpa desc, Gpa avail,
                                  Gpa used) {
  reset();
  if (size == 0 || size > kMaxSize || (size & (size - 1)) != 0) {
    return make_error(std::format("invalid virtqueue size {}", size));
  }
  // Alignments and sizes from virtio 1.2 §2.7 (split virtqueues).
  auto* d = map_ring<Desc>(memory, desc, 16ull * size, 16);
  auto* a = map_ring<uint16_t>(memory, avail, 6ull + 2ull * size, 2);
  auto* u = map_ring<uint16_t>(memory, used, 6ull + 8ull * size, 4);
  if (!d || !a || !u) return make_error("virtqueue rings outside guest RAM or misaligned");
  memory_ = &memory;
  size_ = size;
  desc_ = d;
  avail_ = a;
  used_hdr_ = u;
  used_ring_ = reinterpret_cast<UsedElem*>(u + 2);
  last_avail_ = 0;
  used_idx_ = 0;
  return {};
}

void Virtqueue::reset() {
  memory_ = nullptr;
  size_ = 0;
  desc_ = nullptr;
  avail_ = used_hdr_ = nullptr;
  used_ring_ = nullptr;
}

Result<std::optional<Virtqueue::Chain>> Virtqueue::pop() {
  if (!ready()) return std::nullopt;
  const uint16_t avail_idx = std::atomic_ref<uint16_t>(avail_[1]).load(std::memory_order_acquire);
  if (avail_idx == last_avail_) return std::nullopt;
  if (static_cast<uint16_t>(avail_idx - last_avail_) > size_) {
    return make_error("virtqueue avail index jumped past the ring size");
  }
  const uint16_t head = avail_[2 + last_avail_ % size_];
  ++last_avail_;
  if (head >= size_) return make_error(std::format("virtqueue head {} out of range", head));

  Chain chain{.head = head, .buffers = {}};
  uint16_t idx = head;
  // A chain can't be longer than the table; this also stops descriptor loops.
  for (unsigned count = 0;; ++count) {
    if (count >= size_) return make_error("virtqueue descriptor chain loops");
    Desc d;
    std::memcpy(&d, &desc_[idx], sizeof(d));  // the guest may change it under us: copy once
    if (d.flags & kDescFIndirect) return make_error("indirect descriptors not negotiated");
    auto data = memory_->translate(Gpa(d.addr), d.len);
    if (!data) return make_error(std::format("descriptor {:#x}+{} outside guest RAM", d.addr, d.len));
    chain.buffers.push_back({*data, (d.flags & kDescFWrite) != 0});
    if (!(d.flags & kDescFNext)) break;
    if (d.next >= size_) return make_error("descriptor next index out of range");
    idx = d.next;
  }
  return chain;
}

void Virtqueue::push_used(uint16_t head, uint32_t written) {
  if (!ready()) return;
  used_ring_[used_idx_ % size_] = UsedElem{head, written};
  ++used_idx_;
  std::atomic_ref<uint16_t>(used_hdr_[1]).store(used_idx_, std::memory_order_release);
}

ChainReader::ChainReader(const Virtqueue::Chain& chain) {
  for (const auto& b : chain.buffers) {
    if (!b.device_writable) parts_.push_back(b.data);
  }
}

size_t ChainReader::remaining() const {
  size_t n = 0;
  for (size_t i = part_; i < parts_.size(); ++i) n += parts_[i].size();
  return n - offset_;
}

std::span<uint8_t> ChainReader::next_segment(size_t max) {
  while (part_ < parts_.size() && offset_ == parts_[part_].size()) {
    ++part_;
    offset_ = 0;
  }
  if (part_ == parts_.size()) return {};
  const size_t n = std::min(max, parts_[part_].size() - offset_);
  auto seg = parts_[part_].subspan(offset_, n);
  offset_ += n;
  return seg;
}

bool ChainReader::read(std::span<uint8_t> out) {
  if (remaining() < out.size()) return false;
  size_t done = 0;
  while (done < out.size()) {
    auto seg = next_segment(out.size() - done);
    std::memcpy(out.data() + done, seg.data(), seg.size());
    done += seg.size();
  }
  return true;
}

ChainWriter::ChainWriter(const Virtqueue::Chain& chain) {
  for (const auto& b : chain.buffers) {
    if (b.device_writable) parts_.push_back(b.data);
  }
}

size_t ChainWriter::remaining() const {
  size_t n = 0;
  for (size_t i = part_; i < parts_.size(); ++i) n += parts_[i].size();
  return n - offset_;
}

std::span<uint8_t> ChainWriter::next_segment(size_t max) {
  while (part_ < parts_.size() && offset_ == parts_[part_].size()) {
    ++part_;
    offset_ = 0;
  }
  if (part_ == parts_.size()) return {};
  const size_t n = std::min(max, parts_[part_].size() - offset_);
  auto seg = parts_[part_].subspan(offset_, n);
  offset_ += n;
  written_ += static_cast<uint32_t>(n);
  return seg;
}

bool ChainWriter::write(std::span<const uint8_t> in) {
  if (remaining() < in.size()) return false;
  size_t done = 0;
  while (done < in.size()) {
    auto seg = next_segment(in.size() - done);
    std::memcpy(seg.data(), in.data() + done, seg.size());
    done += seg.size();
  }
  return true;
}

}  // namespace kestrel
