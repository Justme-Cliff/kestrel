#pragma once

// Byte-order and checksum helpers for building and parsing Ethernet/IPv4 frames.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <sys/socket.h>

namespace kestrel::net {

// A peer that closed its end must turn our writes into EPIPE errors, not a
// SIGPIPE that kills the whole emulator.
inline void no_sigpipe(int fd) {
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
}

using Mac = std::array<uint8_t, 6>;
using Ip4 = uint32_t;  // host byte order

constexpr Ip4 ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | d;
}

inline uint16_t get16(std::span<const uint8_t> p, size_t off) {
  return static_cast<uint16_t>((p[off] << 8) | p[off + 1]);
}
inline uint32_t get32(std::span<const uint8_t> p, size_t off) {
  return (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) | (uint32_t(p[off + 2]) << 8) |
         p[off + 3];
}
inline void put16(std::span<uint8_t> p, size_t off, uint16_t v) {
  p[off] = static_cast<uint8_t>(v >> 8);
  p[off + 1] = static_cast<uint8_t>(v);
}
inline void put32(std::span<uint8_t> p, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[off + i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}

// Internet checksum (RFC 1071) over `data`, continuing from `sum`.
uint32_t checksum_add(uint32_t sum, std::span<const uint8_t> data);
uint16_t checksum_finish(uint32_t sum);
// TCP/UDP checksum including the IPv4 pseudo-header.
uint16_t l4_checksum(Ip4 src, Ip4 dst, uint8_t proto, std::span<const uint8_t> segment);

constexpr uint16_t kEtherTypeIp4 = 0x0800;
constexpr uint16_t kEtherTypeArp = 0x0806;
constexpr uint8_t kProtoIcmp = 1;
constexpr uint8_t kProtoTcp = 6;
constexpr uint8_t kProtoUdp = 17;
constexpr size_t kEthHeader = 14;
constexpr size_t kIpHeader = 20;

// Builds Ethernet + IPv4 headers around `l4` (whose checksum must already be set).
std::vector<uint8_t> build_ip4_frame(const Mac& dst_mac, const Mac& src_mac, Ip4 src, Ip4 dst,
                                     uint8_t proto, std::span<const uint8_t> l4, uint16_t ip_id);

}  // namespace kestrel::net
