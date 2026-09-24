/**
 * @file src/mic_mixer.cpp
 * @brief Per-session Opus decoding and host microphone mixing.
 */
#include "mic_mixer.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <opus/opus.h>

namespace mic_mixer {
  namespace {
    constexpr int channels = 1;
    constexpr std::size_t max_queued_frames = 3;

    /**
     * @brief Deleter that releases an Opus decoder.
     */
    struct opus_decoder_deleter_t {
      void
      operator()(OpusDecoder *decoder) const noexcept {
        if (decoder) {
          opus_decoder_destroy(decoder);
        }
      }
    };

    using opus_decoder_t = std::unique_ptr<OpusDecoder, opus_decoder_deleter_t>;  ///< Owning Opus decoder handle.

    /**
     * @brief Decoder and jitter queue of a single microphone source.
     */
    struct source_t {
      opus_decoder_t decoder;  ///< Decoder owned by this source.
      std::optional<std::uint16_t> last_sequence;  ///< Sequence number of the last accepted packet.
      std::optional<std::uint16_t> expected_restart_sequence;  ///< Next sequence needed to confirm a source restart.
      std::deque<std::vector<std::int16_t>> frames;  ///< Decoded frames waiting to be mixed.
    };

    /**
     * @brief Append a decoded frame, dropping the oldest one when the queue is full.
     *
     * @param source Source receiving the frame.
     * @param frame Decoded mono PCM frame.
     */
    void
    queue_frame(source_t &source, std::vector<std::int16_t> frame) {
      if (source.frames.size() >= max_queued_frames) {
        source.frames.pop_front();
      }
      source.frames.emplace_back(std::move(frame));
    }

    /**
     * @brief Decode one Opus packet into the source's frame queue.
     *
     * @param source Source the packet belongs to.
     * @param data Pointer to the Opus packet payload.
     * @param size Size of the payload in bytes.
     * @return True when a full 20 ms frame was decoded and queued.
     */
    bool
    decode_frame(source_t &source, const std::uint8_t *data, std::size_t size) {
      const auto frame_size = opus_decoder_get_nb_samples(
        source.decoder.get(),
        data,
        static_cast<opus_int32>(size));
      if (frame_size != static_cast<int>(frame_samples)) {
        return false;
      }

      std::vector<std::int16_t> pcm(static_cast<std::size_t>(frame_size));
      const auto decoded_samples = opus_decode(
        source.decoder.get(),
        data,
        static_cast<opus_int32>(size),
        pcm.data(),
        frame_size,
        0);
      if (decoded_samples <= 0) {
        return false;
      }

      pcm.resize(static_cast<std::size_t>(decoded_samples));
      queue_frame(source, std::move(pcm));
      return true;
    }
  }  // namespace

  struct mixer_t::impl_t {
    std::unordered_map<source_id_t, source_t> sources;  ///< Sources keyed by their session id.
  };

  mixer_t::mixer_t():
      impl_ {std::make_unique<impl_t>()} {}

  mixer_t::~mixer_t() = default;

  bool
  is_valid_opus_packet(const std::uint8_t *data, std::size_t size) {
    if (!data || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<opus_int32>::max())) {
      return false;
    }
    const auto samples = opus_packet_get_nb_samples(data, static_cast<opus_int32>(size), sample_rate);
    return samples == static_cast<int>(frame_samples);
  }

  bool
  mixer_t::add_source(source_id_t source_id) {
    if (impl_->sources.contains(source_id)) {
      return true;
    }

    int error = OPUS_OK;
    opus_decoder_t decoder {opus_decoder_create(sample_rate, channels, &error)};
    if (!decoder || error != OPUS_OK) {
      return false;
    }

    impl_->sources.emplace(source_id, source_t {std::move(decoder), std::nullopt, std::nullopt, {}});
    return true;
  }

  void
  mixer_t::remove_source(source_id_t source_id) {
    impl_->sources.erase(source_id);
  }

  void
  mixer_t::clear() {
    impl_->sources.clear();
  }

  bool
  mixer_t::push_packet(source_id_t source_id, const std::uint8_t *data, std::size_t size, std::uint16_t sequence_number) {
    auto source_it = impl_->sources.find(source_id);
    if (source_it == impl_->sources.end() || !data || size == 0) {
      return false;
    }

    auto &source = source_it->second;
    if (source.last_sequence) {
      const auto distance = static_cast<std::uint16_t>(sequence_number - *source.last_sequence);
      if (distance == 0) {
        return false;
      }

      if (distance >= 0x8000) {
        // Source restart detection along the lines of RFC 3550: a single out-of-order packet only
        // arms the check with the next expected sequence number, and only a consecutive packet
        // confirms that the sender restarted - one late packet must not reset the decoder state.
        if (!source.expected_restart_sequence || sequence_number != *source.expected_restart_sequence) {
          source.expected_restart_sequence = static_cast<std::uint16_t>(sequence_number + 1);
          return false;
        }

        if (opus_decoder_ctl(source.decoder.get(), OPUS_RESET_STATE) != OPUS_OK) {
          source.expected_restart_sequence.reset();
          return false;
        }

        source.frames.clear();
        source.last_sequence.reset();
      }
    }

    // Mixing runs on a fixed 20 ms clock. A late FEC frame has already missed its slot, and
    // inserting it would leave the source permanently one frame behind, so only the current packet
    // is decoded here.
    if (!decode_frame(source, data, size)) {
      return false;
    }

    source.last_sequence = sequence_number;
    source.expected_restart_sequence.reset();
    return true;
  }

  std::optional<std::vector<std::int16_t>>
  mixer_t::mix_next_frame() {
    std::size_t source_count = 0;
    for (const auto &[source_id, source] : impl_->sources) {
      (void) source_id;
      if (!source.frames.empty()) {
        ++source_count;
      }
    }

    if (source_count == 0) {
      return std::nullopt;
    }

    std::vector<std::int64_t> sums(frame_samples, 0);
    for (auto &[source_id, source] : impl_->sources) {
      (void) source_id;
      if (source.frames.empty()) {
        continue;
      }

      auto frame = std::move(source.frames.front());
      source.frames.pop_front();
      for (std::size_t sample_index = 0; sample_index < frame.size(); ++sample_index) {
        sums[sample_index] += frame[sample_index];
      }
    }

    std::vector<std::int16_t> mixed(frame_samples, 0);
    for (std::size_t sample_index = 0; sample_index < frame_samples; ++sample_index) {
      const auto averaged = sums[sample_index] / static_cast<std::int64_t>(source_count);
      mixed[sample_index] = static_cast<std::int16_t>(std::clamp<std::int64_t>(
        averaged,
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
    }

    return mixed;
  }
}  // namespace mic_mixer
