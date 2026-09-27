#include "app/boot_progress.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace kestrel;

TEST(BootProgress, MovesForwardOnMilestones) {
  std::vector<double> seen;
  BootProgress p([&](double f, const std::string&) { seen.push_back(f); });
  auto feed = [&](const std::string& s) {
    for (char c : s) p.feed(static_cast<uint8_t>(c));
  };
  feed("[    0.000000] Booting Linux on physical CPU 0\n");
  feed("random line\n");
  feed("init: starting service 'zygote'...\n");
  feed("[    0.1] Run /init as init process\n");  // earlier milestone: ignored now
  p.finish();
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_DOUBLE_EQ(seen[0], 0.05);
  EXPECT_DOUBLE_EQ(seen[1], 0.45);
  EXPECT_DOUBLE_EQ(seen[2], 1.0);
}
