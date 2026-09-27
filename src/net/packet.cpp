#include "net/packet.h"

namespace kestrel::net {

uint32_t checksum_add(uint32_t sum, std::span<const uint8_t> data) {
  size_t i = 0;
  for (; i + 1 < data.size(); i += 2) sum += (uint32_t(data[i]) << 8) | data[i + 1];
  if (i < data.size()) sum += uint32_t(data[i]) << 8;
  return sum;
}

uint16_t checksum_finish(uint32_t sum) {
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}

uint16_t l4_checksum(Ip4 src, Ip4 dst, uint8_t proto, std::span<const uint8_t> segment) {
  uint8_t pseudo[12];
  put32(pseudo, 0, src);
  put32(pseudo, 4, dst);
  pseudo[8] = 0;
  pseudo[9] = proto;
  put16(pseudo, 10, static_cast<uint16_t>(segment.size()));
  const uint16_t c = checksum_finish(checksum_add(checksum_add(0, pseudo), segment));
  // UDP transmits a computed zero as all ones (RFC 768).
  return (proto == kProtoUdp && c == 0) ? 0xffff : c;
}

std::vector<uint8_t> build_ip4_frame(const Mac& dst_mac, const Mac& src_mac, Ip4 src, Ip4 dst,
                                     uint8_t proto, std::span<const uint8_t> l4, uint16_t ip_id) {
  std::vector<uint8_t> f(kEthHeader + kIpHeader + l4.size());
  std::span<uint8_t> s(f);
  std::copy(dst_mac.begin(), dst_mac.end(), f.begin());
  std::copy(src_mac.begin(), src_mac.end(), f.begin() + 6);
  put16(s, 12, kEtherTypeIp4);
  auto ip = s.subspan(kEthHeader, kIpHeader);
  ip[0] = 0x45;  // IPv4, 20-byte header
  ip[1] = 0;
  put16(ip, 2, static_cast<uint16_t>(kIpHeader + l4.size()));
  put16(ip, 4, ip_id);
  put16(ip, 6, 0x4000);  // don't fragment
  ip[8] = 64;            // TTL
  ip[9] = proto;
  put16(ip, 10, 0);
  put32(ip, 12, src);
  put32(ip, 16, dst);
  put16(ip, 10, checksum_finish(checksum_add(0, ip)));
  std::copy(l4.begin(), l4.end(), f.begin() + kEthHeader + kIpHeader);
  return f;
}

}  // namespace kestrel::net
