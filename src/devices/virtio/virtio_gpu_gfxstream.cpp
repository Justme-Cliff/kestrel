#include "devices/virtio/virtio_gpu_gfxstream.h"
#include "core/thread_priority.h"

#include <gfxstream/virtio-gpu-gfxstream-renderer.h>
#include <gfxstream/virtio-gpu-gfxstream-renderer-unstable.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <future>
#include <map>

#include "core/gpa.h"
#include "devices/virtio/edid.h"
#include "core/log.h"

namespace kestrel {
namespace {

enum : uint32_t {
  kCmdGetDisplayInfo = 0x0100,
  kCmdResourceCreate2d = 0x0101,
  kCmdResourceUnref = 0x0102,
  kCmdSetScanout = 0x0103,
  kCmdResourceFlush = 0x0104,
  kCmdTransferToHost2d = 0x0105,
  kCmdResourceAttachBacking = 0x0106,
  kCmdResourceDetachBacking = 0x0107,
  kCmdGetCapsetInfo = 0x0108,
  kCmdGetCapset = 0x0109,
  kCmdResourceCreateBlob = 0x010c,
  kCmdSetScanoutBlob = 0x010d,
  kCmdGetEdid = 0x010a,
  kCmdCtxCreate = 0x0200,
  kCmdCtxDestroy = 0x0201,
  kCmdCtxAttachResource = 0x0202,
  kCmdCtxDetachResource = 0x0203,
  kCmdResourceCreate3d = 0x0204,
  kCmdTransferToHost3d = 0x0205,
  kCmdTransferFromHost3d = 0x0206,
  kCmdSubmit3d = 0x0207,
  kCmdResourceMapBlob = 0x0208,
  kCmdResourceUnmapBlob = 0x0209,
  kCmdUpdateCursor = 0x0300,
  kCmdMoveCursor = 0x0301,

  kRespOkNodata = 0x1100,
  kRespOkDisplayInfo = 0x1101,
  kRespOkCapsetInfo = 0x1102,
  kRespOkCapset = 0x1103,
  kRespOkMapInfo = 0x1106,
  kRespOkEdid = 0x1104,
  kRespErrUnspec = 0x1200,
  kRespErrOutOfMemory = 0x1201,
  kRespErrInvalidScanoutId = 0x1202,
  kRespErrInvalidResourceId = 0x1203,
  kRespErrInvalidContextId = 0x1204,
  kRespErrInvalidParameter = 0x1205,
};

constexpr uint32_t kFlagFence = 1;
constexpr uint32_t kFlagInfoRingIdx = 2;
constexpr size_t kHdrSize = 24;
constexpr uint32_t kMaxScanouts = 16;
constexpr uint32_t kMaxEntries = 1 << 20;
constexpr uint64_t kHostPage = 16 * 1024;

// Capsets gfxstream offers (Linux include/uapi/linux/virtio_gpu.h).
constexpr uint32_t kCapsets[] = {3 /*gfxstream-vulkan*/, 8 /*gfxstream-gles*/, 9 /*gfxstream-composer*/};

constexpr uint64_t kFeatureVirgl = 1ull << 0;
constexpr uint64_t kFeatureEdid = 1ull << 1;  // virtio 1.2 §5.7.3: display modes from an EDID
constexpr uint64_t kFeatureResourceBlob = 1ull << 3;
constexpr uint64_t kFeatureContextInit = 1ull << 4;

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

size_t request_size(uint32_t type) {
  switch (type) {
    case kCmdResourceCreate2d: return kHdrSize + 16;
    case kCmdResourceUnref: return kHdrSize + 8;
    case kCmdSetScanout: return kHdrSize + 24;
    case kCmdResourceFlush: return kHdrSize + 24;
    case kCmdTransferToHost2d: return kHdrSize + 32;
    case kCmdResourceAttachBacking: return kHdrSize + 8;
    case kCmdResourceDetachBacking: return kHdrSize + 8;
    case kCmdGetCapsetInfo: return kHdrSize + 8;
    case kCmdGetCapset: return kHdrSize + 8;
    case kCmdGetEdid: return kHdrSize + 8;
    case kCmdResourceCreateBlob: return kHdrSize + 32;
    case kCmdSetScanoutBlob: return kHdrSize + 72;
    case kCmdCtxCreate: return kHdrSize + 72;
    case kCmdCtxAttachResource: case kCmdCtxDetachResource: return kHdrSize + 8;
    case kCmdResourceCreate3d: return kHdrSize + 48;
    case kCmdTransferToHost3d: case kCmdTransferFromHost3d: return kHdrSize + 48;
    case kCmdSubmit3d: return kHdrSize + 8;
    case kCmdResourceMapBlob: return kHdrSize + 16;
    case kCmdResourceUnmapBlob: return kHdrSize + 8;
    case kCmdUpdateCursor: case kCmdMoveCursor: return kHdrSize + 32;
    default: return kHdrSize;
  }
}

bool is_bgra(uint32_t format) { return format >= 1 && format <= 4; }

VirtioGpuGfxstream* g_instance = nullptr;  // gfxstream's callbacks are process-wide

void fence_callback(void* /*user*/, stream_renderer_fence* f) {
  if (g_instance) {
    g_instance->on_fence(f->fence_id, f->ctx_id, f->ring_idx,
                         (f->flags & STREAM_RENDERER_FLAG_FENCE_RING_IDX) != 0);
  }
}

void debug_callback(void* /*user*/, stream_renderer_debug* d) {
  const LogLevel level = d->debug_type == STREAM_RENDERER_DEBUG_ERROR  ? LogLevel::kError
                         : d->debug_type == STREAM_RENDERER_DEBUG_WARN ? LogLevel::kWarn
                         : d->debug_type == STREAM_RENDERER_DEBUG_INFO ? LogLevel::kDebug
                                                                        : LogLevel::kDebug;
  const std::string_view msg = d->message ? d->message : "";
  // Display surface/swapchain changes matter when diagnosing crashes: keep them.
  const bool display = msg.find("wapchain") != std::string_view::npos || msg.find("surface") != std::string_view::npos;
  log(display && level == LogLevel::kDebug ? LogLevel::kInfo : level, "gfxstream: {}", msg);
}

}  // namespace

Result<std::unique_ptr<VirtioGpuGfxstream>> VirtioGpuGfxstream::create(const GuestMemory& memory,
                                                                      Options options) {
  if (g_instance) return make_error("only one gfxstream GPU per process");
  // MoltenVK can't compile some of gfxstream's compositor shaders with Metal argument
  // buffers; the classic binding model works.
  setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0);
  // Games compile many shaders when a match loads: let Metal use every core for it
  // (fewer hitches as new effects first appear).
  setenv("MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "1", 0);
  // ANGLE on its Metal backend: its OpenGL backend sits on Apple's deprecated GL
  // driver, which hangs under games.
  setenv("ANGLE_DEFAULT_PLATFORM", "metal", 0);
  // No MTLSharedEvent GPU-side waits: with several GL contexts on one Metal queue a
  // wait can be committed before its signal and stall the queue for good.
  setenv("ANGLE_FEATURE_OVERRIDES_DISABLED", "hasEvents", 0);
  // Host GLES: guest OpenGL ES runs on ANGLE's Metal backend on the Mac (how the
  // Android Emulator runs Unity games). Otherwise Vulkan only: guest GLES on guest
  // ANGLE over Vulkan/MoltenVK. The Vulkan path can also present into our window
  // directly (native swapchain); until a window is attached we read frames back.
  const uint64_t flags = options.host_gles
                             ? (STREAM_RENDERER_FLAGS_USE_EGL_BIT | STREAM_RENDERER_FLAGS_USE_GLES_BIT |
                                STREAM_RENDERER_FLAGS_USE_VK_BIT | STREAM_RENDERER_FLAGS_USE_SURFACELESS_BIT)
                             : (STREAM_RENDERER_FLAGS_USE_VK_BIT | STREAM_RENDERER_FLAGS_USE_SURFACELESS_BIT |
                                STREAM_RENDERER_FLAGS_VULKAN_NATIVE_SWAPCHAIN_BIT);
  stream_renderer_param params[] = {
      {STREAM_RENDERER_PARAM_USER_DATA, 0},
      {STREAM_RENDERER_PARAM_RENDERER_FLAGS, flags},
      {STREAM_RENDERER_PARAM_FENCE_CALLBACK, reinterpret_cast<uint64_t>(&fence_callback)},
      {STREAM_RENDERER_PARAM_DEBUG_CALLBACK, reinterpret_cast<uint64_t>(&debug_callback)},
      {STREAM_RENDERER_PARAM_WIN0_WIDTH, options.width},
      {STREAM_RENDERER_PARAM_WIN0_HEIGHT, options.height},
  };
  auto dev = std::unique_ptr<VirtioGpuGfxstream>(new VirtioGpuGfxstream(memory, std::move(options)));
  g_instance = dev.get();
  if (const int r = stream_renderer_init(params, std::size(params)); r != 0) {
    g_instance = nullptr;
    return make_error(std::format("gfxstream failed to start ({}): no usable Vulkan GPU?", r));
  }
  log(LogLevel::kInfo, "GPU acceleration: gfxstream on Metal ({})",
      dev->options_.host_gles ? "OpenGL ES via ANGLE, Vulkan via MoltenVK" : "Vulkan via MoltenVK");
  dev->thread_ = std::thread([d = dev.get()] { d->worker(); });
  dev->watchdog_ = std::thread([d = dev.get()] { d->watchdog(); });
  return dev;
}

VirtioGpuGfxstream::VirtioGpuGfxstream(const GuestMemory& memory, Options options)
    : memory_(memory), options_(std::move(options)) {}

VirtioGpuGfxstream::~VirtioGpuGfxstream() {
  {
    std::lock_guard lock(work_mutex_);
    stop_ = true;
  }
  work_cv_.notify_all();
  {
    std::lock_guard lock(watchdog_mutex_);
    watchdog_stop_ = true;
  }
  watchdog_cv_.notify_all();
  if (watchdog_.joinable()) watchdog_.join();
  if (thread_.joinable()) thread_.join();
  stream_renderer_teardown();
  g_instance = nullptr;
}

uint64_t VirtioGpuGfxstream::features() const {
  return kFeatureVirgl | kFeatureEdid | kFeatureResourceBlob | kFeatureContextInit;
}

void VirtioGpuGfxstream::read_config(uint64_t offset, std::span<uint8_t> out) {
  uint32_t config[4] = {events_read_, 0, 1, static_cast<uint32_t>(std::size(kCapsets))};
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = offset + i < sizeof(config) ? reinterpret_cast<uint8_t*>(config)[offset + i] : 0;
  }
}

void VirtioGpuGfxstream::write_config(uint64_t offset, std::span<const uint8_t> in) {
  if (offset == 4 && in.size() == 4) events_read_ &= ~rd32(in, 0);
}

void VirtioGpuGfxstream::reset() {
  std::lock_guard lock(mutex_);
  for (auto& [id, m] : mappings_) options_.unmap(m.gpa, m.size);
  mappings_.clear();
  for (auto& [id, r] : resources_) stream_renderer_resource_unref(id);
  resources_.clear();
  {
    std::lock_guard fences(fence_mutex_);
    pending_.clear();
  }
  {
    std::lock_guard work(work_mutex_);
    work_.clear();
  }
  scanout_ = {};
}

void VirtioGpuGfxstream::queue_notify(unsigned queue) {
  // Pop under the transport lock and queue in the same step, so ring order is kept
  // even when several vCPUs notify at once.
  bool broken = false;
  bool any = false;
  access_->with_queue(queue, [&](Virtqueue& vq) {
    std::lock_guard work(work_mutex_);
    while (true) {
      auto chain = vq.pop();
      if (!chain) {
        log(LogLevel::kWarn, "virtio-gpu: {}", chain.error().message);
        broken = true;
        return;
      }
      if (!*chain) return;
      work_.emplace_back(queue, std::move(**chain));
      any = true;
    }
  });
  if (broken) access_->set_needs_reset();
  if (any) work_cv_.notify_one();
}

void VirtioGpuGfxstream::worker() {
  // gfxstream render threads started from here inherit this QoS.
  set_thread_role(ThreadRole::kInteractive);
  std::unique_lock lock(work_mutex_);
  while (true) {
    work_cv_.wait(lock, [&] { return stop_ || !work_.empty() || !tasks_.empty(); });
    if (stop_) return;
    const auto now_ns = [] { return std::chrono::steady_clock::now().time_since_epoch().count(); };
    if (!tasks_.empty()) {
      auto task = std::move(tasks_.front());
      tasks_.pop_front();
      lock.unlock();
      busy_since_ = now_ns();
      task();
      busy_since_ = 0;
      lock.lock();
      continue;
    }
    auto [queue, chain] = std::move(work_.front());
    work_.pop_front();
    lock.unlock();
    busy_since_ = now_ns();
    process(queue, chain);
    busy_since_ = 0;
    lock.lock();
  }
}

void VirtioGpuGfxstream::watchdog() {
  using clock = std::chrono::steady_clock;
  std::optional<clock::time_point> stalled_at;
  std::unique_lock lock(watchdog_mutex_);
  while (!watchdog_cv_.wait_for(lock, std::chrono::seconds(1), [&] { return watchdog_stop_; })) {
    lock.unlock();
    const auto now = clock::now();
    const auto limit = std::chrono::seconds(kStallSeconds);
    std::string what;
    if (const int64_t since = busy_since_; since != 0) {
      if (now - clock::time_point(clock::duration(since)) > limit) {
        what = std::format("a graphics command has been running for over {} s", kStallSeconds);
      }
    }
    if (what.empty()) {
      std::lock_guard fences(fence_mutex_);
      for (const auto& p : pending_) {
        if (now - p.created > limit) {
          what = std::format("the Mac's GPU hasn't finished work for context {} in over {} s", p.ctx_id, kStallSeconds);
          break;
        }
      }
    }
    if (!what.empty() && !stalled_at) {
      stalled_at = now;
      log(LogLevel::kWarn, "virtio-gpu: stalled: {}", what);
      if (options_.on_stall) options_.on_stall(what);
    } else if (what.empty() && stalled_at) {
      const double secs = std::chrono::duration<double>(now - *stalled_at).count() + kStallSeconds;
      stalled_at.reset();
      log(LogLevel::kInfo, "virtio-gpu: moving again after about {:.0f} s", secs);
      if (options_.on_recover) options_.on_recover(secs);
    }
    lock.lock();
  }
}

void VirtioGpuGfxstream::process(unsigned queue, const Virtqueue::Chain& chain) {
  ChainReader in(chain);
  std::vector<uint8_t> hdr(kHdrSize);
  std::optional<std::vector<uint8_t>> response;
  if (!in.read(hdr) || queue == 1) {
    response = std::vector<uint8_t>{};  // malformed, or cursor queue (no hardware cursor)
  } else {
    const uint32_t type = rd32(hdr, 0);
    std::vector<uint8_t> req(hdr);
    req.resize(std::max(request_size(type), kHdrSize));
    if (!in.read(std::span(req).subspan(kHdrSize))) {
      response = std::vector<uint8_t>(kHdrSize, 0);
      wr32(*response, 0, kRespErrInvalidParameter);
    } else {
      const auto t0 = std::chrono::steady_clock::now();
      response = handle(req, in, chain);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (ms > 12) log(LogLevel::kDebug, "gpu: command {:#x} took {:.0f} ms on the GPU thread", type, ms);
    }
  }
  if (!response) return;  // answered when its fence signals
  access_->with_queue(queue, [&](Virtqueue& vq) {
    ChainWriter out(chain);
    if (!response->empty()) out.write(std::span(*response).first(std::min(response->size(), out.remaining())));
    vq.push_used(chain.head, out.written());
  });
  access_->signal_used();
}

void VirtioGpuGfxstream::on_fence(uint64_t fence_id, uint32_t ctx_id, uint8_t ring_idx, bool per_ring) {
  std::vector<Pending> done;
  {
    std::lock_guard lock(fence_mutex_);
    for (auto it = pending_.begin(); it != pending_.end();) {
      const bool same_timeline = per_ring ? (it->per_ring && it->ctx_id == ctx_id && it->ring_idx == ring_idx)
                                          : !it->per_ring;
      if (same_timeline && it->fence_id <= fence_id) {
        ++fences_done_;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - it->created).count();
        if (ms > 30) log(LogLevel::kDebug, "gpu: fence ctx{}/ring{} took {:.0f} ms", it->ctx_id, it->per_ring ? int(it->ring_idx) : -1, ms);
        done.push_back(std::move(*it));
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }
  }
  if (done.empty()) return;
  access_->with_queue(0, [&](Virtqueue& vq) {
    for (auto& p : done) {
      ChainWriter out(p.chain);
      out.write(std::span(p.response).first(std::min(p.response.size(), out.remaining())));
      vq.push_used(p.chain.head, out.written());
    }
  });
  access_->signal_used();
}

Result<std::vector<iovec>> VirtioGpuGfxstream::guest_iovecs(ChainReader& extra, uint32_t count) {
  if (count > kMaxEntries) return make_error("too many backing entries");
  std::vector<iovec> iov;
  iov.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t entry[16];  // struct virtio_gpu_mem_entry { le64 addr; le32 length; le32 padding; }
    if (!extra.read(entry)) return make_error("truncated backing entries");
    uint64_t addr;
    uint32_t len;
    std::memcpy(&addr, entry, 8);
    std::memcpy(&len, entry + 8, 4);
    auto span = memory_.translate(Gpa(addr), len);  // every page must be guest RAM
    if (!span) return make_error("backing page outside guest RAM");
    iov.push_back({span->data(), span->size()});
  }
  return iov;
}

uint32_t VirtioGpuGfxstream::create_2d(std::span<const uint8_t> req) {
  const uint32_t id = rd32(req, kHdrSize);
  if (id == 0 || resources_.contains(id)) return kRespErrInvalidResourceId;
  stream_renderer_resource_create_args args{};
  args.handle = id;
  args.target = 2;  // PIPE_TEXTURE_2D
  args.format = rd32(req, kHdrSize + 4);
  args.bind = 2;  // PIPE_BIND_RENDER_TARGET
  args.width = rd32(req, kHdrSize + 8);
  args.height = rd32(req, kHdrSize + 12);
  args.depth = 1;
  args.array_size = 1;
  if (stream_renderer_resource_create(&args, nullptr, 0) != 0) return kRespErrUnspec;
  resources_[id] = Resource{args.format, args.width, args.height, {}};
  return kRespOkNodata;
}

uint32_t VirtioGpuGfxstream::create_3d(std::span<const uint8_t> req) {
  const uint32_t id = rd32(req, kHdrSize);
  if (id == 0 || resources_.contains(id)) return kRespErrInvalidResourceId;
  stream_renderer_resource_create_args args{};
  args.handle = id;
  args.target = rd32(req, kHdrSize + 4);
  args.format = rd32(req, kHdrSize + 8);
  args.bind = rd32(req, kHdrSize + 12);
  args.width = rd32(req, kHdrSize + 16);
  args.height = rd32(req, kHdrSize + 20);
  args.depth = rd32(req, kHdrSize + 24);
  args.array_size = rd32(req, kHdrSize + 28);
  args.last_level = rd32(req, kHdrSize + 32);
  args.nr_samples = rd32(req, kHdrSize + 36);
  args.flags = rd32(req, kHdrSize + 40);
  if (stream_renderer_resource_create(&args, nullptr, 0) != 0) return kRespErrUnspec;
  resources_[id] = Resource{args.format, args.width, args.height, {}};
  return kRespOkNodata;
}

uint32_t VirtioGpuGfxstream::attach_backing(std::span<const uint8_t> req, ChainReader& extra) {
  auto it = resources_.find(rd32(req, kHdrSize));
  if (it == resources_.end()) return kRespErrInvalidResourceId;
  auto iov = guest_iovecs(extra, rd32(req, kHdrSize + 4));
  if (!iov) return kRespErrInvalidParameter;
  it->second.backing = std::move(*iov);
  if (stream_renderer_resource_attach_iov(static_cast<int>(it->first), it->second.backing.data(),
                                          static_cast<int>(it->second.backing.size())) != 0) {
    it->second.backing.clear();
    return kRespErrUnspec;
  }
  return kRespOkNodata;
}

uint32_t VirtioGpuGfxstream::create_blob(std::span<const uint8_t> req, ChainReader& extra) {
  const uint32_t ctx_id = rd32(req, 16);
  const uint32_t id = rd32(req, kHdrSize);
  if (id == 0 || resources_.contains(id)) return kRespErrInvalidResourceId;
  struct stream_renderer_create_blob blob{};
  blob.blob_mem = rd32(req, kHdrSize + 4);
  blob.blob_flags = rd32(req, kHdrSize + 8);
  const uint32_t entries = rd32(req, kHdrSize + 12);
  blob.blob_id = rd64(req, kHdrSize + 16);
  blob.size = rd64(req, kHdrSize + 24);
  auto iov = guest_iovecs(extra, entries);
  if (!iov) return kRespErrInvalidParameter;
  Resource r;
  r.backing = std::move(*iov);
  if (stream_renderer_create_blob(ctx_id, id, &blob, r.backing.data(),
                                  static_cast<uint32_t>(r.backing.size()), nullptr) != 0) {
    return kRespErrUnspec;
  }
  resources_[id] = std::move(r);
  return kRespOkNodata;
}

std::vector<uint8_t> VirtioGpuGfxstream::map_blob(std::span<const uint8_t> req) {
  std::vector<uint8_t> resp(kHdrSize + 8, 0);
  wr32(resp, 0, kRespErrUnspec);
  const uint32_t id = rd32(req, kHdrSize);
  const uint64_t offset = rd64(req, kHdrSize + 8);
  if (!resources_.contains(id) || mappings_.contains(id)) {
    wr32(resp, 0, kRespErrInvalidResourceId);
    return resp;
  }
  const auto base = shm_base_ ? shm_base_() : std::nullopt;
  void* hva = nullptr;
  uint64_t size = 0;
  if (!base || stream_renderer_resource_map(id, &hva, &size) != 0) return resp;
  // Hypervisor.framework maps whole 16 KB host pages; the guest must also stay in its BAR.
  const uint64_t mapped = align_up(size, kHostPage);
  if (reinterpret_cast<uintptr_t>(hva) % kHostPage != 0 || offset % kHostPage != 0 ||
      offset > kHostVisibleSize || mapped > kHostVisibleSize - offset) {
    log(LogLevel::kWarn, "virtio-gpu: can't map blob {} (hva {:p}, size {:#x}, offset {:#x})", id, hva,
        size, offset);
    stream_renderer_resource_unmap(id);
    wr32(resp, 0, kRespErrInvalidParameter);
    return resp;
  }
  if (auto r = options_.map(hva, *base + offset, mapped); !r) {
    log(LogLevel::kWarn, "virtio-gpu: {}", r.error().message);
    stream_renderer_resource_unmap(id);
    return resp;
  }
  mappings_[id] = Mapping{*base + offset, mapped};
  uint32_t map_info = 0;
  stream_renderer_resource_map_info(id, &map_info);
  wr32(resp, 0, kRespOkMapInfo);
  wr32(resp, kHdrSize, map_info);
  return resp;
}

uint32_t VirtioGpuGfxstream::unmap_blob(uint32_t id) {
  auto it = mappings_.find(id);
  if (it == mappings_.end()) return kRespErrInvalidResourceId;
  options_.unmap(it->second.gpa, it->second.size);
  mappings_.erase(it);
  stream_renderer_resource_unmap(id);
  return kRespOkNodata;
}

void VirtioGpuGfxstream::set_frame_tap(std::function<void(const Frame&)> tap) {
  std::lock_guard lock(tap_mutex_);
  tapping_ = static_cast<bool>(tap);
  frame_tap_ = std::move(tap);
}

void VirtioGpuGfxstream::debug_stall(int seconds) {
  {
    std::lock_guard lock(work_mutex_);
    tasks_.push_back([seconds] { std::this_thread::sleep_for(std::chrono::seconds(seconds)); });
  }
  work_cv_.notify_one();
}

bool VirtioGpuGfxstream::capture_frame() {
  auto done = std::make_shared<std::promise<void>>();
  auto finished = done->get_future();
  {
    std::lock_guard lock(work_mutex_);
    tasks_.push_back([this, done] {
      {
        std::lock_guard state(mutex_);
        if (scanout_.resource) readback(scanout_.resource);
      }
      done->set_value();
    });
  }
  work_cv_.notify_one();
  return finished.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
}

bool VirtioGpuGfxstream::show_in_window(void* native_window, int x, int y, int w, int h, float dpr) {
  const bool ok = stream_renderer_kestrel_show_window(native_window, x, y, w, h, static_cast<int>(options_.width),
                                                      static_cast<int>(options_.height), dpr) == 0;
  if (ok && !direct_display_.exchange(true)) log(LogLevel::kInfo, "display: direct from the GPU (no readback)");
  return ok;
}

void VirtioGpuGfxstream::present(uint32_t id) {
  if (!options_.sink || id == 0 || id != scanout_.resource) return;
  auto it = resources_.find(id);
  if (it == resources_.end() || scanout_.w == 0 || scanout_.h == 0) return;
  if (direct_display_) {
    // gfxstream shows the frame in our window from its own thread; the guest
    // doesn't wait for a copy.
    const auto t0 = std::chrono::steady_clock::now();
    stream_renderer_flush(id);
    const auto t1 = std::chrono::steady_clock::now();
    options_.sink->frame_shown();
    // Where hitches come from: a late frame (gap) vs. a slow hand-off to the display.
    static std::chrono::steady_clock::time_point last;
    const double gap = std::chrono::duration<double, std::milli>(t0 - last).count();
    const double flush = std::chrono::duration<double, std::milli>(t1 - t0).count();
    last = t0;
    if (gap > 40 && gap < 1000) {
      size_t backlog;
      {
        std::lock_guard w(work_mutex_);
        backlog = work_.size();
      }
      log(LogLevel::kDebug, "display: hitch {:.0f} ms since the previous frame (flush {:.1f} ms, {} commands waiting)", gap, flush, backlog);
    }
    if (tapping_) readback(id, false);  // recording: only the tap gets a copy
    return;
  }
  readback(id);
}

void VirtioGpuGfxstream::readback(uint32_t id, bool to_sink) {
  auto it = resources_.find(id);
  if (it == resources_.end() || scanout_.w == 0 || scanout_.h == 0) return;
  // Read the finished frame back from the GPU.
  const auto t0 = std::chrono::steady_clock::now();
  frame_.resize(size_t(scanout_.w) * scanout_.h * 4);
  iovec iov{frame_.data(), frame_.size()};
  stream_renderer_box box{scanout_.x, scanout_.y, 0, scanout_.w, scanout_.h, 1};
  if (stream_renderer_transfer_read_iov(id, 0, 0, scanout_.w * 4, 0, &box, 0, &iov, 1) != 0) return;
  const Frame frame{scanout_.w, scanout_.h, scanout_.w * 4, is_bgra(it->second.format), frame_};
  if (tapping_) {
    std::lock_guard tap(tap_mutex_);
    if (frame_tap_) frame_tap_(frame);
  }
  if (to_sink) options_.sink->present(frame);
  // Frame-rate statistics for performance work (debug log every 5 s).
  stats_frames_++;
  stats_busy_ += std::chrono::steady_clock::now() - t0;
  if (const auto now = std::chrono::steady_clock::now(); now - stats_start_ > std::chrono::seconds(5)) {
    const double secs = std::chrono::duration<double>(now - stats_start_).count();
    log(LogLevel::kDebug, "display: {:.1f} fps, {:.2f} ms per frame to read back and present",
        stats_frames_ / secs, std::chrono::duration<double, std::milli>(stats_busy_).count() / std::max(1u, stats_frames_));
    stats_start_ = now;
    stats_frames_ = 0;
    stats_busy_ = {};
  }
}

std::optional<std::vector<uint8_t>> VirtioGpuGfxstream::handle(std::span<const uint8_t> req,
                                                               ChainReader& extra,
                                                               const Virtqueue::Chain& chain) {
  std::vector<uint8_t> resp(kHdrSize, 0);
  uint32_t result = kRespOkNodata;
  const uint32_t type = rd32(req, 0);
  const uint32_t flags = rd32(req, 4);
  const uint64_t fence_id = rd64(req, 8);
  const uint32_t ctx_id = rd32(req, 16);
  const uint8_t ring_idx = req[20];

  std::unique_lock lock(mutex_);
  switch (type) {
    case kCmdGetEdid: {
      // virtio 1.2 §5.7.6.8: le32 size, le32 padding, u8 edid[1024].
      resp.resize(kHdrSize + 8 + 1024, 0);
      if (rd32(req, kHdrSize) != 0) {  // one scanout
        result = kRespErrInvalidParameter;
        break;
      }
      const auto edid = make_edid(options_.width, options_.height, options_.refresh_hz);
      wr32(resp, kHdrSize, static_cast<uint32_t>(edid.size()));
      std::memcpy(resp.data() + kHdrSize + 8, edid.data(), edid.size());
      result = kRespOkEdid;
      break;
    }
    case kCmdGetDisplayInfo:
      resp.resize(kHdrSize + kMaxScanouts * 24, 0);
      wr32(resp, kHdrSize + 8, options_.width);
      wr32(resp, kHdrSize + 12, options_.height);
      wr32(resp, kHdrSize + 16, 1);
      result = kRespOkDisplayInfo;
      break;
    case kCmdGetCapsetInfo: {
      const uint32_t index = rd32(req, kHdrSize);
      if (index >= std::size(kCapsets)) {
        result = kRespErrInvalidParameter;
        break;
      }
      uint32_t max_ver = 0, max_size = 0;
      stream_renderer_get_cap_set(kCapsets[index], &max_ver, &max_size);
      resp.resize(kHdrSize + 16, 0);
      wr32(resp, kHdrSize, kCapsets[index]);
      wr32(resp, kHdrSize + 4, max_ver);
      wr32(resp, kHdrSize + 8, max_size);
      result = kRespOkCapsetInfo;
      break;
    }
    case kCmdGetCapset: {
      const uint32_t id = rd32(req, kHdrSize), version = rd32(req, kHdrSize + 4);
      uint32_t max_ver = 0, max_size = 0;
      stream_renderer_get_cap_set(id, &max_ver, &max_size);
      if (max_size == 0 || max_size > 64 * 1024) {
        result = kRespErrInvalidParameter;
        break;
      }
      resp.resize(kHdrSize + max_size, 0);
      stream_renderer_fill_caps(id, version, resp.data() + kHdrSize);
      result = kRespOkCapset;
      break;
    }
    case kCmdCtxCreate: {
      const uint32_t nlen = std::min<uint32_t>(rd32(req, kHdrSize), 64);
      const uint32_t context_init = rd32(req, kHdrSize + 4);
      const char* name = reinterpret_cast<const char*>(req.data() + kHdrSize + 8);
      if (stream_renderer_context_create(ctx_id, nlen, name, context_init) != 0) result = kRespErrUnspec;
      break;
    }
    case kCmdCtxDestroy: stream_renderer_context_destroy(ctx_id); break;
    case kCmdCtxAttachResource:
      stream_renderer_ctx_attach_resource(static_cast<int>(ctx_id), static_cast<int>(rd32(req, kHdrSize)));
      break;
    case kCmdCtxDetachResource:
      stream_renderer_ctx_detach_resource(static_cast<int>(ctx_id), static_cast<int>(rd32(req, kHdrSize)));
      break;
    case kCmdResourceCreate2d: result = create_2d(req); break;
    case kCmdResourceCreate3d: result = create_3d(req); break;
    case kCmdResourceCreateBlob: result = create_blob(req, extra); break;
    case kCmdResourceUnref: {
      const uint32_t id = rd32(req, kHdrSize);
      if (!resources_.contains(id)) {
        result = kRespErrInvalidResourceId;
        break;
      }
      if (mappings_.contains(id)) unmap_blob(id);
      stream_renderer_resource_unref(id);
      resources_.erase(id);
      if (scanout_.resource == id) scanout_ = {};
      break;
    }
    case kCmdResourceAttachBacking: result = attach_backing(req, extra); break;
    case kCmdResourceDetachBacking: {
      auto it = resources_.find(rd32(req, kHdrSize));
      if (it == resources_.end()) {
        result = kRespErrInvalidResourceId;
        break;
      }
      iovec* iov = nullptr;
      int n = 0;
      stream_renderer_resource_detach_iov(static_cast<int>(it->first), &iov, &n);
      it->second.backing.clear();
      break;
    }
    case kCmdTransferToHost2d: {
      const uint32_t id = rd32(req, kHdrSize + 24);
      if (!resources_.contains(id)) {
        result = kRespErrInvalidResourceId;
        break;
      }
      stream_renderer_box box{rd32(req, kHdrSize), rd32(req, kHdrSize + 4), 0,
                              rd32(req, kHdrSize + 8), rd32(req, kHdrSize + 12), 1};
      if (stream_renderer_transfer_write_iov(id, 0, 0, 0, 0, &box, rd64(req, kHdrSize + 16), nullptr, 0) != 0) {
        result = kRespErrUnspec;
      }
      break;
    }
    case kCmdTransferToHost3d:
    case kCmdTransferFromHost3d: {
      stream_renderer_box box{rd32(req, kHdrSize),      rd32(req, kHdrSize + 4),  rd32(req, kHdrSize + 8),
                              rd32(req, kHdrSize + 12), rd32(req, kHdrSize + 16), rd32(req, kHdrSize + 20)};
      const uint64_t offset = rd64(req, kHdrSize + 24);
      const uint32_t id = rd32(req, kHdrSize + 32), level = rd32(req, kHdrSize + 36);
      const uint32_t stride = rd32(req, kHdrSize + 40), layer_stride = rd32(req, kHdrSize + 44);
      if (!resources_.contains(id)) {
        result = kRespErrInvalidResourceId;
        break;
      }
      const int r = type == kCmdTransferToHost3d
                        ? stream_renderer_transfer_write_iov(id, ctx_id, static_cast<int>(level), stride,
                                                             layer_stride, &box, offset, nullptr, 0)
                        : stream_renderer_transfer_read_iov(id, ctx_id, level, stride, layer_stride, &box,
                                                            offset, nullptr, 0);
      if (r != 0) result = kRespErrUnspec;
      break;
    }
    case kCmdSubmit3d: {
      const uint32_t size = rd32(req, kHdrSize);
      if (size > 64 * 1024 * 1024 || extra.remaining() < size) {
        result = kRespErrInvalidParameter;
        break;
      }
      std::vector<uint8_t> cmds(size);
      extra.read(cmds);
      stream_renderer_command cmd{ctx_id, size, cmds.data(), 0, nullptr};
      if (stream_renderer_submit_cmd(&cmd) != 0) result = kRespErrInvalidParameter;
      break;
    }
    case kCmdResourceMapBlob: {
      auto r = map_blob(req);
      resp = std::move(r);
      result = rd32(resp, 0);
      break;
    }
    case kCmdResourceUnmapBlob: result = unmap_blob(rd32(req, kHdrSize)); break;
    case kCmdSetScanout:
    case kCmdSetScanoutBlob: {
      const uint32_t scanout = rd32(req, kHdrSize + 16), id = rd32(req, kHdrSize + 20);
      if (scanout != 0) {
        result = kRespErrInvalidScanoutId;
        break;
      }
      if (id != 0 && !resources_.contains(id)) {
        result = kRespErrInvalidResourceId;
        break;
      }
      scanout_ = Scanout{id, rd32(req, kHdrSize), rd32(req, kHdrSize + 4), rd32(req, kHdrSize + 8),
                         rd32(req, kHdrSize + 12)};
      if (type == kCmdSetScanoutBlob && id != 0) {
        // Blob scanouts carry their own format and size.
        auto& r = resources_[id];
        r.width = rd32(req, kHdrSize + 24);
        r.height = rd32(req, kHdrSize + 28);
        r.format = rd32(req, kHdrSize + 32);
      }
      break;
    }
    case kCmdResourceFlush: present(rd32(req, kHdrSize + 16)); break;
    default:
      log(LogLevel::kDebug, "virtio-gpu: unsupported command {:#x}", type);
      result = kRespErrUnspec;
      break;
  }
  wr32(resp, 0, result);

  if (flags & kFlagFence) {
    // Answer once the GPU work submitted before this fence has finished.
    wr32(resp, 4, flags);
    std::memcpy(resp.data() + 8, req.data() + 8, 16);  // fence_id, ctx_id, ring_idx
    const bool per_ring = (flags & kFlagInfoRingIdx) != 0;
    {
      std::lock_guard fences(fence_mutex_);
      pending_.push_back(Pending{chain, resp, fence_id, ctx_id, ring_idx, per_ring});
      ++fences_created_;
      if (const auto now = std::chrono::steady_clock::now(); now - fence_stats_at_ > std::chrono::seconds(5)) {
        fence_stats_at_ = now;
        std::map<std::pair<uint32_t, int>, size_t> by_ring;
        for (const auto& p : pending_) ++by_ring[{p.ctx_id, p.per_ring ? p.ring_idx : -1}];
        std::string rings;
        for (const auto& [k, n] : by_ring) rings += std::format(" ctx{}/ring{}:{}", k.first, k.second, n);
        log(LogLevel::kDebug, "gpu fences: {} created, {} done, {} pending:{}", fences_created_, fences_done_,
            pending_.size(), rings);
      }
    }
    stream_renderer_fence fence{};
    fence.flags = STREAM_RENDERER_FLAG_FENCE | (per_ring ? STREAM_RENDERER_FLAG_FENCE_RING_IDX : 0);
    fence.fence_id = fence_id;
    fence.ctx_id = ctx_id;
    fence.ring_idx = ring_idx;
    lock.unlock();
    stream_renderer_create_fence(&fence);
    return std::nullopt;
  }
  return resp;
}

}  // namespace kestrel
