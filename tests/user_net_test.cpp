#include "net/user_net.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

using namespace kestrel::net;

namespace {

constexpr Mac kGuestMac = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

// Collects frames the NAT sends to the "guest".
struct Inbox {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::vector<uint8_t>> frames;

  void push(std::vector<uint8_t> f) {
    {
      std::lock_guard lock(mutex);
      frames.push_back(std::move(f));
    }
    cv.notify_all();
  }
  std::optional<std::vector<uint8_t>> pop(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock lock(mutex);
    if (!cv.wait_for(lock, timeout, [&] { return !frames.empty(); })) return std::nullopt;
    auto f = std::move(frames.front());
    frames.pop_front();
    return f;
  }
};

std::vector<uint8_t> guest_ip_frame(Ip4 dst, uint8_t proto, std::vector<uint8_t> l4) {
  if (proto == kProtoTcp || proto == kProtoUdp) {
    const size_t csum_off = proto == kProtoTcp ? 16 : 6;
    put16(l4, csum_off, 0);
    put16(l4, csum_off, l4_checksum(UserNet::kGuest, dst, proto, l4));
  }
  return build_ip4_frame(UserNet::kGatewayMac, kGuestMac, UserNet::kGuest, dst, proto, l4, 1);
}

std::vector<uint8_t> tcp_segment(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                                 uint8_t flags, std::string_view data = {}) {
  std::vector<uint8_t> t(20 + data.size());
  put16(t, 0, sport);
  put16(t, 2, dport);
  put32(t, 4, seq);
  put32(t, 8, ack);
  t[12] = 5 << 4;
  t[13] = flags;
  put16(t, 14, 65535);
  std::copy(data.begin(), data.end(), t.begin() + 20);
  return t;
}

std::span<const uint8_t> l4_of(const std::vector<uint8_t>& frame) {
  return std::span(frame).subspan(kEthHeader + kIpHeader);
}

}  // namespace

TEST(UserNet, AnswersArpForGateway) {
  Inbox inbox;
  UserNet net([&](std::vector<uint8_t> f) { inbox.push(std::move(f)); });
  std::vector<uint8_t> arp(kEthHeader + 28, 0);
  std::fill(arp.begin(), arp.begin() + 6, 0xff);
  std::copy(kGuestMac.begin(), kGuestMac.end(), arp.begin() + 6);
  put16(arp, 12, kEtherTypeArp);
  std::span<uint8_t> a = std::span(arp).subspan(kEthHeader);
  put16(a, 0, 1);
  put16(a, 2, kEtherTypeIp4);
  a[4] = 6;
  a[5] = 4;
  put16(a, 6, 1);
  std::copy(kGuestMac.begin(), kGuestMac.end(), a.begin() + 8);
  put32(a, 14, UserNet::kGuest);
  put32(a, 24, UserNet::kGateway);
  net.from_guest(arp);
  auto reply = inbox.pop();
  ASSERT_TRUE(reply);
  auto r = std::span<const uint8_t>(*reply).subspan(kEthHeader);
  EXPECT_EQ(get16(r, 6), 2);  // ARP reply
  EXPECT_TRUE(std::equal(UserNet::kGatewayMac.begin(), UserNet::kGatewayMac.end(), r.begin() + 8));
  EXPECT_EQ(get32(r, 14), UserNet::kGateway);
}

TEST(UserNet, DhcpOffersGuestAddress) {
  Inbox inbox;
  UserNet net([&](std::vector<uint8_t> f) { inbox.push(std::move(f)); });
  std::vector<uint8_t> bootp(240, 0);
  bootp[0] = 1;
  bootp[1] = 1;
  bootp[2] = 6;
  put32(bootp, 4, 0xdeadbeef);
  std::copy(kGuestMac.begin(), kGuestMac.end(), bootp.begin() + 28);
  put32(bootp, 236, 0x63825363);
  bootp.insert(bootp.end(), {53, 1, 1, 255});  // DHCPDISCOVER
  std::vector<uint8_t> udp(8 + bootp.size());
  put16(udp, 0, 68);
  put16(udp, 2, 67);
  put16(udp, 4, static_cast<uint16_t>(udp.size()));
  std::copy(bootp.begin(), bootp.end(), udp.begin() + 8);
  net.from_guest(guest_ip_frame(ip4(255, 255, 255, 255), kProtoUdp, udp));
  auto reply = inbox.pop();
  ASSERT_TRUE(reply);
  auto u = l4_of(*reply);
  auto b = u.subspan(8);
  EXPECT_EQ(b[0], 2);
  EXPECT_EQ(get32(b, 4), 0xdeadbeefu);
  EXPECT_EQ(get32(b, 16), UserNet::kGuest);
  EXPECT_EQ(b[240], 53);
  EXPECT_EQ(b[242], 2);  // DHCPOFFER
}

TEST(UserNet, TcpToHostLoopbackThroughGateway) {
  // A host server on 127.0.0.1, reached by the guest as 10.0.2.2.
  int server = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  socklen_t len = sizeof(addr);
  getsockname(server, reinterpret_cast<sockaddr*>(&addr), &len);
  const uint16_t port = ntohs(addr.sin_port);
  listen(server, 1);
  std::thread echo([&] {
    int c = accept(server, nullptr, nullptr);
    char buf[64];
    ssize_t n = recv(c, buf, sizeof(buf), 0);
    send(c, "pong:", 5, 0);
    send(c, buf, static_cast<size_t>(n), 0);
    close(c);
  });

  Inbox inbox;
  UserNet net([&](std::vector<uint8_t> f) { inbox.push(std::move(f)); });
  const uint32_t iss = 1000;
  net.from_guest(guest_ip_frame(UserNet::kGateway, kProtoTcp, tcp_segment(40000, port, iss, 0, 0x02)));
  auto synack = inbox.pop();
  ASSERT_TRUE(synack);
  auto sa = l4_of(*synack);
  EXPECT_EQ(sa[13] & 0x12, 0x12);  // SYN|ACK
  EXPECT_EQ(get32(sa, 8), iss + 1);
  EXPECT_EQ(l4_checksum(UserNet::kGateway, UserNet::kGuest, kProtoTcp, sa), 0);
  const uint32_t their = get32(sa, 4) + 1;

  net.from_guest(guest_ip_frame(UserNet::kGateway, kProtoTcp,
                                tcp_segment(40000, port, iss + 1, their, 0x18, "ping")));
  std::string received;
  bool fin = false;
  for (int i = 0; i < 10 && !fin; ++i) {
    auto f = inbox.pop();
    ASSERT_TRUE(f);
    auto t = l4_of(*f);
    const size_t off = (t[12] >> 4) * 4u;
    received.append(t.begin() + off, t.end());
    fin = t[13] & 0x01;
  }
  EXPECT_EQ(received, "pong:ping");
  EXPECT_TRUE(fin);
  echo.join();
  close(server);
}

TEST(UserNet, ForwardsHostPortIntoGuest) {
  Inbox inbox;
  UserNet net([&](std::vector<uint8_t> f) { inbox.push(std::move(f)); });
  uint16_t port = 0;
  for (uint16_t p = 47000; p < 47100 && !port; ++p) {
    if (net.add_port_forward(p, 5555)) port = p;
  }
  ASSERT_NE(port, 0);
  // The guest must have spoken first so the NAT knows its MAC.
  net.from_guest(guest_ip_frame(UserNet::kGateway, kProtoUdp, std::vector<uint8_t>(8, 0)));

  int host = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(connect(host, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

  auto syn = inbox.pop();
  ASSERT_TRUE(syn);
  auto s = l4_of(*syn);
  EXPECT_EQ(get16(s, 2), 5555);
  EXPECT_EQ(s[13] & 0x12, 0x02);  // SYN only
  const uint16_t their_port = get16(s, 0);
  const uint32_t their_seq = get32(s, 4);

  // Guest (the "adbd") accepts: SYN-ACK, then expects our ACK.
  const uint32_t guest_iss = 7000;
  net.from_guest(guest_ip_frame(UserNet::kGateway, kProtoTcp,
                                tcp_segment(5555, their_port, guest_iss, their_seq + 1, 0x12)));
  auto ack = inbox.pop();
  ASSERT_TRUE(ack);
  EXPECT_EQ(get32(l4_of(*ack), 8), guest_iss + 1);

  send(host, "hello", 5, 0);
  auto data = inbox.pop();
  ASSERT_TRUE(data);
  auto d = l4_of(*data);
  const size_t off = (d[12] >> 4) * 4u;
  EXPECT_EQ(std::string(d.begin() + off, d.end()), "hello");
  close(host);
}
