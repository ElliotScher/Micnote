#include <micnote.h>

#include <gtest/gtest.h>

#include <regex>
#include <string>

TEST(Version, MatchesProjectVersion) { EXPECT_STREQ(mn_version(), MICNOTE_VERSION_STRING); }

TEST(Version, IsMajorMinorPatch) {
  EXPECT_TRUE(std::regex_match(std::string{mn_version()}, std::regex{R"(\d+\.\d+\.\d+)"}));
}
