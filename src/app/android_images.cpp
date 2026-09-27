#include "app/android_images.h"

#include <spawn.h>
#include <sys/wait.h>

#include <filesystem>
#include <format>

#include "host/macos/mac_window.h"
#include "storage/android_sparse.h"

extern char** environ;

namespace kestrel {
namespace fs = std::filesystem;

namespace {

constexpr const char* kRequired[] = {"boot.img", "init_boot.img", "vendor_boot.img", "vbmeta.img"};

Result<void> run_tool(std::vector<std::string> args) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid;
  if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) {
    return make_error("cannot run " + args[0]);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return make_error(args[0] + " failed");
  return {};
}

}  // namespace

bool android_images_present(const std::string& dir) {
  for (const char* f : kRequired) {
    if (!fs::exists(fs::path(dir) / f)) return false;
  }
  return fs::exists(fs::path(dir) / "instance" / "super.raw") || fs::exists(fs::path(dir) / "super.img");
}

Result<void> install_android_images(const std::string& dir,
                                    const std::function<void(const std::string&, double)>& report) {
  std::error_code ec;
  fs::create_directories(fs::path(dir) / "instance", ec);
  if (ec) return make_error("cannot create " + dir + ": " + ec.message());

  const std::string product = std::string(kAndroidTarget).substr(0, std::string(kAndroidTarget).rfind('-'));
  const std::string zip_name = std::format("{}-img-{}.zip", product, kAndroidBuildId);
  const std::string url = std::format("https://ci.android.com/builds/submitted/{}/{}/latest/raw/{}",
                                      kAndroidBuildId, kAndroidTarget, zip_name);
  const std::string zip = (fs::path(dir) / zip_name).string();

  report("Downloading Android (about 1.1 GB)…", 0);
  auto dl = download_file(url, zip, [&](double p) {
    report(std::format("Downloading Android… {}%", int(p * 100)), p);
  });
  if (!dl) return dl;

  report("Unpacking…", -1);
  if (auto r = run_tool({"/usr/bin/unzip", "-o", "-q", zip, "-d", dir}); !r) return r;
  fs::remove(zip, ec);

  report("Preparing the system disk…", -1);
  const auto super_img = fs::path(dir) / "super.img";
  if (auto r = unsparse_image(super_img.string(), (fs::path(dir) / "instance" / "super.raw").string()); !r) {
    return r;
  }
  fs::remove(super_img, ec);  // the expanded copy replaces it
  report("Done", 1);
  return {};
}

}  // namespace kestrel
