#include "devices/virtio/virtio_snd.h"

#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "core/thread_priority.h"

namespace kestrel {
namespace {

enum : uint32_t {
  kJackInfo = 1,
  kPcmInfo = 0x0100,
  kPcmSetParams = 0x0101,
  kPcmPrepare = 0x0102,
  kPcmRelease = 0x0103,
  kPcmStart = 0x0104,
  kPcmStop = 0x0105,
  kChmapInfo = 0x0200,
  kStatusOk = 0x8000,
  kStatusBadMsg = 0x8001,
  kStatusNotSupported = 0x8002,
};
constexpr uint8_t kDirOutput = 0, kDirInput = 1;
constexpr uint8_t kFmtS16 = 5;
constexpr uint8_t kRate44100 = 6, kRate48000 = 7;
constexpr uint8_t kChmapFL = 3, kChmapFR = 4, kChmapMono = 2;  // VIRTIO_SND_CHMAP_*
constexpr unsigned kControlQ = 0, kTxQ = 2, kRxQ = 3;
constexpr size_t kMaxBufferBytes = 1 << 20;
constexpr size_t kXferHeader = 4;  // struct virtio_snd_pcm_xfer

uint32_t rd32(std::span<const uint8_t> b, size_t off) {
  uint32_t v = 0;
  if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
  return v;
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + 4);
}
void put64(std::vector<uint8_t>& b, uint64_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + 8);
}

}  // namespace

VirtioSnd::VirtioSnd(std::unique_ptr<AudioOutput> output, std::unique_ptr<AudioInput> input)
    : output_(std::move(output)), input_(std::move(input)) {
  pacer_ = std::thread([this] { pace(); });
}

VirtioSnd::~VirtioSnd() {
  quit_ = true;
  cv_.notify_all();
  pacer_.join();
  if (output_) output_->stop();
}

void VirtioSnd::read_config(uint64_t offset, std::span<uint8_t> out) {
  const uint32_t config[3] = {0 /*jacks*/, 2 /*streams*/, 2 /*chmaps*/};
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = offset + i < sizeof(config) ? reinterpret_cast<const uint8_t*>(config)[offset + i] : 0;
  }
}

void VirtioSnd::reset() {
  std::lock_guard lock(mutex_);
  for (auto& s : streams_) {
    s.state = State::kIdle;
    s.buffers.clear();
  }
  if (output_) output_->stop();
}

std::vector<uint8_t> VirtioSnd::control(std::span<const uint8_t> req) {
  std::vector<uint8_t> resp;
  const uint32_t code = rd32(req, 0);
  switch (code) {
    case kPcmInfo:
    case kChmapInfo:
    case kJackInfo: {
      // struct virtio_snd_query_info { hdr; start_id; count; size }
      const uint32_t start = rd32(req, 4), count = rd32(req, 8);
      const uint32_t total = code == kJackInfo ? 0 : 2;
      if (start > total || count > total - start) {
        put32(resp, kStatusBadMsg);
        return resp;
      }
      put32(resp, kStatusOk);
      for (uint32_t i = start; i < start + count; ++i) {
        const bool out = i == 0;
        if (code == kPcmInfo) {  // struct virtio_snd_pcm_info (32 bytes)
          put32(resp, 0);        // hda_fn_nid: both streams on PCM device 0
          put32(resp, 0);        // features
          put64(resp, 1ull << kFmtS16);
          put64(resp, (1ull << kRate44100) | (1ull << kRate48000));
          resp.push_back(out ? kDirOutput : kDirInput);
          resp.push_back(1);  // channels_min
          resp.push_back(2);  // channels_max
          resp.insert(resp.end(), 5, 0);
        } else {  // struct virtio_snd_chmap_info (24 bytes)
          put32(resp, 0);
          resp.push_back(out ? kDirOutput : kDirInput);
          resp.push_back(out ? 2 : 1);
          uint8_t pos[18] = {};
          if (out) {
            pos[0] = kChmapFL;
            pos[1] = kChmapFR;
          } else {
            pos[0] = kChmapMono;
          }
          resp.insert(resp.end(), pos, pos + 18);
        }
      }
      return resp;
    }
    case kPcmSetParams: {
      // hdr, stream_id, buffer_bytes, period_bytes, features, channels u8, format u8, rate u8
      const uint32_t id = rd32(req, 4);
      if (id > 1 || req.size() < 24) {
        put32(resp, kStatusBadMsg);
        return resp;
      }
      const uint8_t channels = req[20], format = req[21], rate = req[22];
      if (format != kFmtS16 || (rate != kRate44100 && rate != kRate48000) || channels < 1 || channels > 2) {
        put32(resp, kStatusNotSupported);
        return resp;
      }
      std::lock_guard lock(mutex_);
      auto& s = streams_[id];
      s.channels = channels;
      s.rate = rate == kRate48000 ? 48000 : 44100;
      s.state = State::kSet;
      put32(resp, kStatusOk);
      return resp;
    }
    case kPcmPrepare:
    case kPcmRelease:
    case kPcmStart:
    case kPcmStop: {
      const uint32_t id = rd32(req, 4);
      if (id > 1) {
        put32(resp, kStatusBadMsg);
        return resp;
      }
      std::lock_guard lock(mutex_);
      auto& s = streams_[id];
      if (code == kPcmPrepare) {
        s.state = State::kPrepared;
      } else if (code == kPcmStart) {
        s.state = State::kRunning;
        s.started = std::chrono::steady_clock::now();
        s.delivered = 0;
        s.queued_bytes = 0;
        s.peak = 0;
        s.played_base = output_ ? output_->played_bytes() : 0;
        if (s.output && output_ && !output_->start(s.rate, s.channels)) {
          log(LogLevel::kWarn, "audio: no host output; playing silently");
        }
        if (s.output && output_) s.played_base = output_->played_bytes();
        // The microphone opens only while an app records (macOS asks the first time).
        if (!s.output && input_ && !input_->start(s.rate, s.channels)) {
          log(LogLevel::kInfo, "audio: microphone unavailable; recording silence");
        }
        log(LogLevel::kDebug, "audio: {} stream started, {} Hz, {} ch", s.output ? "playback" : "capture",
            s.rate, s.channels);
      } else if (code == kPcmStop && !s.output) {
        if (input_) input_->stop();
        s.state = State::kPrepared;
      } else if (code == kPcmStop) {
        if (s.output) {
          log(LogLevel::kDebug, "audio: playback stopped after {} ms, peak level {}",
              s.queued_bytes * 1000 / (s.rate * s.channels * 2ull), s.peak);
        }
        s.state = State::kPrepared;
        if (s.output && output_) output_->stop();
      } else {  // release: hand back any buffers still queued
        s.state = State::kSet;
        if (s.output && output_) output_->stop();
        if (!s.output && input_) input_->stop();
        std::vector<Buffer> done(std::make_move_iterator(s.buffers.begin()),
                                 std::make_move_iterator(s.buffers.end()));
        s.buffers.clear();
        complete(s.output ? kTxQ : kRxQ, done);
      }
      cv_.notify_all();
      put32(resp, kStatusOk);
      return resp;
    }
    default:
      put32(resp, kStatusNotSupported);
      return resp;
  }
}

void VirtioSnd::complete(unsigned queue, std::vector<Buffer>& done) {
  if (done.empty()) return;
  access_->with_queue(queue, [&](Virtqueue& vq) {
    for (auto& b : done) {
      ChainWriter out(b.chain);
      // Capture: the recorded audio comes before the status in the writable part;
      // whatever the microphone hasn't delivered yet is silence.
      if (queue == kRxQ) {
        for (size_t left = out.remaining() >= 8 ? out.remaining() - 8 : 0; left > 0;) {
          auto seg = out.next_segment(left);
          const size_t got = input_ ? input_->read(seg) : 0;
          std::memset(seg.data() + got, 0, seg.size() - got);
          left -= seg.size();
        }
      }
      const uint32_t status[2] = {kStatusOk, 0};  // struct virtio_snd_pcm_status
      out.write(std::span(reinterpret_cast<const uint8_t*>(status), 8));
      vq.push_used(b.chain.head, out.written());
    }
  });
  access_->signal_used();
  done.clear();
}

void VirtioSnd::queue_notify(unsigned queue) {
  std::vector<Virtqueue::Chain> chains;
  access_->with_queue(queue, [&](Virtqueue& vq) {
    while (true) {
      auto chain = vq.pop();
      if (!chain || !*chain) return;
      chains.push_back(std::move(**chain));
    }
  });
  if (chains.empty()) return;

  if (queue == kControlQ) {
    std::vector<std::pair<Virtqueue::Chain*, std::vector<uint8_t>>> answers;
    for (auto& c : chains) {
      ChainReader in(c);
      std::vector<uint8_t> req(std::min<size_t>(in.remaining(), 64));
      in.read(req);
      answers.emplace_back(&c, control(req));
    }
    access_->with_queue(kControlQ, [&](Virtqueue& vq) {
      for (auto& [c, resp] : answers) {
        ChainWriter out(*c);
        out.write(std::span(resp).first(std::min(resp.size(), out.remaining())));
        vq.push_used(c->head, out.written());
      }
    });
    access_->signal_used();
    return;
  }
  if (queue == kTxQ || queue == kRxQ) {
    std::lock_guard lock(mutex_);
    for (auto& c : chains) {
      ChainReader in(c);
      uint8_t xfer[kXferHeader];
      in.read(xfer);  // struct virtio_snd_pcm_xfer { le32 stream_id; }
      const uint32_t id = rd32(xfer, 0);
      // Samples are read later, just before they play: the guest may still be
      // filling this part of its ring buffer (virtio-snd works like DMA).
      streams_[id <= 1 ? id : (queue == kTxQ ? 0 : 1)].buffers.push_back(Buffer{std::move(c), 0, false});
    }
    cv_.notify_all();
  }
  // Event queue: we never raise events; buffers stay posted.
}

void VirtioSnd::pace() {
  set_thread_role(ThreadRole::kInteractive);  // late audio buffers are audible crackles
  // Every few milliseconds: feed playback into the host ring as space frees up,
  // and complete capture buffers at the stream's real-time rate.
  while (!quit_) {
    std::vector<Buffer> tx_done, rx_done;
    {
      std::unique_lock lock(mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(5));
      auto& out = streams_[0];
      if (out.state == State::kRunning) {
        const uint64_t frame = out.channels * 2ull;
        const uint64_t lead = out.rate * frame * 30 / 1000;  // keep ~30 ms queued on the host
        // Without a host device, pretend to play at the right speed.
        const uint64_t played =
            output_ ? output_->played_bytes() - out.played_base
                    : static_cast<uint64_t>(std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                                          out.started).count() *
                                            out.rate) * frame;
        // Copy the next buffers' samples to the host while we're less than `lead` ahead.
        for (auto& b : out.buffers) {
          if (b.queued) continue;
          if (out.queued_bytes > played + lead) break;
          ChainReader in(b.chain);
          uint8_t skip[kXferHeader];
          in.read(skip);
          std::vector<uint8_t> pcm(std::min<size_t>(in.remaining(), kMaxBufferBytes));
          in.read(pcm);
          if (output_) {
            // The ring is sized far above `lead`; anything it can't take is dropped.
            output_->write(pcm);
          }
          for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
            int16_t v;
            std::memcpy(&v, &pcm[i], 2);
            out.peak = std::max(out.peak, std::abs(int(v)));
          }
          out.queued_bytes += pcm.size();
          b.end = out.queued_bytes;
          b.queued = true;
        }
        // A buffer goes back to the guest once it has actually been played.
        while (!out.buffers.empty() && out.buffers.front().queued && out.buffers.front().end <= played) {
          tx_done.push_back(std::move(out.buffers.front()));
          out.buffers.pop_front();
        }
      }
      auto& in = streams_[1];
      if (in.state == State::kRunning) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - in.started).count();
        const uint64_t due = static_cast<uint64_t>(secs * in.rate) * in.channels * 2;
        while (!in.buffers.empty()) {
          ChainWriter probe(in.buffers.front().chain);
          const uint64_t size = probe.remaining() >= 8 ? probe.remaining() - 8 : 0;
          if (in.delivered + size > due) break;
          in.delivered += size;
          rx_done.push_back(std::move(in.buffers.front()));
          in.buffers.pop_front();
        }
      }
    }
    complete(kTxQ, tx_done);
    complete(kRxQ, rx_done);
  }
}

}  // namespace kestrel
