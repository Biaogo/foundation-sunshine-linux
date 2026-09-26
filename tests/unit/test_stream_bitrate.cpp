/**
 * @file tests/unit/test_stream_bitrate.cpp
 * @brief Tests for the runtime bitrate API's request validation.
 */

// standard includes
#include <limits>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/nvhttp.h"
#include "src/video_bitrate.h"

TEST(StreamBitrateValidation, AcceptsTheRangeClientsUse) {
  // Values a Moonlight client sends for a normal stream, and the documented upper bound.
  EXPECT_TRUE(nvhttp::is_valid_stream_bitrate(1));
  EXPECT_TRUE(nvhttp::is_valid_stream_bitrate(2000));
  EXPECT_TRUE(nvhttp::is_valid_stream_bitrate(nvhttp::MAX_STREAM_BITRATE_KBPS));
}

TEST(StreamBitrateValidation, RejectsValuesNoEncoderCouldConfigure) {
  EXPECT_FALSE(nvhttp::is_valid_stream_bitrate(0));
  EXPECT_FALSE(nvhttp::is_valid_stream_bitrate(-1));
  EXPECT_FALSE(nvhttp::is_valid_stream_bitrate(nvhttp::MAX_STREAM_BITRATE_KBPS + 1));
  EXPECT_FALSE(nvhttp::is_valid_stream_bitrate(std::numeric_limits<int>::max()));
}

TEST(StreamBitrateBudget, ScalesWithTheRequestedBitrate) {
  // A 20 Mbps session on a 60 fps stream: the encoder sizes rc_buffer_size as bitrate / framerate.
  const int vbv = 20000000 / 60;

  EXPECT_EQ(video::scale_bitrate_budget(vbv, 20000000, 2000000), vbv / 10);
  EXPECT_EQ(video::scale_bitrate_budget(vbv, 20000000, 40000000), vbv * 2);
  EXPECT_EQ(video::scale_bitrate_budget(vbv, 20000000, 20000000), vbv);
}

TEST(StreamBitrateBudget, LeavesUnusableBudgetsAlone) {
  EXPECT_EQ(video::scale_bitrate_budget(0, 20000000, 2000000), 0);
  EXPECT_EQ(video::scale_bitrate_budget(-1, 20000000, 2000000), -1);
  EXPECT_EQ(video::scale_bitrate_budget(333333, 0, 2000000), 333333);
  EXPECT_EQ(video::scale_bitrate_budget(333333, 20000000, 0), 333333);
}
