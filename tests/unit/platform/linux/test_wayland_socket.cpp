/**
 * @file tests/unit/platform/linux/test_wayland_socket.cpp
 * @brief Tests for resolving the active Wayland compositor socket.
 */

#ifdef __linux__

  #include <cstdlib>
  #include <cstring>
  #include <filesystem>
  #include <optional>
  #include <stdexcept>
  #include <string>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>

  #include <gtest/gtest.h>

  #include "src/platform/linux/wayland_socket.h"

namespace {
  /**
   * @brief Temporary runtime directory for one resolver test.
   */
  class runtime_dir_t {
  public:
    /**
     * @brief Create an empty runtime directory.
     *
     * @param name Test-specific directory suffix.
     */
    explicit runtime_dir_t(const std::string &name) {
      path_ = std::filesystem::temp_directory_path() /
              ("sunshine-wayland-socket-" + name + "-" + std::to_string(::getpid()));
      std::error_code error;
      std::filesystem::remove_all(path_, error);
      std::filesystem::create_directories(path_);
    }

    /**
     * @brief Remove the runtime directory and its sockets.
     */
    ~runtime_dir_t() {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }

    runtime_dir_t(const runtime_dir_t &) = delete;
    runtime_dir_t &operator=(const runtime_dir_t &) = delete;

    /**
     * @brief Get the runtime directory path.
     *
     * @return Directory path.
     */
    std::string path() const {
      return path_.string();
    }

    /**
     * @brief Build a path inside the runtime directory.
     *
     * @param name Entry name.
     * @return Full entry path.
     */
    std::string entry(const std::string &name) const {
      return (path_ / name).string();
    }

  private:
    std::filesystem::path path_;  ///< Directory owned by this fixture.
  };

  /**
   * @brief Listening AF_UNIX socket used as a fake compositor.
   */
  class unix_socket_server_t {
  public:
    /**
     * @brief Create and listen on a Unix socket.
     *
     * @param path Socket path.
     */
    explicit unix_socket_server_t(const std::string &path) :
        path_ {path} {
      fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd_ < 0) {
        throw std::runtime_error {"socket() failed"};
      }

      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      if (path.size() >= sizeof(address.sun_path)) {
        close();
        throw std::runtime_error {"socket path is too long"};
      }
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      if (::bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || ::listen(fd_, 4) != 0) {
        close();
        throw std::runtime_error {"bind() or listen() failed"};
      }
    }

    /**
     * @brief Close the listener and remove its socket entry.
     */
    ~unix_socket_server_t() {
      close();
    }

    unix_socket_server_t(const unix_socket_server_t &) = delete;
    unix_socket_server_t &operator=(const unix_socket_server_t &) = delete;

    /**
     * @brief Stop listening and remove the socket entry.
     */
    void close() {
      if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
      }
      if (!path_.empty()) {
        ::unlink(path_.c_str());
        path_.clear();
      }
    }

  private:
    std::string path_;  ///< Socket entry removed when the listener closes.
    int fd_ {-1};       ///< Listening descriptor.
  };

  /**
   * @brief Capture and restore environment variables changed by a test.
   */
  class scoped_wayland_environment_t {
  public:
    /**
     * @brief Save the Wayland-related environment.
     */
    scoped_wayland_environment_t() :
        runtime_dir_ {get("XDG_RUNTIME_DIR")},
        display_ {get("WAYLAND_DISPLAY")} {
    }

    /**
     * @brief Restore the saved environment.
     */
    ~scoped_wayland_environment_t() {
      restore("XDG_RUNTIME_DIR", runtime_dir_);
      restore("WAYLAND_DISPLAY", display_);
    }

    scoped_wayland_environment_t(const scoped_wayland_environment_t &) = delete;
    scoped_wayland_environment_t &operator=(const scoped_wayland_environment_t &) = delete;

  private:
    /**
     * @brief Read and preserve whether an environment variable is set.
     *
     * @param name Variable name.
     * @return Value, or an empty optional when unset.
     */
    static std::optional<std::string> get(const char *name) {
      const char *value = std::getenv(name);
      return value ? std::optional<std::string> {value} : std::nullopt;
    }

    /**
     * @brief Restore one saved environment variable.
     *
     * @param name Variable name.
     * @param value Saved value.
     */
    static void restore(const char *name, const std::optional<std::string> &value) {
      if (value) {
        ::setenv(name, value->c_str(), 1);
      }
      else {
        ::unsetenv(name);
      }
    }

    std::optional<std::string> runtime_dir_;  ///< Previous XDG_RUNTIME_DIR.
    std::optional<std::string> display_;      ///< Previous WAYLAND_DISPLAY.
  };
}  // namespace

TEST(WaylandSocketResolution, KeepsAConnectableConfiguredSocket) {
  const scoped_wayland_environment_t environment;
  const runtime_dir_t runtime_dir {"configured"};
  const unix_socket_server_t server {runtime_dir.entry("wayland-7")};
  ASSERT_EQ(::setenv("XDG_RUNTIME_DIR", runtime_dir.path().c_str(), 1), 0);
  ASSERT_EQ(::setenv("WAYLAND_DISPLAY", "wayland-7", 1), 0);

  EXPECT_EQ(platf::resolve_wayland_socket(), "wayland-7");
  EXPECT_STREQ(std::getenv("WAYLAND_DISPLAY"), "wayland-7");
}

TEST(WaylandSocketResolution, AdoptsTheLowestNumberedConnectableSocket) {
  const scoped_wayland_environment_t environment;
  const runtime_dir_t runtime_dir {"lowest"};
  [[maybe_unused]] const unix_socket_server_t highest {runtime_dir.entry("wayland-10")};
  [[maybe_unused]] const unix_socket_server_t lowest {runtime_dir.entry("wayland-2")};
  [[maybe_unused]] const unix_socket_server_t lock {runtime_dir.entry("wayland-1.lock")};
  ASSERT_EQ(::setenv("XDG_RUNTIME_DIR", runtime_dir.path().c_str(), 1), 0);
  ASSERT_EQ(::setenv("WAYLAND_DISPLAY", "wayland-0", 1), 0);

  EXPECT_EQ(platf::resolve_wayland_socket(), "wayland-2");
  EXPECT_STREQ(std::getenv("WAYLAND_DISPLAY"), "wayland-2");
}

TEST(WaylandSocketResolution, LeavesTheEnvironmentAloneWithoutACompositor) {
  const scoped_wayland_environment_t environment;
  const runtime_dir_t runtime_dir {"missing"};
  ASSERT_EQ(::setenv("XDG_RUNTIME_DIR", runtime_dir.path().c_str(), 1), 0);
  ASSERT_EQ(::setenv("WAYLAND_DISPLAY", "wayland-0", 1), 0);

  EXPECT_TRUE(platf::resolve_wayland_socket().empty());
  EXPECT_STREQ(std::getenv("WAYLAND_DISPLAY"), "wayland-0");
}

TEST(WaylandSocketResolution, ReplacesACachedSocketThatStoppedListening) {
  const scoped_wayland_environment_t environment;
  const runtime_dir_t runtime_dir {"session-restart"};
  unix_socket_server_t first_session {runtime_dir.entry("wayland-1")};
  ASSERT_EQ(::setenv("XDG_RUNTIME_DIR", runtime_dir.path().c_str(), 1), 0);
  ASSERT_EQ(::setenv("WAYLAND_DISPLAY", "wayland-0", 1), 0);
  ASSERT_EQ(platf::resolve_wayland_socket(), "wayland-1");

  first_session.close();
  const unix_socket_server_t second_session {runtime_dir.entry("wayland-3")};
  EXPECT_EQ(platf::resolve_wayland_socket(), "wayland-3");
  EXPECT_STREQ(std::getenv("WAYLAND_DISPLAY"), "wayland-3");
}

#endif
