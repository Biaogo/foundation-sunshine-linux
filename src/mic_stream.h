/**
 * @file src/mic_stream.h
 * @brief Client microphone intake: a UDP lane that feeds the host virtual microphone.
 *
 * Clients that support microphone redirection (Moonlight VPlus and other implementations of the
 * same protocol) send 20 ms Opus frames to the microphone port advertised in the RTSP DESCRIBE
 * response. Every session that performed RTSP SETUP for that stream is registered here; its packets
 * are decoded by src/mic_mixer.* and mixed into the platform virtual microphone that each
 * platform backend provides.
 *
 * The lane owns one thread: the socket receive handler and the 20 ms playout timer both run on the
 * same boost::asio::io_context, which is what keeps the mixer's single-threaded contract.
 */
#pragma once

#include <cstdint>
#include <vector>

#include <boost/asio/ip/address.hpp>

namespace mic_stream {
  /**
   * @brief A session allowed to send microphone audio.
   */
  struct client_t {
    std::uint32_t session_id {};  ///< Session id, used as the mixer source id.
    boost::asio::ip::address address {};  ///< Client address the microphone packets arrive from.
    std::vector<std::uint8_t> key {};  ///< AES key of the session's input stream, empty when unencrypted.
    std::uint32_t key_id {};  ///< Key id used in the AES-CBC IV, matching the audio stream's ri key id.
    bool encrypted {};  ///< Whether microphone packets from this client are AES-CBC encrypted.
  };

  /**
   * @brief Start the microphone lane.
   *
   * Binds the microphone UDP port, creates the host virtual microphone and spawns the worker thread.
   * Idempotent: a second call while running does nothing.
   *
   * @return True when the lane is running after the call.
   */
  bool
  start();

  /**
   * @brief Stop the microphone lane and release the virtual microphone.
   *
   * Idempotent; safe to call when the lane is not running.
   */
  void
  stop();

  /**
   * @brief Whether the lane is currently running.
   *
   * @return True while the worker thread is alive.
   */
  bool
  running();

  /**
   * @brief Register a client whose microphone packets should be accepted.
   *
   * Replaces an earlier registration with the same session id.
   *
   * @param client Client description.
   */
  void
  add_client(const client_t &client);

  /**
   * @brief Unregister a client and drop its queued microphone audio.
   *
   * @param session_id Session id passed to add_client().
   */
  void
  remove_client(std::uint32_t session_id);
}  // namespace mic_stream
