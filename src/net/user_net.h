#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "net/packet.h"

namespace kestrel::net {

// User-mode NAT (docs/DESIGN.md §4.2, §5.5): the guest sees a private 10.0.2.0/24
// network; its TCP/UDP/ICMP traffic is re-issued through ordinary host sockets.
// Needs no root and no entitlements, and nothing listens on external interfaces.
//
//   10.0.2.15  guest (via DHCP)     10.0.2.2  gateway, also the host's 127.0.0.1
//   10.0.2.3   DNS (forwarded to the host's resolver)
class UserNet {
 public:
  using SendFn = std::function<void(std::vector<uint8_t> frame)>;

  static constexpr Ip4 kNetwork = ip4(10, 0, 2, 0);
  static constexpr Ip4 kGateway = ip4(10, 0, 2, 2);
  static constexpr Ip4 kDns = ip4(10, 0, 2, 3);
  static constexpr Ip4 kGuest = ip4(10, 0, 2, 15);
  static constexpr Mac kGatewayMac = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};

  // `send` delivers frames to the guest; called on the network thread.
  explicit UserNet(SendFn send);
  ~UserNet();

  // A frame from the guest. Thread-safe; processing happens on the network thread.
  void from_guest(std::span<const uint8_t> frame);

  // Forwards host 127.0.0.1:<host_port> to the guest's <guest_port> (TCP), e.g.
  // adb: 6520 -> 5555. Call before traffic starts. Loopback only: never exposed
  // to the network (docs/DESIGN.md §4.2).
  bool add_port_forward(uint16_t host_port, uint16_t guest_port);

 private:
  struct TcpConn;
  struct UdpFlow;
  using Clock = std::chrono::steady_clock;

  void loop();
  void handle_frame(std::span<const uint8_t> frame);
  void handle_arp(std::span<const uint8_t> frame);
  void handle_ip(std::span<const uint8_t> frame);
  void handle_udp(Ip4 src, Ip4 dst, std::span<const uint8_t> udp);
  void handle_dhcp(std::span<const uint8_t> payload);
  void handle_icmp(Ip4 dst, std::span<const uint8_t> icmp);
  void handle_tcp(Ip4 src, Ip4 dst, std::span<const uint8_t> tcp);

  void send_ip(Ip4 src, Ip4 dst, uint8_t proto, std::span<const uint8_t> l4);
  void send_udp(Ip4 src, uint16_t sport, Ip4 dst, uint16_t dport, std::span<const uint8_t> data);
  void send_tcp(const TcpConn& c, uint8_t flags, uint32_t seq, std::span<const uint8_t> data,
                bool mss_option = false);

  void poll_udp(UdpFlow& flow);
  void poll_icmp();
  void tcp_connected(TcpConn& c);
  void tcp_readable(TcpConn& c);
  void tcp_writable(TcpConn& c);
  void tcp_reset(TcpConn& c);
  void tcp_retransmit(TcpConn& c, size_t max_bytes);
  void tcp_timers();
  int next_timer_ms() const;
  void housekeeping();
  void accept_forward(size_t index);

  Ip4 host_address_for(Ip4 guest_dst) const;  // NAT rewrite of the destination
  Ip4 guest_address_for(Ip4 host_src, uint16_t port) const;

  SendFn send_;
  Mac guest_mac_{};
  bool guest_mac_known_ = false;
  Ip4 host_dns_;
  uint16_t ip_id_ = 1;

  std::map<uint64_t, std::unique_ptr<TcpConn>> tcp_;  // key: guest port | remote ip | port
  std::map<uint16_t, std::unique_ptr<UdpFlow>> udp_;  // key: guest source port
  int icmp_fd_ = -1;
  struct Forward {
    int fd;
    uint16_t guest_port;
  };
  std::vector<Forward> forwards_;
  uint16_t next_forward_port_ = 40000;  // source ports the guest sees for forwarded connections

  std::mutex queue_mutex_;
  std::deque<std::vector<uint8_t>> inbox_;
  int wake_pipe_[2] = {-1, -1};
  std::atomic<bool> quit_{false};
  std::thread thread_;
};

}  // namespace kestrel::net
