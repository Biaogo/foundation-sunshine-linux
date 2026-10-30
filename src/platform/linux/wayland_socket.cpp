/**
 * @file src/platform/linux/wayland_socket.cpp
 * @brief Resolution of the Wayland socket used by the Linux backends.
 */
#include "wayland_socket.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "src/logging.h"

using namespace std::literals;

namespace platf {
  namespace {
    /// Prefix shared by compositor socket names.
    constexpr auto WAYLAND_SOCKET_PREFIX = "wayland-"sv;

    /**
     * @brief Read an environment variable.
     *
     * @param name Variable name.
     * @return Variable value, empty when it is unset.
     */
    std::string environment_value(const char *name) {
      const char *value = std::getenv(name);
      return value ? value : "";
    }

    /**
     * @brief Build the filesystem path addressed by a `WAYLAND_DISPLAY` value.
     *
     * @param display Value of `WAYLAND_DISPLAY`.
     * @param runtime_dir Value of `XDG_RUNTIME_DIR`.
     * @return Socket path, empty when a relative name has no runtime directory.
     */
    std::string socket_path(std::string_view display, std::string_view runtime_dir) {
      if (display.empty()) {
        return {};
      }

      const auto path = std::filesystem::path {std::string {display}};
      if (path.is_absolute()) {
        return path.string();
      }
      if (runtime_dir.empty()) {
        return {};
      }
      return (std::filesystem::path {std::string {runtime_dir}} / path).string();
    }

    /**
     * @brief Check whether a path accepts a connection as an AF_UNIX stream socket.
     *
     * @param path Socket path to probe.
     * @return True when a connection succeeds.
     */
    bool socket_is_connectable(const std::string &path) {
      if (path.empty()) {
        return false;
      }

      sockaddr_un address {};
      if (path.size() >= sizeof(address.sun_path)) {
        return false;
      }

      const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) {
        return false;
      }

      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      int result;
      do {
        result = ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
      } while (result != 0 && errno == EINTR);
      ::close(fd);
      return result == 0;
    }

    /**
     * @brief Check whether a directory entry is a numbered Wayland socket name.
     *
     * @param name Directory entry name.
     * @return True for `wayland-<digits>`, false for lock files and other entries.
     */
    bool is_wayland_socket_name(std::string_view name) {
      if (name.ends_with(".lock"sv) || !name.starts_with(WAYLAND_SOCKET_PREFIX)) {
        return false;
      }

      const auto digits = name.substr(WAYLAND_SOCKET_PREFIX.size());
      return !digits.empty() && std::ranges::all_of(digits, [](char character) {
        return character >= '0' && character <= '9';
      });
    }

    /**
     * @brief Compare two Wayland socket names by their numeric suffix.
     *
     * @param lhs First socket name.
     * @param rhs Second socket name.
     * @return True when @p lhs has the lower number.
     */
    bool has_lower_socket_number(std::string_view lhs, std::string_view rhs) {
      auto left = lhs.substr(WAYLAND_SOCKET_PREFIX.size());
      auto right = rhs.substr(WAYLAND_SOCKET_PREFIX.size());
      while (left.size() > 1 && left.front() == '0') {
        left.remove_prefix(1);
      }
      while (right.size() > 1 && right.front() == '0') {
        right.remove_prefix(1);
      }
      return left.size() < right.size() || (left.size() == right.size() && left < right);
    }

    /**
     * @brief Find the lowest-numbered connectable compositor socket.
     *
     * @param runtime_dir Directory containing the per-user Wayland sockets.
     * @return Socket name, empty when none is connectable.
     */
    std::string find_lowest_wayland_socket(const std::string &runtime_dir) {
      if (runtime_dir.empty()) {
        return {};
      }

      std::error_code error;
      std::filesystem::directory_iterator entry {runtime_dir, error};
      const std::filesystem::directory_iterator end;
      if (error) {
        return {};
      }

      std::string best;
      for (; entry != end; entry.increment(error)) {
        if (error) {
          break;
        }

        const auto name = entry->path().filename().string();
        if (!is_wayland_socket_name(name) || (!best.empty() && !has_lower_socket_number(name, best))) {
          continue;
        }
        if (socket_is_connectable(socket_path(name, runtime_dir))) {
          best = name;
        }
      }
      return best;
    }

    /**
     * @brief Export a selected socket for child processes.
     *
     * @param name Socket name to export.
     * @param replaced Previous `WAYLAND_DISPLAY` value.
     * @return True when the environment was updated.
     */
    bool adopt_wayland_socket(const std::string &name, const std::string &replaced) {
      if (::setenv("WAYLAND_DISPLAY", name.c_str(), 1) != 0) {
        BOOST_LOG(warning) << "[wayland] Could not export WAYLAND_DISPLAY=["sv << name << ']';
        return false;
      }

      BOOST_LOG(info) << "[wayland] Adopted socket ["sv << name << "], replacing ["sv
                      << (replaced.empty() ? "<unset>"sv : std::string_view {replaced}) << ']';
      return true;
    }
  }  // namespace

  std::string resolve_wayland_socket() {
    static std::mutex mutex;
    static std::string cached;
    const std::lock_guard lock {mutex};

    const auto runtime_dir = environment_value("XDG_RUNTIME_DIR");
    const auto configured = environment_value("WAYLAND_DISPLAY");

    if (!configured.empty() && socket_is_connectable(socket_path(configured, runtime_dir))) {
      cached = configured;
      return configured;
    }

    if (!cached.empty() && cached != configured && socket_is_connectable(socket_path(cached, runtime_dir))) {
      if (!adopt_wayland_socket(cached, configured)) {
        cached.clear();
        return {};
      }
      return cached;
    }

    const auto found = find_lowest_wayland_socket(runtime_dir);
    if (found.empty()) {
      cached.clear();
      return {};
    }
    if (!adopt_wayland_socket(found, configured)) {
      cached.clear();
      return {};
    }

    cached = found;
    return found;
  }
}  // namespace platf
