#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "devices/virtio/virtio_vsock.h"

namespace kestrel::net {

// Host end of virtio-vsock (CID 2). Bridges host TCP ports on 127.0.0.1 to guest
// vsock ports (adb: 6520 -> 5555, as Cuttlefish does), and accepts guest
// connections to chosen host ports (their data is discarded for now).
class VsockHost {
 public:
  using SendFn = std::function<void(VsockPacket)>;

  VsockHost(uint64_t guest_cid, SendFn send);
  ~VsockHost();

  bool add_forward(uint16_t host_tcp_port, uint32_t guest_port);  // call before traffic
  void accept_guest_connections(uint32_t host_port);              // call before traffic

  void from_guest(VsockPacket packet);  // thread-safe

  static constexpr uint32_t kBufAlloc = 256 * 1024;  // our receive buffer per connection

 private:
  struct Conn;
  void loop();
  void handle(VsockPacket& p);
  void accept_forward(size_t index);
  void conn_readable(Conn& c);
  void conn_writable(Conn& c);
  void send_control(const Conn& c, uint16_t op, uint32_t flags = 0);
  void send_rst(const VsockPacket& to);
  uint32_t peer_credit(const Conn& c) const;

  uint64_t guest_cid_;
  SendFn send_;
  struct Forward {
    int fd;
    uint32_t guest_port;
  };
  std::vector<Forward> forwards_;
  std::set<uint32_t> accepted_ports_;
  std::map<uint64_t, std::unique_ptr<Conn>> conns_;  // key: local port << 32 | guest port
  uint32_t next_local_port_ = 49152;

  std::mutex inbox_mutex_;
  std::deque<VsockPacket> inbox_;
  int wake_pipe_[2] = {-1, -1};
  std::atomic<bool> quit_{false};
  std::thread thread_;
};

}  // namespace kestrel::net
