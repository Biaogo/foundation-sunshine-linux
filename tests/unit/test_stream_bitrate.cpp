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
