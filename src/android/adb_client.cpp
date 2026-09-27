#include "android/adb_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <thread>
#include <vector>

namespace kestrel {
namespace {

constexpr uint32_t kCnxn = 0x4e584e43, kOpen = 0x4e45504f, kOkay = 0x59414b4f, kClse = 0x45534c43,
                   kWrte = 0x45545257, kAuth = 0x48545541;
constexpr uint32_t kVersion = 0x01000001;  // no payload checksums needed
constexpr uint32_t kMaxPayload = 256 * 1024;
constexpr uint32_t kLocalId = 1;

struct Message {
  uint32_t command, arg0, arg1;
  std::string data;
};

// One adb transport connection.
class Connection {
 public:
  ~Connection() {
    if (fd_ >= 0) ::close(fd_);
  }

  // `timeout_s`: longest silence to wait for between reads; 0 waits forever.
  Result<void> open(uint16_t port, int timeout_s) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return make_error("adb: no socket");
    const int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));  // closed peer: error, not SIGPIPE
    if (timeout_s > 0) {
      timeval tv{timeout_s, 0};
      ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    sockaddr_in a{};
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
      return make_error("adb: cannot connect");
    }
    const std::string banner("host::\0", 7);
    if (auto r = send(kCnxn, kVersion, kMaxPayload, banner); !r) return r;
    auto reply = receive();
    if (!reply) return std::unexpected(reply.error());
    if (reply->command == kAuth) return make_error("adb: device requires authorization");
    if (reply->command != kCnxn) return make_error("adb: bad handshake");
    max_payload_ = std::min(reply->arg1, kMaxPayload);
    return {};
  }

  Result<void> send(uint32_t cmd, uint32_t a0, uint32_t a1, std::string_view data) {
    uint32_t sum = 0;
    for (unsigned char c : data) sum += c;
    const uint32_t hdr[6] = {cmd, a0, a1, static_cast<uint32_t>(data.size()), sum, cmd ^ 0xffffffff};
    std::string buf(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    buf.append(data);
    size_t done = 0;
    while (done < buf.size()) {
      const ssize_t n = ::write(fd_, buf.data() + done, buf.size() - done);
      if (n <= 0) return make_error("adb: connection lost");
      done += static_cast<size_t>(n);
    }
    return {};
  }

  Result<Message> receive() {
    uint32_t hdr[6];
    if (!read_exact(reinterpret_cast<char*>(hdr), sizeof(hdr))) return make_error("adb: connection lost");
    if (hdr[3] > 16 * 1024 * 1024) return make_error("adb: oversized message");
    Message m{hdr[0], hdr[1], hdr[2], std::string(hdr[3], '\0')};
    if (!read_exact(m.data.data(), m.data.size())) return make_error("adb: connection lost");
    return m;
  }

  uint32_t max_payload() const { return max_payload_; }

 private:
  bool read_exact(char* p, size_t n) {
    while (n > 0) {
      const ssize_t r = ::read(fd_, p, n);
      if (r <= 0) return false;
      p += r;
      n -= static_cast<size_t>(r);
    }
    return true;
  }

  int fd_ = -1;
  uint32_t max_payload_ = 4096;
};

// Opens a service stream ("shell:...", "exec:...") and returns the remote id.
Result<uint32_t> open_stream(Connection& c, const std::string& service) {
  if (auto r = c.send(kOpen, kLocalId, 0, service + '\0'); !r) return std::unexpected(r.error());
  while (true) {
    auto m = c.receive();
    if (!m) return std::unexpected(m.error());
    if (m->command == kOkay) return m->arg0;
    if (m->command == kClse) return make_error("adb: service refused: " + service);
  }
}

// Reads a stream's output until the device closes it.
Result<std::string> read_until_close(Connection& c, uint32_t remote) {
  std::string out;
  while (true) {
    auto m = c.receive();
    if (!m) return std::unexpected(m.error());
    if (m->command == kWrte) {
      out += m->data;
      if (auto r = c.send(kOkay, kLocalId, remote, {}); !r) return std::unexpected(r.error());
    } else if (m->command == kClse) {
      c.send(kClse, kLocalId, remote, {});
      return out;
    }
  }
}

}  // namespace

Result<std::string> AdbClient::shell(const std::string& command) {
  Connection c;
  if (auto r = c.open(port_, timeout_s_ == 0 ? 0 : timeout_s_); !r) return std::unexpected(r.error());
  auto remote = open_stream(c, "shell:" + command);
  if (!remote) return std::unexpected(remote.error());
  return read_until_close(c, *remote);
}

Result<void> AdbClient::install(const std::string& apk_path,
                                const std::function<void(double)>& progress) {
  std::ifstream in(apk_path, std::ios::binary | std::ios::ate);
  if (!in) return make_error("cannot open " + apk_path);
  const auto size = static_cast<uint64_t>(in.tellg());
  in.seekg(0);

  Connection c;
  if (auto r = c.open(port_, timeout_s_ == 0 ? 0 : std::max(timeout_s_, 900)); !r) return r;
  // Streamed install: the package manager reads exactly `size` bytes from stdin.
  auto remote = open_stream(c, std::format("exec:cmd package install -S {}", size));
  if (!remote) return std::unexpected(remote.error());

  std::vector<char> buf(c.max_payload());
  std::string output;
  for (uint64_t sent = 0; sent < size;) {
    const auto n = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - sent));
    in.read(buf.data(), static_cast<std::streamsize>(n));
    if (!in) return make_error("read error on " + apk_path);
    if (auto r = c.send(kWrte, kLocalId, *remote, std::string_view(buf.data(), n)); !r) return r;
    // Wait for the device's OKAY before sending more (adb flow control).
    while (true) {
      auto m = c.receive();
      if (!m) return std::unexpected(m.error());
      if (m->command == kOkay) break;
      if (m->command == kWrte) {
        output += m->data;
        c.send(kOkay, kLocalId, *remote, {});
      }
      if (m->command == kClse) return make_error("install failed: " + output);
    }
    sent += n;
    if (progress) progress(double(sent) / double(size));
  }
  auto rest = read_until_close(c, *remote);
  if (!rest) return std::unexpected(rest.error());
  output += *rest;
  if (output.find("Success") == std::string::npos) {
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return make_error("install failed: " + output);
  }
  return {};
}

Result<void> AdbClient::push(const std::string& local_path, const std::string& remote_path) {
  std::ifstream in(local_path, std::ios::binary);
  if (!in) return make_error("cannot open " + local_path);
  if (remote_path.size() > 1000) return make_error("path too long");
  Connection c;
  if (auto r = c.open(port_, timeout_s_ == 0 ? 0 : std::max(timeout_s_, 900)); !r) return r;
  auto remote = open_stream(c, "sync:");
  if (!remote) return std::unexpected(remote.error());

  // Sync packets travel inside WRTE messages; each needs the device's OKAY.
  auto write = [&](const std::string& data) -> Result<void> {
    if (auto r = c.send(kWrte, kLocalId, *remote, data); !r) return r;
    while (true) {
      auto m = c.receive();
      if (!m) return std::unexpected(m.error());
      if (m->command == kOkay) return {};
      if (m->command == kClse) return make_error("device closed the transfer");
      if (m->command == kWrte) c.send(kOkay, kLocalId, *remote, {});  // e.g. an early FAIL
    }
  };
  auto packet = [](const char id[4], uint32_t len, std::string_view body = {}) {
    std::string p(id, 4);
    p.append(reinterpret_cast<const char*>(&len), 4);
    p.append(body);
    return p;
  };
  const std::string target = remote_path + ",33188";  // mode 0100644
  if (auto r = write(packet("SEND", static_cast<uint32_t>(target.size()), target)); !r) return r;
  std::vector<char> buf(64 * 1024);  // sync DATA chunks are at most 64 KB
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = static_cast<uint32_t>(in.gcount());
    if (n == 0) break;
    // Keep each WRTE within the device's max payload.
    std::string p = packet("DATA", n, std::string_view(buf.data(), n));
    for (size_t off = 0; off < p.size(); off += c.max_payload()) {
      if (auto r = write(p.substr(off, c.max_payload())); !r) return r;
    }
  }
  const auto mtime = static_cast<uint32_t>(std::time(nullptr));
  if (auto r = write(packet("DONE", mtime)); !r) return r;
  // The device answers OKAY or FAIL <message>.
  std::string reply;
  while (reply.size() < 8) {
    auto m = c.receive();
    if (!m) return std::unexpected(m.error());
    if (m->command == kWrte) {
      reply += m->data;
      c.send(kOkay, kLocalId, *remote, {});
    } else if (m->command == kClse) {
      break;
    }
  }
  c.send(kClse, kLocalId, *remote, {});
  if (!reply.starts_with("OKAY")) {
    return make_error("copy failed: " + (reply.size() > 8 ? reply.substr(8) : std::string("no answer")));
  }
  return {};
}

bool AdbClient::wait_for_boot(int timeout_s) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  while (std::chrono::steady_clock::now() < deadline) {
    auto r = shell("getprop sys.boot_completed");
    if (r && r->starts_with("1")) return true;
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  return false;
}

}  // namespace kestrel
