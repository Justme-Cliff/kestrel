#include "host/block_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace kestrel {
namespace {

class PosixBlockFile final : public BlockFile {
 public:
  PosixBlockFile(int fd, uint64_t size, bool read_only)
      : fd_(fd), size_(size), read_only_(read_only) {}
  ~PosixBlockFile() override { ::close(fd_); }

  uint64_t size() const override { return size_; }
  bool read_only() const override { return read_only_; }

  bool read_at(uint64_t offset, std::span<uint8_t> out) override {
    size_t done = 0;
    while (done < out.size()) {
      const ssize_t n = ::pread(fd_, out.data() + done, out.size() - done,
                                static_cast<off_t>(offset + done));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      done += static_cast<size_t>(n);
    }
    return true;
  }

  bool write_at(uint64_t offset, std::span<const uint8_t> in) override {
    if (read_only_) return false;
    size_t done = 0;
    while (done < in.size()) {
      const ssize_t n = ::pwrite(fd_, in.data() + done, in.size() - done,
                                 static_cast<off_t>(offset + done));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      done += static_cast<size_t>(n);
    }
    return true;
  }

  bool flush() override { return read_only_ || ::fsync(fd_) == 0; }

 private:
  int fd_;
  uint64_t size_;
  bool read_only_;
};

}  // namespace

Result<std::unique_ptr<BlockFile>> BlockFile::open(const std::string& path, bool read_only) {
  const int fd = ::open(path.c_str(), (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC);
  if (fd < 0) return make_error("cannot open disk " + path + ": " + std::strerror(errno));
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return make_error("cannot stat disk " + path);
  }
  return std::unique_ptr<BlockFile>(
      new PosixBlockFile(fd, static_cast<uint64_t>(st.st_size), read_only));
}

}  // namespace kestrel
