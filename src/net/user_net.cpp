#include "net/user_net.h"
#include "core/thread_priority.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>

#include <algorithm>
#include <deque>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "core/log.h"

namespace kestrel::net {
namespace {

constexpr uint8_t kTcpFin = 0x01;
constexpr uint8_t kTcpSyn = 0x02;
constexpr uint8_t kTcpRst = 0x04;
constexpr uint8_t kTcpPsh = 0x08;
constexpr uint8_t kTcpAck = 0x10;

constexpr uint16_t kMss = 1460;
// Bytes we let the guest have unacknowledged per connection. Window scaling lets it
// go past 64 KB, which keeps downloads at full speed.
constexpr uint32_t kMaxInFlight = 1024 * 1024;
constexpr auto kMinRto = std::chrono::milliseconds(200);
constexpr auto kMaxRto = std::chrono::milliseconds(4000);
constexpr int kMaxRetransmits = 12;  // ~40 s of silence: the guest is gone
constexpr size_t kMaxToHost = 256 * 1024;  // guest data buffered for a slow host socket
// Like a home router's UDP mapping: long enough that a quiet game session keeps its
// port (the server identifies players by it).
constexpr auto kUdpIdleTimeout = std::chrono::seconds(300);
constexpr size_t kMaxInbox = 4096;

bool seq_lt(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }
bool seq_le(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) <= 0; }

uint64_t tcp_key(uint16_t guest_port, Ip4 remote, uint16_t remote_port) {
  return (uint64_t(guest_port) << 48) | (uint64_t(remote) << 16) | remote_port;
}

void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

sockaddr_in make_addr(Ip4 ip, uint16_t port) {
  sockaddr_in a{};
  a.sin_len = sizeof(a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(ip);
  return a;
}

// First IPv4 nameserver in /etc/resolv.conf, falling back to a public resolver.
Ip4 read_host_dns() {
  std::ifstream in("/etc/resolv.conf");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::string key, value;
    ss >> key >> value;
    in_addr a{};
    if (key == "nameserver" && inet_pton(AF_INET, value.c_str(), &a) == 1) return ntohl(a.s_addr);
  }
  return ip4(1, 1, 1, 1);
}

}  // namespace

struct UserNet::TcpConn {
  int fd = -1;
  uint16_t guest_port = 0;
  Ip4 remote = 0;  // as the guest sees it
  uint16_t remote_port = 0;
  bool connecting = true;
  uint32_t snd_una = 0, snd_nxt = 0;  // our sequence space
  uint32_t rcv_nxt = 0;               // next byte expected from the guest
  uint32_t guest_window = 0;  // bytes, already scaled
  uint8_t guest_wscale = 0;   // RFC 7323 window scale the guest asked for
  bool wscale = false;        // both sides agreed to scale windows
  std::deque<uint8_t> unacked;  // data sent to the guest from snd_una on, for resending
  UserNet::Clock::time_point rto_deadline{};
  std::chrono::milliseconds rto = kMinRto;
  int retransmits = 0;
  int dup_acks = 0;
  std::vector<uint8_t> to_host;
  bool host_eof = false, fin_sent = false, guest_fin = false, shut_wr = false;
  bool dead = false;
  bool syn_sent = false;  // we opened this connection (port forward), waiting for SYN-ACK
};

struct UserNet::UdpFlow {
  int fd = -1;
  uint16_t guest_port = 0;
  Clock::time_point last_used;
};

UserNet::UserNet(SendFn send) : send_(std::move(send)), host_dns_(read_host_dns()) {
  if (pipe(wake_pipe_) != 0) log(LogLevel::kError, "net: pipe() failed");
  set_nonblocking(wake_pipe_[0]);
  set_nonblocking(wake_pipe_[1]);
  // Unprivileged ICMP echo socket (supported by macOS and Linux).
  icmp_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
  if (icmp_fd_ >= 0) set_nonblocking(icmp_fd_);
  thread_ = std::thread([this] { loop(); });
}

bool UserNet::add_port_forward(uint16_t host_port, uint16_t guest_port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  const sockaddr_in addr = make_addr(ip4(127, 0, 0, 1), host_port);
  if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 8) != 0) {
    close(fd);
    return false;
  }
  set_nonblocking(fd);
  forwards_.push_back({fd, guest_port});
  return true;
}

UserNet::~UserNet() {
  quit_ = true;
  (void)!write(wake_pipe_[1], "q", 1);
  thread_.join();
  for (auto& [k, c] : tcp_) close(c->fd);
  for (auto& [k, f] : udp_) close(f->fd);
  for (auto& f : forwards_) close(f.fd);
  if (icmp_fd_ >= 0) close(icmp_fd_);
  close(wake_pipe_[0]);
  close(wake_pipe_[1]);
}

void UserNet::from_guest(std::span<const uint8_t> frame) {
  {
    std::lock_guard lock(queue_mutex_);
    if (inbox_.size() >= kMaxInbox) return;  // guest outruns us: drop, TCP will resend
    inbox_.emplace_back(frame.begin(), frame.end());
  }
  (void)!write(wake_pipe_[1], "w", 1);
}

void UserNet::loop() {
  // Game traffic (Free Fire's UDP) passes here packet by packet: a slow core or a
  // late wake-up is added ping, so this runs like the vCPUs, not like disk I/O.
  set_thread_role(ThreadRole::kInteractive);
  std::vector<pollfd> fds;
  enum class Kind { kWake, kIcmp, kUdp, kTcp, kListen };
  std::vector<std::pair<Kind, uint64_t>> owners;
  auto last_housekeeping = Clock::now();

  while (!quit_) {
    fds.clear();
    owners.clear();
    fds.push_back({wake_pipe_[0], POLLIN, 0});
    owners.push_back({Kind::kWake, 0});
    if (icmp_fd_ >= 0) {
      fds.push_back({icmp_fd_, POLLIN, 0});
      owners.push_back({Kind::kIcmp, 0});
    }
    for (auto& [port, f] : udp_) {
      fds.push_back({f->fd, POLLIN, 0});
      owners.push_back({Kind::kUdp, port});
    }
    for (size_t i = 0; i < forwards_.size(); ++i) {
      fds.push_back({forwards_[i].fd, POLLIN, 0});
      owners.push_back({Kind::kListen, i});
    }
    for (auto& [key, c] : tcp_) {
      short ev = 0;
      if (c->syn_sent) {
        ev = 0;  // nothing to do until the guest answers
      } else if (c->connecting) {
        ev = POLLOUT;
      } else {
        const uint32_t in_flight = c->snd_nxt - c->snd_una;
        if (!c->host_eof && in_flight < std::min(c->guest_window, kMaxInFlight)) ev |= POLLIN;
        if (!c->to_host.empty()) ev |= POLLOUT;
      }
      // A negative fd is ignored by poll(); otherwise POLLHUP after EOF would spin.
      fds.push_back({ev ? c->fd : -1, ev, 0});
      owners.push_back({Kind::kTcp, key});
    }

    poll(fds.data(), static_cast<nfds_t>(fds.size()), next_timer_ms());

    for (size_t i = 0; i < fds.size(); ++i) {
      if (!fds[i].revents) continue;
      auto [kind, key] = owners[i];
      if (kind == Kind::kWake) {
        char buf[256];
        while (read(wake_pipe_[0], buf, sizeof(buf)) > 0) {
        }
      } else if (kind == Kind::kIcmp) {
        poll_icmp();
      } else if (kind == Kind::kListen) {
        accept_forward(key);
      } else if (kind == Kind::kUdp) {
        if (auto it = udp_.find(static_cast<uint16_t>(key)); it != udp_.end()) poll_udp(*it->second);
      } else if (auto it = tcp_.find(key); it != tcp_.end()) {
        TcpConn& c = *it->second;
        if (c.connecting) {
          tcp_connected(c);
        } else {
          if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) tcp_readable(c);
          if (!c.dead && (fds[i].revents & POLLOUT)) tcp_writable(c);
        }
      }
    }

    std::deque<std::vector<uint8_t>> frames;
    {
      std::lock_guard lock(queue_mutex_);
      frames.swap(inbox_);
    }
    for (const auto& f : frames) handle_frame(f);
    tcp_timers();

    std::erase_if(tcp_, [](const auto& kv) {
      if (kv.second->dead) close(kv.second->fd);
      return kv.second->dead;
    });
    if (Clock::now() - last_housekeeping > std::chrono::seconds(5)) {
      housekeeping();
      last_housekeeping = Clock::now();
    }
  }
}

void UserNet::housekeeping() {
  const auto now = Clock::now();
  std::erase_if(udp_, [&](const auto& kv) {
    if (now - kv.second->last_used < kUdpIdleTimeout) return false;
    close(kv.second->fd);
    return true;
  });
}

Ip4 UserNet::host_address_for(Ip4 guest_dst) const {
  if (guest_dst == kGateway) return ip4(127, 0, 0, 1);
  if (guest_dst == kDns) return host_dns_;
  return guest_dst;
}

Ip4 UserNet::guest_address_for(Ip4 host_src, uint16_t port) const {
  if (host_src == ip4(127, 0, 0, 1)) return kGateway;
  if (host_src == host_dns_ && port == 53) return kDns;
  return host_src;
}

// ---- frame dispatch ----

void UserNet::handle_frame(std::span<const uint8_t> frame) {
  if (frame.size() < kEthHeader) return;
  if (!guest_mac_known_) {
    std::copy(frame.begin() + 6, frame.begin() + 12, guest_mac_.begin());
    guest_mac_known_ = true;
  }
  switch (get16(frame, 12)) {
    case kEtherTypeArp: handle_arp(frame); break;
    case kEtherTypeIp4: handle_ip(frame); break;
    default: break;  // IPv6 etc. not supported yet
  }
}

void UserNet::handle_arp(std::span<const uint8_t> frame) {
  auto arp = frame.subspan(kEthHeader);
  if (arp.size() < 28 || get16(arp, 0) != 1 || get16(arp, 2) != kEtherTypeIp4 ||
      get16(arp, 6) != 1 /*request*/) {
    return;
  }
  const Ip4 target = get32(arp, 24);
  // Answer for every address on our subnet except the guest's own.
  if ((target & 0xffffff00) != kNetwork || target == kGuest) return;
  std::vector<uint8_t> reply(kEthHeader + 28);
  std::span<uint8_t> r(reply);
  std::copy(arp.begin() + 8, arp.begin() + 14, reply.begin());  // to the sender's MAC
  std::copy(kGatewayMac.begin(), kGatewayMac.end(), reply.begin() + 6);
  put16(r, 12, kEtherTypeArp);
  auto a = r.subspan(kEthHeader);
  put16(a, 0, 1);
  put16(a, 2, kEtherTypeIp4);
  a[4] = 6;
  a[5] = 4;
  put16(a, 6, 2);  // reply
  std::copy(kGatewayMac.begin(), kGatewayMac.end(), a.begin() + 8);
  put32(a, 14, target);
  std::copy(arp.begin() + 8, arp.begin() + 18, a.begin() + 18);  // sender MAC + IP
  send_(std::move(reply));
}

void UserNet::handle_ip(std::span<const uint8_t> frame) {
  auto ip = frame.subspan(kEthHeader);
  if (ip.size() < kIpHeader || (ip[0] >> 4) != 4) return;
  const size_t ihl = (ip[0] & 0xf) * 4u;
  const size_t total = get16(ip, 2);
  if (ihl < kIpHeader || total < ihl || total > ip.size()) return;
  if ((get16(ip, 6) & 0x3fff) != 0) return;  // fragments aren't supported
  const Ip4 src = get32(ip, 12), dst = get32(ip, 16);
  auto l4 = ip.subspan(ihl, total - ihl);
  switch (ip[9]) {
    case kProtoUdp: handle_udp(src, dst, l4); break;
    case kProtoTcp: handle_tcp(src, dst, l4); break;
    case kProtoIcmp: handle_icmp(dst, l4); break;
    default: break;
  }
}

void UserNet::send_ip(Ip4 src, Ip4 dst, uint8_t proto, std::span<const uint8_t> l4) {
  static constexpr Mac kBroadcast = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  send_(build_ip4_frame(guest_mac_known_ ? guest_mac_ : kBroadcast, kGatewayMac, src, dst, proto,
                        l4, ip_id_++));
}

// ---- UDP (+ DHCP) ----

void UserNet::send_udp(Ip4 src, uint16_t sport, Ip4 dst, uint16_t dport,
                       std::span<const uint8_t> data) {
  std::vector<uint8_t> udp(8 + data.size());
  std::span<uint8_t> u(udp);
  put16(u, 0, sport);
  put16(u, 2, dport);
  put16(u, 4, static_cast<uint16_t>(udp.size()));
  std::copy(data.begin(), data.end(), udp.begin() + 8);
  put16(u, 6, l4_checksum(src, dst, kProtoUdp, udp));
  send_ip(src, dst, kProtoUdp, udp);
}

void UserNet::handle_udp(Ip4 /*src*/, Ip4 dst, std::span<const uint8_t> udp) {
  if (udp.size() < 8) return;
  const uint16_t sport = get16(udp, 0), dport = get16(udp, 2);
  const size_t len = get16(udp, 4);
  if (len < 8 || len > udp.size()) return;
  auto payload = udp.subspan(8, len - 8);

  if (dport == 67) {
    handle_dhcp(payload);
    return;
  }
  if (dst == ip4(255, 255, 255, 255)) return;

  auto& flow = udp_[sport];
  if (!flow) {
    flow = std::make_unique<UdpFlow>();
    flow->guest_port = sport;
    flow->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (flow->fd < 0) {
      udp_.erase(sport);
      return;
    }
    {
      // Game traffic comes in bursts: room for them instead of drops.
      const int size = 1 << 20;
      setsockopt(flow->fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
      setsockopt(flow->fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    }
    set_nonblocking(flow->fd);
  }
  flow->last_used = Clock::now();
  const sockaddr_in to = make_addr(host_address_for(dst), dport);
  sendto(flow->fd, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr*>(&to),
         sizeof(to));
}

void UserNet::poll_udp(UdpFlow& flow) {
  uint8_t buf[65536];
  while (true) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const ssize_t n = recvfrom(flow.fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from),
                               &from_len);
    if (n < 0) return;
    flow.last_used = Clock::now();
    const uint16_t port = ntohs(from.sin_port);
    send_udp(guest_address_for(ntohl(from.sin_addr.s_addr), port), port, kGuest, flow.guest_port,
             std::span<const uint8_t>(buf, static_cast<size_t>(n)));
  }
}

void UserNet::handle_dhcp(std::span<const uint8_t> p) {
  // BOOTP (RFC 2131): fixed 236-byte header, then the magic cookie and options.
  if (p.size() < 240 || p[0] != 1 || get32(p, 236) != 0x63825363) return;
  uint8_t msg_type = 0;
  for (size_t i = 240; i < p.size();) {
    const uint8_t opt = p[i];
    if (opt == 255) break;
    if (opt == 0) {
      ++i;
      continue;
    }
    if (i + 1 >= p.size() || i + 2 + p[i + 1] > p.size()) break;
    if (opt == 53 && p[i + 1] >= 1) msg_type = p[i + 2];
    i += 2 + p[i + 1];
  }
  uint8_t reply_type;
  if (msg_type == 1) reply_type = 2;       // DISCOVER -> OFFER
  else if (msg_type == 3) reply_type = 5;  // REQUEST -> ACK
  else return;

  std::vector<uint8_t> r(240);
  std::span<uint8_t> s(r);
  r[0] = 2;  // BOOTREPLY
  r[1] = 1;
  r[2] = 6;
  std::copy(p.begin() + 4, p.begin() + 8, r.begin() + 4);    // xid
  std::copy(p.begin() + 10, p.begin() + 12, r.begin() + 10);  // flags
  put32(s, 16, kGuest);                                       // yiaddr
  put32(s, 20, kGateway);                                     // siaddr
  std::copy(p.begin() + 28, p.begin() + 44, r.begin() + 28);  // chaddr
  put32(s, 236, 0x63825363);
  auto opt_ip = [&](uint8_t code, Ip4 v) {
    r.insert(r.end(), {code, 4, uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)});
  };
  r.insert(r.end(), {53, 1, reply_type});
  opt_ip(54, kGateway);                   // server id
  opt_ip(51, 86400);                      // lease time (seconds)
  opt_ip(1, ip4(255, 255, 255, 0));       // netmask
  opt_ip(3, kGateway);                    // router
  opt_ip(6, kDns);                        // DNS server
  r.push_back(255);
  send_udp(kGateway, 67, ip4(255, 255, 255, 255), 68, r);
}

// ---- ICMP echo ----

void UserNet::handle_icmp(Ip4 dst, std::span<const uint8_t> icmp) {
  if (icmp.size() < 8 || icmp[0] != 8 /*echo request*/ || icmp_fd_ < 0) return;
  const sockaddr_in to = make_addr(host_address_for(dst), 0);
  sendto(icmp_fd_, icmp.data(), icmp.size(), 0, reinterpret_cast<const sockaddr*>(&to),
         sizeof(to));
}

void UserNet::poll_icmp() {
  uint8_t buf[65536];
  while (true) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const ssize_t n = recvfrom(icmp_fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from),
                               &from_len);
    if (n <= 0) return;
    std::span<uint8_t> msg(buf, static_cast<size_t>(n));
    // macOS includes the IP header on ICMP datagram sockets; Linux doesn't.
    if (msg.size() >= kIpHeader && (msg[0] >> 4) == 4) msg = msg.subspan((msg[0] & 0xf) * 4u);
    if (msg.size() < 8 || msg[0] != 0 /*echo reply*/) continue;
    put16(msg, 2, 0);
    put16(msg, 2, checksum_finish(checksum_add(0, msg)));
    send_ip(guest_address_for(ntohl(from.sin_addr.s_addr), 0), kGuest, kProtoIcmp, msg);
  }
}

// ---- TCP ----
// The guest's TCP connection is terminated here and re-originated from a host socket.
// Data sent to the guest is kept until acknowledged and resent on timeout or three
// duplicate ACKs: frames can be dropped when the guest's receive ring is full.

void UserNet::send_tcp(const TcpConn& c, uint8_t flags, uint32_t seq,
                       std::span<const uint8_t> data, bool mss_option) {
  // SYN options: MSS, plus window scale 0 when the guest offered scaling (RFC 7323 §2.2).
  const bool ws = mss_option && c.wscale;
  const size_t hdr = mss_option ? (ws ? 28 : 24) : 20;
  std::vector<uint8_t> seg(hdr + data.size());
  std::span<uint8_t> s(seg);
  put16(s, 0, c.remote_port);
  put16(s, 2, c.guest_port);
  put32(s, 4, seq);
  put32(s, 8, c.rcv_nxt);
  s[12] = static_cast<uint8_t>((hdr / 4) << 4);
  s[13] = flags;
  const size_t free = kMaxToHost - std::min(kMaxToHost, c.to_host.size());
  put16(s, 14, static_cast<uint16_t>(std::min<size_t>(free, 65535)));
  if (mss_option) {
    s[20] = 2;
    s[21] = 4;
    put16(s, 22, kMss);
    if (ws) {
      s[24] = 1;  // NOP
      s[25] = 3;  // window scale
      s[26] = 3;
      s[27] = 0;  // we don't scale our own window
    }
  }
  std::copy(data.begin(), data.end(), seg.begin() + hdr);
  put16(s, 16, l4_checksum(c.remote, kGuest, kProtoTcp, seg));
  send_ip(c.remote, kGuest, kProtoTcp, seg);
}

void UserNet::tcp_reset(TcpConn& c) {
  send_tcp(c, kTcpRst | kTcpAck, c.snd_nxt, {});
  c.dead = true;
}

void UserNet::handle_tcp(Ip4 /*src*/, Ip4 dst, std::span<const uint8_t> tcp) {
  if (tcp.size() < 20) return;
  const uint16_t sport = get16(tcp, 0), dport = get16(tcp, 2);
  const uint32_t seq = get32(tcp, 4), ack = get32(tcp, 8);
  const size_t off = (tcp[12] >> 4) * 4u;
  const uint8_t flags = tcp[13];
  const uint16_t window = get16(tcp, 14);
  if (off < 20 || off > tcp.size()) return;
  auto payload = tcp.subspan(off);
  const uint64_t key = tcp_key(sport, dst, dport);
  auto it = tcp_.find(key);

  if (it == tcp_.end()) {
    if ((flags & (kTcpSyn | kTcpAck | kTcpRst)) != kTcpSyn) {
      if (!(flags & kTcpRst)) {  // stray segment: reset it
        TcpConn tmp;
        tmp.guest_port = sport;
        tmp.remote = dst;
        tmp.remote_port = dport;
        tmp.rcv_nxt = seq + static_cast<uint32_t>(payload.size()) + ((flags & kTcpFin) ? 1 : 0);
        send_tcp(tmp, kTcpRst | kTcpAck, (flags & kTcpAck) ? ack : 0, {});
      }
      return;
    }
    auto c = std::make_unique<TcpConn>();
    c->guest_port = sport;
    c->remote = dst;
    c->remote_port = dport;
    c->rcv_nxt = seq + 1;
    c->guest_window = window;  // SYN windows are never scaled
    // TCP options: look for window scale (kind 3). All lengths are bounds-checked.
    for (size_t i = 20; i < off;) {
      const uint8_t kind = tcp[i];
      if (kind == 0) break;
      if (kind == 1) {
        ++i;
        continue;
      }
      if (i + 1 >= off || tcp[i + 1] < 2 || i + tcp[i + 1] > off) break;
      if (kind == 3 && tcp[i + 1] == 3) {
        c->wscale = true;
        c->guest_wscale = std::min<uint8_t>(tcp[i + 2], 14);
      }
      i += tcp[i + 1];
    }
    static std::mt19937 rng{std::random_device{}()};
    c->snd_una = c->snd_nxt = static_cast<uint32_t>(rng());
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) return;
    no_sigpipe(c->fd);
    set_nonblocking(c->fd);
    const int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    const sockaddr_in to = make_addr(host_address_for(dst), dport);
    if (connect(c->fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0 &&
        errno != EINPROGRESS) {
      tcp_reset(*c);
      close(c->fd);
      return;
    }
    tcp_.emplace(key, std::move(c));
    return;  // SYN-ACK is sent once the host connection completes
  }

  TcpConn& c = *it->second;
  if (flags & kTcpRst) {
    c.dead = true;
    return;
  }
  if (c.syn_sent) {  // our SYN to the guest (port forward): expect SYN-ACK
    if ((flags & (kTcpSyn | kTcpAck)) != (kTcpSyn | kTcpAck) || ack != c.snd_nxt) {
      tcp_reset(c);
      return;
    }
    c.syn_sent = false;
    c.rcv_nxt = seq + 1;
    c.snd_una = ack;
    c.guest_window = window;
    send_tcp(c, kTcpAck, c.snd_nxt, {});
    return;
  }
  if (c.connecting) return;  // duplicate SYN while we're still connecting
  if (flags & kTcpSyn) {     // retransmitted SYN: our SYN-ACK was lost
    send_tcp(c, kTcpSyn | kTcpAck, c.snd_una, {}, true);
    return;
  }

  const uint32_t scaled_window = c.wscale ? (uint32_t{window} << c.guest_wscale) : window;
  if ((flags & kTcpAck) && seq_lt(c.snd_una, ack) && seq_le(ack, c.snd_nxt)) {
    const uint32_t acked = ack - c.snd_una;
    c.unacked.erase(c.unacked.begin(), c.unacked.begin() + std::min<size_t>(acked, c.unacked.size()));
    c.snd_una = ack;
    c.rto = kMinRto;
    c.retransmits = 0;
    c.dup_acks = 0;
    c.rto_deadline = Clock::now() + c.rto;
  } else if ((flags & kTcpAck) && ack == c.snd_una && c.snd_una != c.snd_nxt && payload.empty() &&
             !(flags & (kTcpFin | kTcpSyn)) && scaled_window == c.guest_window) {
    if (++c.dup_acks == 3) tcp_retransmit(c, kMss);  // fast retransmit (RFC 5681 §3.2)
  }
  c.guest_window = scaled_window;

  bool need_ack = false;
  if (!payload.empty() || (flags & kTcpFin)) {
    if (seq == c.rcv_nxt) {
      if (!payload.empty()) {
        if (c.to_host.size() + payload.size() > kMaxToHost) {
          send_tcp(c, kTcpAck, c.snd_nxt, {});  // no room: guest will retransmit
          return;
        }
        c.to_host.insert(c.to_host.end(), payload.begin(), payload.end());
        c.rcv_nxt += static_cast<uint32_t>(payload.size());
        tcp_writable(c);
      }
      if (flags & kTcpFin) {
        c.rcv_nxt += 1;
        c.guest_fin = true;
        if (c.to_host.empty() && !c.shut_wr) {
          shutdown(c.fd, SHUT_WR);
          c.shut_wr = true;
        }
      }
    }
    need_ack = true;  // also re-ACK out-of-order or duplicate segments
  }
  if (need_ack && !c.dead) send_tcp(c, kTcpAck, c.snd_nxt, {});
  if (c.fin_sent && c.guest_fin && c.snd_una == c.snd_nxt) c.dead = true;
}

void UserNet::accept_forward(size_t index) {
  const int fd = accept(forwards_[index].fd, nullptr, nullptr);
  if (fd < 0) return;
  no_sigpipe(fd);
  if (!guest_mac_known_) {  // the guest hasn't networked yet: nobody to connect to
    close(fd);
    return;
  }
  set_nonblocking(fd);
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  auto c = std::make_unique<TcpConn>();
  c->fd = fd;
  c->guest_port = forwards_[index].guest_port;
  c->remote = kGateway;  // the guest sees the connection coming from the host
  c->remote_port = next_forward_port_;
  next_forward_port_ = next_forward_port_ >= 60999 ? 40000 : next_forward_port_ + 1;
  c->connecting = false;
  c->syn_sent = true;
  static std::mt19937 rng{std::random_device{}()};
  c->snd_una = c->snd_nxt = static_cast<uint32_t>(rng());
  c->guest_window = 65535;
  send_tcp(*c, kTcpSyn, c->snd_nxt, {}, true);
  c->snd_nxt += 1;
  tcp_.emplace(tcp_key(c->guest_port, c->remote, c->remote_port), std::move(c));
}

void UserNet::tcp_connected(TcpConn& c) {
  int err = 0;
  socklen_t len = sizeof(err);
  getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
  if (err != 0) {
    tcp_reset(c);
    return;
  }
  c.connecting = false;
  send_tcp(c, kTcpSyn | kTcpAck, c.snd_nxt, {}, true);
  c.snd_nxt += 1;
  c.snd_una = c.snd_nxt;  // the guest's final ACK isn't needed to start
}

void UserNet::tcp_readable(TcpConn& c) {
  const uint32_t in_flight = c.snd_nxt - c.snd_una;
  const uint32_t window = std::min(c.guest_window, kMaxInFlight);
  if (c.host_eof || in_flight >= window) return;
  uint8_t buf[65536];
  const ssize_t n = recv(c.fd, buf, std::min<size_t>(sizeof(buf), window - in_flight), 0);
  if (n < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) tcp_reset(c);
    return;
  }
  if (n == 0) {
    c.host_eof = true;
    send_tcp(c, kTcpFin | kTcpAck, c.snd_nxt, {});
    if (c.snd_una == c.snd_nxt) c.rto_deadline = Clock::now() + c.rto;
    c.snd_nxt += 1;
    c.fin_sent = true;
    return;
  }
  if (c.snd_una == c.snd_nxt) c.rto_deadline = Clock::now() + c.rto;  // timer starts
  c.unacked.insert(c.unacked.end(), buf, buf + n);
  for (ssize_t off = 0; off < n; off += kMss) {
    const size_t len = std::min<size_t>(kMss, static_cast<size_t>(n - off));
    send_tcp(c, kTcpAck | kTcpPsh, c.snd_nxt, std::span<const uint8_t>(buf + off, len));
    c.snd_nxt += static_cast<uint32_t>(len);
  }
}

// Resends up to `max_bytes` of unacknowledged data from snd_una, or the FIN if only
// the FIN is outstanding.
void UserNet::tcp_retransmit(TcpConn& c, size_t max_bytes) {
  const size_t len = std::min({c.unacked.size(), max_bytes, size_t{c.guest_window ? c.guest_window : kMss}});
  std::vector<uint8_t> seg;
  for (size_t off = 0; off < len; off += kMss) {
    const size_t n = std::min<size_t>(kMss, len - off);
    seg.assign(c.unacked.begin() + static_cast<ptrdiff_t>(off),
               c.unacked.begin() + static_cast<ptrdiff_t>(off + n));
    send_tcp(c, kTcpAck | kTcpPsh, c.snd_una + static_cast<uint32_t>(off), seg);
  }
  if (c.fin_sent && c.unacked.size() <= len && c.snd_una + c.unacked.size() + 1 == c.snd_nxt) {
    send_tcp(c, kTcpFin | kTcpAck, c.snd_nxt - 1, {});
  }
}

void UserNet::tcp_timers() {
  const auto now = Clock::now();
  for (auto& [key, cp] : tcp_) {
    TcpConn& c = *cp;
    if (c.dead || c.connecting || c.syn_sent || c.snd_una == c.snd_nxt || now < c.rto_deadline) continue;
    // A zero window means the app isn't reading yet: resends are just probes then.
    if (c.guest_window != 0 && ++c.retransmits > kMaxRetransmits) {
      tcp_reset(c);
      continue;
    }
    tcp_retransmit(c, 16 * kMss);
    c.rto = std::min(c.rto * 2, kMaxRto);  // exponential backoff (RFC 6298 §5.5)
    c.rto_deadline = now + c.rto;
  }
}

int UserNet::next_timer_ms() const {
  auto next = Clock::now() + std::chrono::seconds(1);
  for (const auto& [key, cp] : tcp_) {
    const TcpConn& c = *cp;
    if (!c.dead && !c.connecting && !c.syn_sent && c.snd_una != c.snd_nxt) next = std::min(next, c.rto_deadline);
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count();
  return static_cast<int>(std::clamp<long long>(ms, 0, 1000));
}

void UserNet::tcp_writable(TcpConn& c) {
  if (c.to_host.empty()) return;
  const ssize_t n = send(c.fd, c.to_host.data(), c.to_host.size(), 0);
  if (n < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) tcp_reset(c);
    return;
  }
  const bool was_full = c.to_host.size() >= kMaxToHost / 2;
  c.to_host.erase(c.to_host.begin(), c.to_host.begin() + n);
  if (was_full) send_tcp(c, kTcpAck, c.snd_nxt, {});  // window update
  if (c.to_host.empty() && c.guest_fin && !c.shut_wr) {
    shutdown(c.fd, SHUT_WR);
    c.shut_wr = true;
  }
}

}  // namespace kestrel::net
