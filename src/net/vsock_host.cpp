#include "net/vsock_host.h"

#include "net/packet.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "core/log.h"

namespace kestrel::net {
namespace {

void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }
uint64_t conn_key(uint32_t local, uint32_t guest) { return (uint64_t(local) << 32) | guest; }

}  // namespace

struct VsockHost::Conn {
  int fd = -1;  // host TCP socket; -1 for a sink (guest-initiated) connection
  uint32_t local_port = 0, guest_port = 0;
  bool established = false;
  uint32_t tx_cnt = 0;                           // bytes we sent to the guest
  uint32_t peer_buf_alloc = 0, peer_fwd_cnt = 0;  // guest's receive credit
  uint32_t fwd_cnt = 0, credit_sent_at = 0;      // bytes we consumed from the guest
  std::vector<uint8_t> to_host;
  bool host_eof = false, guest_done_sending = false, dead = false;
};

VsockHost::VsockHost(uint64_t guest_cid, SendFn send) : guest_cid_(guest_cid), send_(std::move(send)) {
  if (pipe(wake_pipe_) != 0) log(LogLevel::kError, "vsock: pipe() failed");
  set_nonblocking(wake_pipe_[0]);
  set_nonblocking(wake_pipe_[1]);
  thread_ = std::thread([this] { loop(); });
}

VsockHost::~VsockHost() {
  quit_ = true;
  (void)!write(wake_pipe_[1], "q", 1);
  thread_.join();
  for (auto& [k, c] : conns_) {
    if (c->fd >= 0) close(c->fd);
  }
  for (auto& f : forwards_) close(f.fd);
  close(wake_pipe_[0]);
  close(wake_pipe_[1]);
}

bool VsockHost::add_forward(uint16_t host_tcp_port, uint32_t guest_port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_len = sizeof(addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(host_tcp_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable from the network
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 8) != 0) {
    close(fd);
    return false;
  }
  set_nonblocking(fd);
  forwards_.push_back({fd, guest_port});
  return true;
}

void VsockHost::accept_guest_connections(uint32_t host_port) { accepted_ports_.insert(host_port); }

void VsockHost::from_guest(VsockPacket packet) {
  {
    std::lock_guard lock(inbox_mutex_);
    if (inbox_.size() >= 4096) return;
    inbox_.push_back(std::move(packet));
  }
  (void)!write(wake_pipe_[1], "w", 1);
}

uint32_t VsockHost::peer_credit(const Conn& c) const {
  const uint32_t in_flight = c.tx_cnt - c.peer_fwd_cnt;
  return in_flight >= c.peer_buf_alloc ? 0 : c.peer_buf_alloc - in_flight;
}

void VsockHost::send_control(const Conn& c, uint16_t op, uint32_t flags) {
  send_(VsockPacket{VirtioVsock::kHostCid, guest_cid_, c.local_port, c.guest_port,
                    VsockPacket::kTypeStream, op, flags, kBufAlloc, c.fwd_cnt, {}});
}

void VsockHost::send_rst(const VsockPacket& to) {
  send_(VsockPacket{VirtioVsock::kHostCid, to.src_cid, to.dst_port, to.src_port,
                    VsockPacket::kTypeStream, VsockPacket::kRst, 0, 0, 0, {}});
}

void VsockHost::loop() {
  enum class Kind { kWake, kListen, kConn };
  std::vector<pollfd> fds;
  std::vector<std::pair<Kind, uint64_t>> owners;
  while (!quit_) {
    fds.clear();
    owners.clear();
    fds.push_back({wake_pipe_[0], POLLIN, 0});
    owners.push_back({Kind::kWake, 0});
    for (size_t i = 0; i < forwards_.size(); ++i) {
      fds.push_back({forwards_[i].fd, POLLIN, 0});
      owners.push_back({Kind::kListen, i});
    }
    for (auto& [key, c] : conns_) {
      if (c->fd < 0) continue;
      short ev = 0;
      if (c->established && !c->host_eof && peer_credit(*c) > 0) ev |= POLLIN;
      if (!c->to_host.empty()) ev |= POLLOUT;
      fds.push_back({ev ? c->fd : -1, ev, 0});  // negative fd: ignored (no POLLHUP spin)
      owners.push_back({Kind::kConn, key});
    }
    poll(fds.data(), static_cast<nfds_t>(fds.size()), 1000);

    for (size_t i = 0; i < fds.size(); ++i) {
      if (!fds[i].revents) continue;
      auto [kind, key] = owners[i];
      if (kind == Kind::kWake) {
        char buf[256];
        while (read(wake_pipe_[0], buf, sizeof(buf)) > 0) {
        }
      } else if (kind == Kind::kListen) {
        accept_forward(key);
      } else if (auto it = conns_.find(key); it != conns_.end()) {
        if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) conn_readable(*it->second);
        if (!it->second->dead && (fds[i].revents & POLLOUT)) conn_writable(*it->second);
      }
    }

    std::deque<VsockPacket> packets;
    {
      std::lock_guard lock(inbox_mutex_);
      packets.swap(inbox_);
    }
    for (auto& p : packets) handle(p);

    std::erase_if(conns_, [](const auto& kv) {
      if (kv.second->dead && kv.second->fd >= 0) close(kv.second->fd);
      return kv.second->dead;
    });
  }
}

void VsockHost::accept_forward(size_t index) {
  const int fd = accept(forwards_[index].fd, nullptr, nullptr);
  if (fd < 0) return;
  no_sigpipe(fd);
  set_nonblocking(fd);
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  auto c = std::make_unique<Conn>();
  c->fd = fd;
  c->local_port = next_local_port_;
  next_local_port_ = next_local_port_ >= 65535 ? 49152 : next_local_port_ + 1;
  c->guest_port = forwards_[index].guest_port;
  send_control(*c, VsockPacket::kRequest);
  log(LogLevel::kDebug, "vsock: host connection -> guest port {} (local port {})", c->guest_port,
      c->local_port);
  conns_.emplace(conn_key(c->local_port, c->guest_port), std::move(c));
}

void VsockHost::handle(VsockPacket& p) {
  if (p.op != VsockPacket::kRw && p.op != VsockPacket::kCreditUpdate) {
    log(LogLevel::kDebug, "vsock: guest {}:{} -> {}:{} op {} flags {}", p.src_cid, p.src_port,
        p.dst_cid, p.dst_port, p.op, p.flags);
  }
  if (p.type != VsockPacket::kTypeStream || p.dst_cid != VirtioVsock::kHostCid) {
    if (p.op != VsockPacket::kRst) send_rst(p);
    return;
  }
  const uint64_t key = conn_key(p.dst_port, p.src_port);
  auto it = conns_.find(key);

  if (it == conns_.end()) {
    if (p.op == VsockPacket::kRequest && accepted_ports_.contains(p.dst_port)) {
      auto c = std::make_unique<Conn>();
      c->local_port = p.dst_port;
      c->guest_port = p.src_port;
      c->established = true;
      c->peer_buf_alloc = p.buf_alloc;
      c->peer_fwd_cnt = p.fwd_cnt;
      send_control(*c, VsockPacket::kResponse);
      conns_.emplace(key, std::move(c));
    } else if (p.op != VsockPacket::kRst) {
      send_rst(p);  // nobody listening on that host port
    }
    return;
  }

  Conn& c = *it->second;
  c.peer_buf_alloc = p.buf_alloc;
  c.peer_fwd_cnt = p.fwd_cnt;
  switch (p.op) {
    case VsockPacket::kResponse:
      c.established = true;
      break;
    case VsockPacket::kRw:
      if (c.fd < 0) {  // sink: consume immediately
        c.fwd_cnt += static_cast<uint32_t>(p.payload.size());
      } else {
        c.to_host.insert(c.to_host.end(), p.payload.begin(), p.payload.end());
        conn_writable(c);
      }
      if (c.fwd_cnt - c.credit_sent_at >= kBufAlloc / 4) {
        send_control(c, VsockPacket::kCreditUpdate);
        c.credit_sent_at = c.fwd_cnt;
      }
      break;
    case VsockPacket::kCreditRequest:
      send_control(c, VsockPacket::kCreditUpdate);
      c.credit_sent_at = c.fwd_cnt;
      break;
    case VsockPacket::kShutdown:
      if (p.flags & VsockPacket::kShutdownSend) c.guest_done_sending = true;
      if (c.guest_done_sending && c.to_host.empty() && c.fd >= 0) shutdown(c.fd, SHUT_WR);
      if ((p.flags & (VsockPacket::kShutdownSend | VsockPacket::kShutdownRcv)) ==
          (VsockPacket::kShutdownSend | VsockPacket::kShutdownRcv)) {
        send_control(c, VsockPacket::kRst);
        c.dead = true;
      }
      break;
    case VsockPacket::kRst:
      c.dead = true;
      break;
    default:
      break;
  }
}

void VsockHost::conn_readable(Conn& c) {
  const uint32_t credit = peer_credit(c);
  if (c.host_eof || !c.established || credit == 0) return;
  uint8_t buf[64 * 1024];
  const ssize_t n = recv(c.fd, buf, std::min<size_t>(sizeof(buf), credit), 0);
  if (n < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      send_control(c, VsockPacket::kRst);
      c.dead = true;
    }
    return;
  }
  if (n == 0) {  // host side closed: we won't send or receive more
    c.host_eof = true;
    send_control(c, VsockPacket::kShutdown, VsockPacket::kShutdownSend | VsockPacket::kShutdownRcv);
    return;
  }
  VsockPacket p{VirtioVsock::kHostCid, guest_cid_, c.local_port, c.guest_port,
                VsockPacket::kTypeStream, VsockPacket::kRw, 0, kBufAlloc, c.fwd_cnt,
                std::vector<uint8_t>(buf, buf + n)};
  c.tx_cnt += static_cast<uint32_t>(n);
  send_(std::move(p));
}

void VsockHost::conn_writable(Conn& c) {
  if (c.to_host.empty()) return;
  const ssize_t n = send(c.fd, c.to_host.data(), c.to_host.size(), 0);
  if (n < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      send_control(c, VsockPacket::kRst);
      c.dead = true;
    }
    return;
  }
  c.to_host.erase(c.to_host.begin(), c.to_host.begin() + n);
  c.fwd_cnt += static_cast<uint32_t>(n);
  if (c.fwd_cnt - c.credit_sent_at >= kBufAlloc / 4) {
    send_control(c, VsockPacket::kCreditUpdate);
    c.credit_sent_at = c.fwd_cnt;
  }
  if (c.to_host.empty() && c.guest_done_sending) shutdown(c.fd, SHUT_WR);
}

}  // namespace kestrel::net
