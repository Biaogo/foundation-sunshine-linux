/**
 * @file src/platform/linux/virtual_display.cpp
 * @brief Built-in owner of the dynamic virtual monitor (step 2 of docs/virtual-display-linux.md).
 *
 * Sunshine on Linux has no display-driver style virtual monitor: on this platform the output has
 * to be created by something outside the compositor. Until now that something was the user's
 * `global_prep_cmd` hook, which is why a package could never "just work" — a package cannot seed a
 * per-user config. This file moves that job into the process:
 *
 *  - `krfb-virtualmonitor` is started on demand and owned by this object,
 *  - the compositor is polled until it enumerates the created output,
 *  - the output is made live with `kscreen-doctor`,
 *  - killing the helper (session end, destructor) removes the output again.
 *
 * Nothing happens until a session actually asks for a virtual monitor, so hosts that never use one
 * pay nothing beyond two `PATH` lookups.
 */
#include "virtual_display.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gio/gio.h>

#include "src/boost_process_compat.h"
#include "src/logging.h"
#include "src/platform/common.h"

namespace platf {
  namespace {
    /// VNC port the helper listens on; only used locally by the compositor.
    constexpr int VNC_PORT = 5910;

    /// How long to wait for the compositor to enumerate the new output.
    constexpr auto OUTPUT_WAIT = std::chrono::seconds {8};

    /// Poll interval while waiting for the output.
    constexpr auto OUTPUT_POLL = std::chrono::milliseconds {300};

    /**
     * @brief Resolve an executable through `PATH`.
     *
     * @param tool Executable name.
     * @return Absolute path, empty when not found.
     */
    std::string tool_path(const std::string &tool) {
      try {
        return boost::process::v1::search_path(tool).string();
      }
      catch (const std::exception &) {
        return {};
      }
    }

    /**
     * @brief Build a password for the local VNC endpoint.
     *
     * The helper binds a TCP port for the compositor's benefit; keep it unguessable rather than
     * shipping a fixed string.
     *
     * @return Random hexadecimal password.
     */
    std::string random_password() {
      std::random_device device;
      std::uniform_int_distribution<std::uint32_t> dist;
      std::ostringstream out;
      out << std::hex << dist(device) << dist(device);
      return out.str();
    }

    /**
     * @brief Run a command and collect its standard output.
     *
     * @param cmd Command and arguments.
     * @return Captured stdout, empty on failure.
     */
    std::string capture_stdout(const std::vector<std::string> &cmd) {
      try {
        boost::process::v1::ipstream out;
        boost::process::v1::child child(cmd, boost::process::v1::std_out > out);

        std::string line;
        std::string all;
        while (child.running() && std::getline(out, line)) {
          all += line;
          all += '\n';
        }
        child.wait();
        return all;
      }
      catch (const std::exception &e) {
        BOOST_LOG(debug) << "Could not run a helper command: "sv << e.what();
        return {};
      }
    }

    /**
     * @brief Ask kscreen-doctor for the layout.
     *
     * @return Raw `kscreen-doctor -o` output (empty when the tool is missing).
     */
    std::string kscreen_output() {
      const auto exe = tool_path("kscreen-doctor");
      if (exe.empty()) {
        return {};
      }
      return capture_stdout({exe, "-o"});
    }

    /**
     * @brief Find the kscreen uuid of an output by name.
     *
     * Lines look like `Output: 1 eDP-1 d0212254-4127-41d2-9894-898eabae7a38`; the uuid is the last
     * field. ANSI colour escapes are irrelevant because a uuid never contains them.
     *
     * @param layout Raw `kscreen-doctor -o` output.
     * @param name Output name to look for.
     * @return uuid, empty when the output is unknown.
     */
    std::string output_uuid(const std::string &layout, const std::string &name) {
      std::istringstream stream {layout};
      std::string line;
      while (std::getline(stream, line)) {
        if (line.find("Output: ") == std::string::npos || line.find(name) == std::string::npos) {
          continue;
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
          line.pop_back();
        }
        const auto pos = line.find_last_of(" \t");
        if (pos != std::string::npos) {
          return line.substr(pos + 1);
        }
      }
      return {};
    }

    /**
     * @brief Enable an output through kscreen-doctor.
     *
     * @param uuid Output uuid.
     * @return True when the command was executed.
     */
    bool enable_output(const std::string &uuid) {
      const auto exe = tool_path("kscreen-doctor");
      if (exe.empty() || uuid.empty()) {
        return false;
      }

      try {
        boost::process::v1::child child(exe, "output." + uuid + ".enable");
        child.wait();
        return true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "Could not enable the virtual output: "sv << e.what();
        return false;
      }
    }

    /**
     * @brief Poll the compositor's output list for a name.
     *
     * @param name Output name to wait for.
     * @return True when the compositor enumerates it.
     */
    bool wait_for_output(const std::string &name) {
      const auto deadline = std::chrono::steady_clock::now() + OUTPUT_WAIT;
      do {
        for (const auto &entry : display_names(mem_type_e::unknown)) {
          if (entry == name) {
            return true;
          }
        }
        std::this_thread::sleep_for(OUTPUT_POLL);
      } while (std::chrono::steady_clock::now() < deadline);

      return false;
    }
  }  // namespace

  /**
   * @brief Implementation state: the helper process and the output it produced.
   */
  struct virtual_display_t::impl_t {
    boost::process::v1::child child;  ///< krfb-virtualmonitor, owned by this object.
    std::string output_name;          ///< KWin output name (empty when not started).
  };

  virtual_display_t::~virtual_display_t() {
    stop();
  }

  bool virtual_display_t::start(int width, int height, int fps) {
    if (impl_ && impl_->child.valid()) {
      return true;
    }

    const auto helper = tool_path("krfb-virtualmonitor");
    if (helper.empty()) {
      BOOST_LOG(warning) << "Virtual display requested but krfb-virtualmonitor is not installed"sv;
      return false;
    }
    if (tool_path("kscreen-doctor").empty()) {
      BOOST_LOG(warning) << "Virtual display requested but kscreen-doctor is not installed"sv;
      return false;
    }

    if (width <= 0 || height <= 0) {
      width = 1920;
      height = 1080;
    }
    if (fps <= 0) {
      fps = 60;
    }

    // The helper needs the session's compositor connection: inherit this process' environment,
    // which for a user service already carries WAYLAND_DISPLAY and the session bus.
    auto state = std::make_unique<impl_t>();
    try {
      state->child = boost::process::v1::child(
        helper,
        "--resolution", std::to_string(width) + "x" + std::to_string(height),
        "--name", "SunshineVirt",
        "--port", std::to_string(VNC_PORT),
        "--password", random_password());
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Could not start krfb-virtualmonitor: "sv << e.what();
      return false;
    }

    const std::string name {VIRTUAL_DISPLAY_OUTPUT_NAME};
    if (!wait_for_output(name)) {
      BOOST_LOG(warning) << "Virtual display did not appear as ["sv << name << "] within "sv
                         << OUTPUT_WAIT.count() << "s"sv;
      state->child.terminate();
      return false;
    }

    if (!enable_output(output_uuid(kscreen_output(), name))) {
      BOOST_LOG(warning) << "Virtual output ["sv << name << "] appeared but could not be enabled"sv;
      state->child.terminate();
      return false;
    }

    state->output_name = name;
    impl_ = std::move(state);
    BOOST_LOG(info) << "Virtual display ["sv << name << "] created at "sv << width << 'x' << height
                    << '@' << fps;
    return true;
  }

  bool virtual_display_t::active() const {
    return impl_ && impl_->child.valid() && impl_->child.running();
  }

  const std::string &virtual_display_t::output_name() const {
    static const std::string empty;
    return impl_ ? impl_->output_name : empty;
  }

  void virtual_display_t::stop() {
    if (!impl_) {
      return;
    }

    if (impl_->child.valid() && impl_->child.running()) {
      impl_->child.terminate();
      BOOST_LOG(info) << "Virtual display helper stopped; output removed"sv;
    }
    impl_.reset();
  }

  namespace {
    /// Property read from / written to KWin's input devices.
    constexpr auto INPUT_DEVICE_IFACE = "org.kde.KWin.InputDevice";
    constexpr auto INPUT_MANAGER_IFACE = "org.kde.KWin.InputDeviceManager";
    constexpr auto INPUT_MANAGER_PATH = "/org/kde/KWin/InputDevice";

    /**
     * @brief Read one property of a KWin input device.
     *
     * @param path Device object path.
     * @param property Property name.
     * @return Raw busctl output (empty on failure).
     */
    constexpr auto KWIN_SERVICE = "org.kde.KWin";
    constexpr auto PROPERTIES_IFACE = "org.freedesktop.DBus.Properties";

    /**
     * @brief Call a method on KWin's session-bus object, in-process.
     *
     * Spawning a helper (busctl) cannot work from inside a session: this process already owns
     * children (the prep commands, the app) and a SIGCHLD handler reaps ours before we can collect
     * it, so `system()` returns non-zero and a pipe of our own comes back empty — while the very
     * same command in a shell, with this process' own environment, prints the whole device list.
     * Talk to the bus ourselves: GIO is already a dependency of the Linux platform code.
     */
    GVariant *kwin_call(const std::string &path, const std::string &iface, const std::string &method,
                        GVariant *params, const GVariantType *reply_type) {
      GError *error = nullptr;
      auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
      if (!bus) {
        BOOST_LOG(warning) << "Touch binding: no session bus ("sv << (error ? error->message : "unknown") << ')';
        g_clear_error(&error);
        return nullptr;
      }

      auto *reply = g_dbus_connection_call_sync(bus, KWIN_SERVICE, path.c_str(), iface.c_str(), method.c_str(),
                                                params, reply_type, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
      g_object_unref(bus);
      if (!reply) {
        BOOST_LOG(warning) << "Touch binding: D-Bus "sv << iface << '.' << method << " failed: "
                           << (error ? error->message : "unknown");
        g_clear_error(&error);
      }
      return reply;
    }

    /**
     * @brief Interface that owns a given KWin object path.
     */
    std::string kwin_interface(const std::string &path) {
      return path == INPUT_MANAGER_PATH ? std::string {INPUT_MANAGER_IFACE} : std::string {INPUT_DEVICE_IFACE};
    }

    /**
     * @brief Read one property of a KWin input device (or of the device manager).
     *
     * @return Plain value: strings as-is, booleans as "true"/"false", string arrays space separated.
     */
    std::string device_property(const std::string &path, const std::string &property) {
      auto *reply = kwin_call(path, PROPERTIES_IFACE, "Get",
                              g_variant_new("(ss)", kwin_interface(path).c_str(), property.c_str()),
                              G_VARIANT_TYPE("(v)"));
      if (!reply) {
        return {};
      }

      GVariant *value = nullptr;
      g_variant_get(reply, "(v)", &value);
      std::string result;
      if (value) {
        if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
          result = g_variant_get_string(value, nullptr);
        }
        else if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
          result = g_variant_get_boolean(value) ? "true" : "false";
        }
        else if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING_ARRAY)) {
          GVariantIter iter;
          g_variant_iter_init(&iter, value);
          while (auto *item = g_variant_iter_next_value(&iter)) {
            if (!result.empty()) {
              result += ' ';
            }
            result += g_variant_get_string(item, nullptr);
            g_variant_unref(item);
          }
        }
        g_variant_unref(value);
      }
      g_variant_unref(reply);
      return result;
    }

    /**
     * @brief Bind a device's absolute input to an output.
     *
     * @param path Device object path.
     * @param output_name Target KWin output.
     * @return True when the property now holds the wanted value.
     */
    bool bind_device(const std::string &path, const std::string &output_name) {
      if (device_property(path, "outputName") == output_name) {
        return true;
      }

      if (auto *reply = kwin_call(path, PROPERTIES_IFACE, "Set",
                                  g_variant_new("(ssv)", INPUT_DEVICE_IFACE, "outputName",
                                                g_variant_new_string(output_name.c_str())),
                                  nullptr)) {
        g_variant_unref(reply);
      }
      return device_property(path, "outputName") == output_name;
    }
  }  // namespace

  bool session_bind_touch(const std::string &output_name) {
    BOOST_LOG(info) << "Touch binding requested for ["sv << output_name << ']';
    if (output_name.empty()) {
      BOOST_LOG(warning) << "Touch binding skipped (no output name)"sv;
      return false;
    }

    std::thread([output_name]() {
      // The client's devices show up within a second or two of connecting; keep looking for a
      // minute in case a slow client is late, then give up quietly.
      bool touch_bound = false;
      for (int i = 0; i < 120 && !touch_bound; ++i) {
        const auto sysnames = device_property(INPUT_MANAGER_PATH, "devicesSysNames");
        if (i % 10 == 0) {
          BOOST_LOG(info) << "Touch binding poll "sv << i << ": device list=["sv << sysnames << ']';
        }
        std::istringstream names {sysnames};
        std::string sysname;
        while (names >> sysname) {
          sysname.erase(std::remove(sysname.begin(), sysname.end(), '"'), sysname.end());
          const std::string path = std::string {INPUT_MANAGER_PATH} + "/" + sysname;
          const auto name = device_property(path, "name");
          if (name.find("libvirtualhid") == std::string::npos) {
            continue;
          }
          const bool is_touch = device_property(path, "touch") == "true";
          const bool is_pen = device_property(path, "supportsCalibrationMatrix") == "true";
          if (!is_touch && !is_pen) {
            continue;
          }
          const bool ok = bind_device(path, output_name);
          // Build the status as a std::string first: a `sv` literal cannot be concatenated with a
          // std::string, and the ternary would otherwise have to mix both types.
          const std::string status = ok ? std::string {"ok"} :
                                            ("FAILED (was '" + device_property(path, "outputName") + "')");
          BOOST_LOG(info) << "Touch binding: "sv << sysname << " ("sv << name << ") -> ["sv << output_name
                          << "] "sv << status;
          if (ok) {
            touch_bound = touch_bound || is_touch;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {500});
      }
      if (touch_bound) {
        BOOST_LOG(info) << "Touch input bound to ["sv << output_name << ']';
      }
      else {
        BOOST_LOG(warning) << "No libvirtualhid touchscreen appeared to bind to ["sv << output_name << ']';
      }
    }).detach();

    return true;
  }

  bool virtual_display_available() {
    return !tool_path("krfb-virtualmonitor").empty() && !tool_path("kscreen-doctor").empty();
  }

  namespace {
    /// Owns the built-in virtual monitor for the lifetime of one stream session.
    std::unique_ptr<virtual_display_t> session_display;
  }  // namespace

  bool session_virtual_display_start(int width, int height, int fps) {
    if (!session_display) {
      session_display = std::make_unique<virtual_display_t>();
    }
    return session_display->start(width, height, fps);
  }

  void session_virtual_display_stop() {
    session_display.reset();
  }
}  // namespace platf
