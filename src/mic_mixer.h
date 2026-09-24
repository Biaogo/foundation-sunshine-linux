/**
 * @file src/mic_mixer.h
 * @brief Per-source Opus decoding and host microphone mixing.
 *
 * Sunshine can receive microphone audio from several clients at once (one stream per session) while
 * the host only has a single virtual microphone device. This module decodes each source's Opus
 * packets independently and mixes the queued frames into one mono 48 kHz PCM stream that the
 * platform microphone-redirect device can consume.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace mic_mixer {
  /**
   * @brief Identifier of one microphone source, i.e. one client session.
   */
  using source_id_t = std::uint32_t;

  constexpr std::uint32_t sample_rate = 48000;  ///< Sample rate of every accepted microphone stream.
  constexpr std::size_t frame_samples = sample_rate / 50;  ///< Samples in one 20 ms mixing frame.

  /**
   * @brief Validate that a payload is a decodable single Opus frame of the expected duration.
   *
   * @param data Pointer to the Opus packet payload.
   * @param size Size of the payload in bytes.
   * @return True when the packet decodes to exactly one 20 ms frame at \ref sample_rate.
   */
  bool
  is_valid_opus_packet(const std::uint8_t *data, std::size_t size);

  /**
   * @brief Decodes several microphone sources and mixes them into a single PCM stream.
   *
   * @note This type performs no internal locking: every member function must be called serially from
   * the microphone receive thread.
   */
  class mixer_t {
  public:
    mixer_t();
    ~mixer_t();

    mixer_t(const mixer_t &) = delete;
    mixer_t &operator=(const mixer_t &) = delete;

    /**
     * @brief Add an independently decoded microphone source.
     *
     * @param source_id Identifier of the source, usually the session id.
     * @return True when the source exists after the call, false when no decoder could be created.
     */
    bool
    add_source(source_id_t source_id);

    /**
     * @brief Remove a microphone source and its queued audio.
     *
     * @param source_id Identifier of the source to remove.
     */
    void
    remove_source(source_id_t source_id);

    /**
     * @brief Remove every microphone source.
     */
    void
    clear();

    /**
     * @brief Decode and queue one Opus packet for a source.
     *
     * @param source_id Identifier of the source the packet belongs to.
     * @param data Pointer to the Opus packet payload.
     * @param size Size of the payload in bytes.
     * @param sequence_number RTP sequence number of the packet, used for restart detection.
     * @return True when the packet was decoded and queued, false when it was rejected.
     */
    bool
    push_packet(source_id_t source_id, const std::uint8_t *data, std::size_t size, std::uint16_t sequence_number);

    /**
     * @brief Mix one queued frame from every source into a mono PCM frame.
     *
     * @return The mixed 20 ms frame, or an empty optional when no source has audio queued.
     */
    std::optional<std::vector<std::int16_t>>
    mix_next_frame();

  private:
    struct impl_t;  ///< Implementation detail holding the per-source decoders and frame queues.
    std::unique_ptr<impl_t> impl_;  ///< Owning pointer to the implementation.
  };
}  // namespace mic_mixer
