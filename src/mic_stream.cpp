/**
 * @file src/mic_stream.cpp
 * @brief Definitions for the client microphone intake lane.
 *
 * Protocol: every datagram is a 12-byte header (flags, packet type 0x61, little-endian sequence
 * number, timestamp in milliseconds, ssrc) followed by one Opus frame of 20 ms. Payloads are either
 * plaintext or AES-128-CBC encrypted with the session's input key, where the IV starts with
 * (key id + sequence number) in big-endian order and is zero padded. A payload is accepted when it
 * decodes to exactly one Opus frame of the expected duration, which also disambiguates plaintext
 * from encrypted frames without extra negotiation.
 */
#include "mic_stream.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <openssl/bn.h>
#include <openssl/evp.h>

#include <boost/asio.hpp>

#include "src/audio.h"
#include "src/config.h"
#include "src/crypto.h"
#include "src/logging.h"
#include "src/mic_mixer.h"
#include "src/network.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/utility.h"

using namespace std::literals;
namespace asio = boost::asio;
using asio::ip::udp;

namespace mic_stream {
  namespace {
    constexpr std::size_t header_size = 12;  ///< Size of the microphone packet header.
    constexpr std::size_t max_packet_size = 1400;  ///< Largest microphone datagram accepted.
    constexpr std::uint8_t opus_packet_type = 0x61;  ///< Packet type marking an Opus microphone frame.
    constexpr auto playout_interval = 20ms;  ///< Mixing and playout period.

    /**
     * @brief One registered client and the state the receive path keeps for it.
     */
    struct registration_t {
      client_t client;  ///< Client description supplied by the stream session.
      bool announced {};  ///< Whether the mixer already knows this source.
    };

    std::mutex g_registry_mutex;  ///< Guards \ref g_registry.
    std::map<mic_mixer::source_id_t, registration_t> g_registry;  ///< Clients allowed to send microphone audio.

    /**
     * @brief The UDP lane, running entirely on the microphone io_context thread.
     */
    class lane_t {
    public:
      /**
       * @brief Build the lane on a context.
       *
       * @param io Context the socket and playout timer are bound to.
       * @param port UDP port advertised to clients for microphone audio.
       */
      lane_t(asio::io_context &io, std::uint16_t port):
          io_ {io},
          socket_ {io, udp::endpoint {udp::v4(), port}},
          timer_ {io} {
      }

      /**
       * @brief Start receiving and mixing.
       */
      void run() {
        mic_ready_ = true;
        next_playout_ = std::chrono::steady_clock::now() + playout_interval;

        receive_next();
        schedule_playout();
      }

      /**
       * @brief Stop the socket and the playout timer.
       */
      void shutdown() {
        timer_.cancel();

        boost::system::error_code ignored;
        socket_.close(ignored);
      }

      /**
       * @brief Whether the host virtual microphone is currently usable.
       *
       * @return True while the lane can write mixed audio to the host.
       */
      bool mic_ready() const {
        return mic_ready_;
      }

      /**
       * @brief Counters describing the lane's traffic, for a single log line.
       *
       * @return A printable summary.
       */
      std::string stats() const {
        return std::format(
          "packets={}, mixed={}, undecodable={}, unrouted={}, ambiguous={}",
          packets_,
          accepted_,
          undecodable_,
          unrouted_,
          ambiguous_
        );
      }

    private:
      /**
       * @brief Queue the next datagram receive.
       */
      void receive_next() {
        socket_.async_receive_from(
          asio::buffer(buffer_),
          sender_,
          [this](const boost::system::error_code &ec, std::size_t bytes) {
            if (!ec) {
              on_packet(bytes);
            }

            if (ec != asio::error::operation_aborted) {
              receive_next();
            }
          }
        );
      }

      /**
       * @brief Route and queue one received datagram.
       *
       * @param bytes Number of bytes received.
       */
      void on_packet(std::size_t bytes) {
        ++packets_;

        if (bytes <= header_size || buffer_[1] != opus_packet_type) {
          return;
        }

        const auto sequence_number = static_cast<std::uint16_t>(buffer_[2] | (buffer_[3] << 8));
        const auto timestamp_ms = static_cast<std::uint32_t>(buffer_[4]) |
                                  (static_cast<std::uint32_t>(buffer_[5]) << 8) |
                                  (static_cast<std::uint32_t>(buffer_[6]) << 16) |
                                  (static_cast<std::uint32_t>(buffer_[7]) << 24);

        auto source_id = find_source();
        if (!source_id) {
          ++unrouted_;
          return;
        }

        std::vector<std::uint8_t> payload;
        if (!decode_payload(*source_id, buffer_.data() + header_size, bytes - header_size, sequence_number, payload)) {
          ++undecodable_;
          return;
        }

        announce(*source_id);

        if (mixer_.push_packet(*source_id, payload.data(), payload.size(), sequence_number, timestamp_ms)) {
          ++accepted_;
        }
      }

      /**
       * @brief Find the session a datagram belongs to.
       *
       * Matching uses the client address: microphone packets arrive from an ephemeral port, so the
       * port cannot be used. When several sessions share one address (clients behind a NAT), the
       * lowest session id wins so that one speaker keeps working; the ambiguity is counted.
       *
       * @return Session id of the matching client, or an empty optional when unroutable.
       */
      std::optional<mic_mixer::source_id_t> find_source() {
        const auto address = net::normalize_address(sender_.address());

        std::lock_guard lock {g_registry_mutex};
        std::optional<mic_mixer::source_id_t> match;
        for (const auto &[session_id, registration] : g_registry) {
          if (net::normalize_address(registration.client.address) != address) {
            continue;
          }

          if (!match) {
            match = session_id;
            continue;
          }

          ++ambiguous_;
          if (session_id < *match) {
            match = session_id;
          }
        }

        return match;
      }

      /**
       * @brief Make sure the mixer knows about a session.
       *
       * @param session_id Session that sent a decodable frame.
       */
      void announce(mic_mixer::source_id_t session_id) {
        if (announced_.contains(session_id)) {
          return;
        }

        if (mixer_.add_source(session_id)) {
          announced_.insert(session_id);
        }
      }

      /**
       * @brief Decode a microphone payload, trying the encrypted form when the session has a key.
       *
       * @param session_id Session the datagram belongs to.
       * @param data Payload bytes.
       * @param size Payload size.
       * @param sequence_number Sequence number from the packet header.
       * @param decoded Decoded Opus payload on success.
       * @return True when a valid single-frame Opus packet was produced.
       */
      bool decode_payload(mic_mixer::source_id_t session_id, const std::uint8_t *data, std::size_t size, std::uint16_t sequence_number, std::vector<std::uint8_t> &decoded) {
        decoded.clear();

        if (mic_mixer::is_valid_opus_packet(data, size)) {
          decoded.assign(data, data + size);
          return true;
        }

        client_t client;
        {
          std::lock_guard lock {g_registry_mutex};
          const auto it = g_registry.find(session_id);
          if (it == g_registry.end()) {
            return false;
          }
          client = it->second.client;
        }

        if (!client.encrypted || client.key.empty()) {
          return false;
        }

        // The IV is the key id plus the packet sequence number in big-endian order, zero padded.
        crypto::aes_t iv(16, 0);
        const auto iv_sequence = util::endian::big<std::uint32_t>(client.key_id + sequence_number);
        std::memcpy(iv.data(), &iv_sequence, sizeof(iv_sequence));

        crypto::cipher::cbc_t cipher {client.key, true};
        const std::string_view ciphertext {reinterpret_cast<const char *>(data), size};

        return cipher.decrypt(ciphertext, decoded, &iv) == 0 &&
               mic_mixer::is_valid_opus_packet(decoded.data(), decoded.size());
      }

      /**
       * @brief Queue the next playout tick.
       */
      void schedule_playout() {
        timer_.expires_at(next_playout_);
        timer_.async_wait([this](const boost::system::error_code &ec) {
          if (ec == asio::error::operation_aborted) {
            return;
          }

          const auto now = std::chrono::steady_clock::now();
          if (now >= next_playout_ + playout_interval) {
            const auto missed = static_cast<std::size_t>((now - next_playout_) / playout_interval);
            mixer_.skip_playout_frames(missed);
            next_playout_ += playout_interval * missed;
          }

          sync_sources();
          write_frame();

          next_playout_ += playout_interval;
          schedule_playout();
        });
      }

      /**
       * @brief Drop mixer sources whose sessions are gone.
       */
      void sync_sources() {
        std::set<mic_mixer::source_id_t> live;
        {
          std::lock_guard lock {g_registry_mutex};
          for (const auto &[session_id, registration] : g_registry) {
            (void) registration;
            live.insert(session_id);
          }
        }

        for (auto it = announced_.begin(); it != announced_.end();) {
          if (!live.contains(*it)) {
            mixer_.remove_source(*it);
            it = announced_.erase(it);
          }
          else {
            ++it;
          }
        }
      }

      /**
       * @brief Mix one frame and write it to the host virtual microphone.
       */
      void write_frame() {
        auto mixed = mixer_.mix_next_frame();
        if (!mixed) {
          return;
        }

        if (audio::write_mic_pcm(mixed->data(), mixed->size()) < 0) {
          BOOST_LOG(warning) << "Couldn't write to the host virtual microphone; recreating it"sv;
          audio::release_mic_redirect_device();
          if (audio::init_mic_redirect_device() != 0) {
            if (mic_ready_) {
              BOOST_LOG(warning) << "Host virtual microphone is unavailable; dropping microphone audio"sv;
            }
            mic_ready_ = false;
          }
          else {
            mic_ready_ = true;
          }
        }
      }

      asio::io_context &io_;  ///< Context the lane runs on.
      udp::socket socket_;  ///< Socket receiving microphone datagrams.
      asio::steady_timer timer_;  ///< Playout clock.
      std::array<std::uint8_t, max_packet_size> buffer_ {};  ///< Receive buffer.
      udp::endpoint sender_ {};  ///< Endpoint of the datagram in the receive buffer.
      mic_mixer::mixer_t mixer_;  ///< Per-source decoders and mixing.
      std::set<mic_mixer::source_id_t> announced_;  ///< Sessions already added to the mixer.
      std::chrono::steady_clock::time_point next_playout_ {};  ///< Deadline of the next playout tick.
      bool mic_ready_ {};  ///< Host virtual microphone state.
      std::uint64_t packets_ {};  ///< Datagrams received.
      std::uint64_t accepted_ {};  ///< Frames queued into the mixer.
      std::uint64_t undecodable_ {};  ///< Datagrams that were not a valid Opus frame.
      std::uint64_t unrouted_ {};  ///< Datagrams from addresses without a registered session.
      std::uint64_t ambiguous_ {};  ///< Datagrams matching more than one session.
    };

    std::unique_ptr<asio::io_context> g_io;  ///< Context of the worker thread.
    std::unique_ptr<lane_t> g_lane;  ///< Lane bound to \ref g_io.
    std::jthread g_worker;  ///< Thread running \ref g_io.
    std::atomic_bool g_running {false};  ///< Whether the lane is running.
  }  // namespace

  bool
  start() {
    if (g_running) {
      return true;
    }

    if (!config::audio.stream_mic) {
      return false;
    }

    if (audio::init_mic_redirect_device() != 0) {
      BOOST_LOG(warning) << "This platform has no virtual microphone; ignoring client microphone audio"sv;
      return false;
    }

    const auto port = static_cast<std::uint16_t>(net::map_port(stream::MIC_STREAM_PORT));

    try {
      g_io = std::make_unique<asio::io_context>();
      g_lane = std::make_unique<lane_t>(*g_io, port);
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Couldn't bind the microphone port ["sv << port << "]: "sv << e.what();
      g_lane.reset();
      g_io.reset();
      audio::release_mic_redirect_device();
      return false;
    }

    g_running = true;
    g_worker = std::jthread {[](std::stop_token) {
      g_lane->run();
      g_io->run();
    }};

    BOOST_LOG(info) << "Microphone stream listening on port "sv << port;
    return true;
  }

  void
  stop() {
    if (!g_running.exchange(false)) {
      return;
    }

    if (g_lane) {
      BOOST_LOG(info) << "Microphone stream stopped ("sv << g_lane->stats() << ")"sv;
      g_lane->shutdown();
    }

    if (g_io) {
      g_io->stop();
    }

    if (g_worker.joinable()) {
      g_worker.join();
    }

    g_lane.reset();
    g_io.reset();

    {
      std::lock_guard lock {g_registry_mutex};
      g_registry.clear();
    }

    audio::release_mic_redirect_device();
  }

  bool
  running() {
    return g_running;
  }

  void
  add_client(const client_t &client) {
    std::lock_guard lock {g_registry_mutex};
    g_registry[client.session_id] = registration_t {client, false};
  }

  void
  remove_client(std::uint32_t session_id) {
    {
      std::lock_guard lock {g_registry_mutex};
      g_registry.erase(session_id);
      if (!g_registry.empty()) {
        return;
      }
    }

    // The last microphone session left: tear the lane (and with it the virtual microphone) down so
    // the host does not keep a microphone device that nothing can feed.
    stop();
  }
}  // namespace mic_stream
