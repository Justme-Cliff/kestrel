#pragma once

#include <sys/uio.h>

#include <chrono>
#include <functional>
#include <list>
#include <map>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <optional>
#include <vector>

#include "core/error.h"
#include "core/guest_memory.h"
#include "devices/virtio/virtio_device.h"
#include "host/display.h"

namespace kestrel {

// virtio-gpu with 3D acceleration (virtio 1.2 §5.7 + blob/context-init extensions),
// backed by gfxstream: guest Vulkan (and GLES via guest ANGLE) command streams run
// on the Mac's GPU through MoltenVK (docs/DESIGN.md §5.6).
class VirtioGpuGfxstream final : public VirtioDevice {
 public:
  struct Options {
    uint32_t width, height;
    DisplaySink* sink;
    // Run the guest's OpenGL ES on the host (ANGLE on Metal) instead of on guest
    // ANGLE over Vulkan. Needs lib64/gles_angle next to the executable.
    bool host_gles = false;
    uint32_t refresh_hz = 60;  // the display's refresh rate: Android's frame-rate cap
    // Maps/unmaps host memory into guest physical space (host-visible blobs).
    std::function<Result<void>(void* host, uint64_t gpa, uint64_t size)> map;
    std::function<Result<void>(uint64_t gpa, uint64_t size)> unmap;
    // Watchdog (any thread): the GPU made no progress for kStallSeconds (a command
    // stuck, or a fence the host GPU never signals), and later moved again.
    std::function<void(const std::string& what)> on_stall;
    std::function<void(double seconds)> on_recover;
  };
  static constexpr int kStallSeconds = 10;

  // Initializes gfxstream (once per process). Fails if no usable Vulkan GPU.
  static Result<std::unique_ptr<VirtioGpuGfxstream>> create(const GuestMemory& memory, Options options);
  ~VirtioGpuGfxstream() override;

  uint32_t device_id() const override { return virtio_id::kGpu; }
  std::string_view name() const override { return "virtio-gpu-gfxstream"; }
  uint64_t features() const override;
  unsigned num_queues() const override { return 2; }
  void read_config(uint64_t offset, std::span<uint8_t> out) override;
  void write_config(uint64_t offset, std::span<const uint8_t> in) override;
  void queue_notify(unsigned queue) override;
  void reset() override;

  static constexpr uint64_t kHostVisibleSize = 8ull << 30;  // BAR for mapped blobs
  uint64_t shared_memory_size() const override { return kHostVisibleSize; }
  uint8_t shared_memory_id() const override { return 1; }  // VIRTIO_GPU_SHM_ID_HOST_VISIBLE
  void bind_shared_memory(std::function<std::optional<uint64_t>()> guest_base) override {
    shm_base_ = std::move(guest_base);
  }

  // Shows frames straight from the GPU in `native_window` (an NSWindow), in the
  // rectangle x,y,w,h (points, top-left origin): no readback per frame. Call again on
  // resize. Main thread only (AppKit). Returns false if gfxstream can't.
  bool show_in_window(void* native_window, int x, int y, int w, int h, float dpr);
  // Reads the frame on screen back to the sink now (screenshots in direct mode).
  // Waits up to 3 s; false on timeout.
  bool capture_frame();
  bool direct_display() const { return direct_display_; }
  // Screen recording: every frame the guest shows is also read back and handed to
  // `tap` (GPU thread; copy what you need). Empty stops. Costs a readback per frame,
  // so only while recording.
  void set_frame_tap(std::function<void(const Frame&)> tap);
  // Tests: blocks the GPU thread for `seconds` (the watchdog must notice).
  void debug_stall(int seconds);

  // Called by gfxstream when a fence signals (any thread).
  void on_fence(uint64_t fence_id, uint32_t ctx_id, uint8_t ring_idx, bool per_ring);

 private:
  VirtioGpuGfxstream(const GuestMemory& memory, Options options);

  struct Pending {  // a request whose answer waits for a fence
    Virtqueue::Chain chain;
    std::vector<uint8_t> response;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    bool per_ring;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
  };
  struct Resource {
    uint32_t format = 0, width = 0, height = 0;
    std::vector<iovec> backing;  // guest pages; kept alive while gfxstream uses them
  };
  struct Mapping {
    uint64_t gpa, size;
  };
  struct Scanout {
    uint32_t resource = 0;
    uint32_t x = 0, y = 0, w = 0, h = 0;
  };

  // Handles one command; returns the response, or nullopt if `chain` was deferred.
  std::optional<std::vector<uint8_t>> handle(std::span<const uint8_t> req, ChainReader& extra,
                                             const Virtqueue::Chain& chain);
  uint32_t create_2d(std::span<const uint8_t> req);
  uint32_t create_3d(std::span<const uint8_t> req);
  uint32_t attach_backing(std::span<const uint8_t> req, ChainReader& extra);
  uint32_t create_blob(std::span<const uint8_t> req, ChainReader& extra);
  std::vector<uint8_t> map_blob(std::span<const uint8_t> req);
  uint32_t unmap_blob(uint32_t resource_id);
  void present(uint32_t resource_id);
  // Reads the frame back; to the sink (screenshots, no direct display) and the tap.
  void readback(uint32_t resource_id, bool to_sink = true);  // needs mutex_
  Result<std::vector<iovec>> guest_iovecs(ChainReader& extra, uint32_t count);

  const GuestMemory& memory_;
  Options options_;
  std::function<std::optional<uint64_t>()> shm_base_;

  void worker();
  void process(unsigned queue, const Virtqueue::Chain& chain);

  // Requests run on one GPU thread, in ring order, never on vCPU threads: a GLES
  // pipe read can block until a later write from the guest arrives, and vCPUs
  // must keep running to send it.
  std::mutex work_mutex_;
  std::condition_variable work_cv_;
  std::deque<std::pair<unsigned, Virtqueue::Chain>> work_;
  std::deque<std::function<void()>> tasks_;  // host requests, run between commands
  bool stop_ = false;
  std::thread thread_;

  // Watchdog: when the GPU thread started what it's doing now (0: idle).
  std::atomic<int64_t> busy_since_{0};
  void watchdog();
  std::thread watchdog_;
  // Its own wake-up: sharing work_cv_ let a notify_one meant for the GPU thread wake
  // the watchdog instead, leaving guest commands unanswered (Android failed to boot).
  std::mutex watchdog_mutex_;
  std::condition_variable watchdog_cv_;
  bool watchdog_stop_ = false;

  std::mutex fence_mutex_;  // guards pending_ (fences signal from gfxstream threads)
  std::list<Pending> pending_;
  uint64_t fences_created_ = 0, fences_done_ = 0;  // debug statistics
  std::chrono::steady_clock::time_point fence_stats_at_ = std::chrono::steady_clock::now();

  std::atomic<bool> direct_display_{false};
  std::mutex tap_mutex_;
  std::function<void(const Frame&)> frame_tap_;
  std::atomic<bool> tapping_{false};

  std::mutex mutex_;  // guards everything below (GPU thread vs. reset)
  std::map<uint32_t, Resource> resources_;
  std::map<uint32_t, Mapping> mappings_;  // resource id -> guest mapping
  Scanout scanout_;
  std::vector<uint8_t> frame_;
  uint32_t events_read_ = 0;
  unsigned stats_frames_ = 0;
  std::chrono::steady_clock::duration stats_busy_{};
  std::chrono::steady_clock::time_point stats_start_ = std::chrono::steady_clock::now();
};

}  // namespace kestrel
