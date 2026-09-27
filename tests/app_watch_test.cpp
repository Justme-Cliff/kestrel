#include "app/app_watch.h"

#include <gtest/gtest.h>

using namespace kestrel;

namespace {
void feed(AppDeathWatcher& w, const std::string& text) {
  w.feed(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}
}  // namespace

TEST(AppDeathWatcher, ReportsForegroundCrashWithContext) {
  std::vector<AppDeathWatcher::Death> deaths;
  AppDeathWatcher w([&](const auto& d) { deaths.push_back(d); });
  feed(w, "09-26 E AndroidRuntime: FATAL EXCEPTION: main\n09-26 E AndroidRuntime: Process: com.dts.freefireth, PID: 99\n");
  feed(w, "09-26 I ActivityManager: Process com.dts.freefireth (pid 99) has di");  // split across reads
  feed(w, "ed: fg  TOP\n");
  ASSERT_EQ(deaths.size(), 1u);
  EXPECT_EQ(deaths[0].package, "com.dts.freefireth");
  EXPECT_EQ(deaths[0].reason, "crashed");
  EXPECT_NE(deaths[0].context.find("FATAL EXCEPTION"), std::string::npos);
}

TEST(AppDeathWatcher, NamesMemoryKillsAndIgnoresBackground) {
  std::vector<AppDeathWatcher::Death> deaths;
  AppDeathWatcher w([&](const auto& d) { deaths.push_back(d); });
  feed(w, "I ActivityManager: Process WebViewLoader (pid 5) has died: psvc PER\n");
  EXPECT_TRUE(deaths.empty());
  feed(w, "I lowmemorykiller: Kill 'com.dts.freefireth' (1234), uid 10100, oom_score_adj 0\n");
  feed(w, "I ActivityManager: Process com.dts.freefireth (pid 1234) has died: fg  TOP\n");
  ASSERT_EQ(deaths.size(), 1u);
  EXPECT_EQ(deaths[0].reason, "was closed by Android to free memory");
}

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <filesystem>

#include "app/crash_marker.h"

TEST(CrashMarker, WritesNoteOnFatalSignalAndOffersItOnce) {
  const std::string path = (std::filesystem::temp_directory_path() / ("kestrel-crash-" + std::to_string(::getpid()))).string();
  std::filesystem::remove(path);
  std::filesystem::remove(path + ".seen");
  EXPECT_FALSE(take_crash_marker(path));  // nothing yet: no false alarms
  const pid_t child = fork();
  if (child == 0) {
    install_crash_marker(path);
    std::raise(SIGSEGV);
    _exit(0);
  }
  int status = 0;
  waitpid(child, &status, 0);
  EXPECT_TRUE(WIFSIGNALED(status));  // still dies, so macOS writes its crash report too
  auto note = take_crash_marker(path);
  ASSERT_TRUE(note);
  EXPECT_NE(note->find("signal 11"), std::string::npos);
  EXPECT_FALSE(take_crash_marker(path));  // offered once
  std::filesystem::remove(path + ".seen");
}

#include "app/frame_stats.h"

TEST(FrameStats, FpsLowsAndHitches) {
  using C = kestrel::FrameStats::Clock;
  kestrel::FrameStats s;
  auto t = C::time_point{} + std::chrono::seconds(100);
  s.take(t);
  for (int i = 0; i < 120; ++i) {
    t += std::chrono::microseconds(i == 60 ? 50000 : 16667);  // one 50 ms hitch
    s.record(t);
  }
  auto w = s.take(t);
  EXPECT_EQ(w.frames, 119u);
  EXPECT_NEAR(w.fps, 57, 2);
  EXPECT_NEAR(w.worst_ms, 50, 0.1);
  EXPECT_EQ(w.hitches, 1u);
  EXPECT_GT(w.low1_fps, 19);
  EXPECT_LT(w.low1_fps, 61);
}
