/**
 * @file src/video_bitrate.h
 * @brief Helpers for moving a running encoder to a different average bitrate.
 */
#pragma once

// standard includes
#include <cstdint>

namespace video {
  /**
   * @brief Scale a rate-control budget when the average bitrate changes.
   *
   * Sunshine sizes rc_buffer_size from the initial bitrate, so a runtime change has to move it
   * by the same ratio to keep the same buffer depth in frames. A non-positive budget means the
   * encoder was never given one and stays untouched.
   *
   * @param budget Previous budget value.
   * @param old_bitrate Bits per second the budget was sized for.
   * @param new_bitrate Bits per second the encoder is being moved to.
   * @return The scaled budget, or the input value when it cannot be scaled.
   */
  inline int scale_bitrate_budget(int budget, int64_t old_bitrate, int64_t new_bitrate) {
    if (budget <= 0 || old_bitrate <= 0 || new_bitrate <= 0) {
      return budget;
    }

    return static_cast<int>(budget * (static_cast<double>(new_bitrate) / static_cast<double>(old_bitrate)));
  }
}  // namespace video
