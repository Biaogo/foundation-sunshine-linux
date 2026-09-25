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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gio/gio.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "src/boost_process_compat.h"
#include "src/logging.h"
#include "src/config.h"
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
     * @brief Whether a path names a file this process is allowed to execute.
     *
     * @param path Candidate path.
     * @return True when the file exists and carries an execute bit for this process.
     */
    bool executable_file(const std::string &path) {
      return !path.empty() && ::access(path.c_str(), X_OK) == 0;
    }

    /**
     * @brief Find an executable in a colon-separated directory list.
     *
     * Hand-rolled instead of `boost::process::search_path` because the list has to be injectable:
     * the unit tests pin it rather than inherit the `PATH` of whoever runs them.
     *
     * @param tool Executable name.
     * @param dirs Directory list to search.
     * @return Absolute path, empty when no directory carries the tool.
     */
    std::string search_dirs(const std::string &tool, const std::string &dirs) {
      std::istringstream stream {dirs};
      std::string dir;
      while (std::getline(stream, dir, ':')) {
        if (dir.empty()) {
          continue;
        }
        const auto candidate = dir + "/" + tool;
        if (executable_file(candidate)) {
          return candidate;
        }
      }
      return {};
    }

    /**
     * @brief Configured absolute path of a helper, as read from `sunshine.conf`.
     *
     * @param tool Executable name.
     * @return Configured value, empty for tools that have no option.
     */
    const std::string &configured_helper(const std::string &tool) {
      static const std::string none;
      if (tool == VIRTUAL_DISPLAY_HELPER) {
        return config::sunshine.virtual_display_helper;
      }
      if (tool == KSCREEN_HELPER) {
        return config::sunshine.kscreen_helper;
      }
      return none;
    }

    /**
     * @brief Name of the `sunshine.conf` option that points at a helper.
     *
     * @param tool Executable name.
     * @return Option name, empty for tools that have no option.
     */
    std::string helper_option(const std::string &tool) {
      if (tool == VIRTUAL_DISPLAY_HELPER) {
        return "virtual_display_helper";
      }
      if (tool == KSCREEN_HELPER) {
        return "kscreen_helper";
      }
      return {};
    }

    /**
     * @brief Package that ships a helper, for the message shown when it is missing.
     *
     * @param tool Executable name.
     * @return Human-readable hint, empty for tools that have no package.
     */
    std::string helper_package(const std::string &tool) {
      if (tool == VIRTUAL_DISPLAY_HELPER) {
        return "krfb";
      }
      if (tool == KSCREEN_HELPER) {
        return "libkscreen";
      }
      return {};
    }

    /**
     * @brief Comma-separated rendering of the fallback directory list.
     *
     * @return Directory list for a log message.
     */
    std::string fallback_dirs_text() {
      std::string text;
      for (const auto &dir : helper_fallback_dirs()) {
        if (!text.empty()) {
          text += ", ";
        }
        text += dir;
      }
      return text;
    }

    /**
     * @brief Warn, once per process, that a helper cannot be resolved.
     *
     * The availability probe runs for every client display-list request, so a host without the KDE
     * helpers must not repeat the same line per request — but it must say something at all: the
     * failure used to be completely silent, which on 2026-09-25 showed up as the virtual display
     * ids quietly disappearing from the client list with no hint anywhere in the log. One message
     * per helper is enough, because nothing changes until the host is fixed.
     *
     * @param tool Executable name that could not be resolved.
     */
    void warn_helper_unavailable(const std::string &tool) {
      static std::mutex mutex;
      static std::set<std::string> warned;

      const std::lock_guard<std::mutex> lock {mutex};
      if (!warned.insert(tool).second) {
        return;
      }

      const auto &configured = configured_helper(tool);
      const auto option = helper_option(tool);

      if (!configured.empty()) {
        BOOST_LOG(warning) << "Configured "sv << option << " = ["sv << configured
                           << "] is not an executable file; searched $PATH and "sv << fallback_dirs_text()
                           << " instead. Helper "sv << tool << " is unavailable."sv;
        return;
      }

      BOOST_LOG(warning) << "Helper "sv << tool << " was not found in $PATH or "sv << fallback_dirs_text()
                         << "; the virtual display is disabled. Install "sv << helper_package(tool)
                         << ", or set "sv << option << " to an absolute path (for example "sv
                         << option << " = /usr/bin/"sv << tool << ")."sv;
    }

    /**
     * @brief Resolve an executable through its configured path, `$PATH` and the standard locations.
     *
     * @param tool Executable name.
     * @return Absolute path, empty when not found.
     */
    std::string tool_path(const std::string &tool) {
      const char *path = std::getenv("PATH");
      return find_helper(tool, configured_helper(tool), path ? path : "");
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

  std::vector<std::string> helper_fallback_dirs() {
    std::vector<std::string> dirs;

    // Ordinary distribution locations, plus the NixOS system profile — the only place a service can
    // see a globally installed Nix package, because /usr/bin does not exist on NixOS. A service'
    // PATH rarely carries any of them, which is exactly the failure this list exists for.
    for (const auto *dir : {"/usr/bin", "/usr/local/bin", "/run/current-system/sw/bin"}) {
      dirs.emplace_back(dir);
    }

    // A portable installation (AppImage, tarball, a self-contained package) may ship the helper
    // next to the running executable or in a `bin/` directory beside it. Last, because a
    // distribution package would have installed the helper in one of the locations above.
    std::array<char, 4096> self {};
    const auto length = ::readlink("/proc/self/exe", self.data(), self.size() - 1);
    if (length > 0) {
      const std::filesystem::path executable {std::string {self.data(), static_cast<std::size_t>(length)}};
      const auto directory = executable.parent_path();
      dirs.emplace_back((directory / ".." / "bin").lexically_normal().string());
      dirs.emplace_back(directory.string());
    }

    return dirs;
  }

  std::string find_helper(const std::string &tool, const std::string &configured, const std::string &path_env) {
    if (executable_file(configured)) {
      return configured;
    }

    if (const auto from_path = search_dirs(tool, path_env); !from_path.empty()) {
      return from_path;
    }

    for (const auto &dir : helper_fallback_dirs()) {
      const auto candidate = dir + "/" + tool;
      if (executable_file(candidate)) {
        return candidate;
      }
    }

    return {};
  }

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

  namespace {
    /// Defined with the other helper functions further down; declared here for start().
    void cleanup_stale_helpers();
  }  // namespace

  bool virtual_display_t::start(int width, int height, int fps) {
    // A previous session (or a crashed one) may have left a virtual monitor behind; the hook used to
    // clean that up before creating the new one.
    cleanup_stale_helpers();

    if (impl_ && impl_->child.valid()) {
      return true;
    }

    const auto helper = tool_path(VIRTUAL_DISPLAY_HELPER);
    if (helper.empty()) {
      warn_helper_unavailable(VIRTUAL_DISPLAY_HELPER);
      return false;
    }
    if (tool_path(KSCREEN_HELPER).empty()) {
      warn_helper_unavailable(KSCREEN_HELPER);
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

    /// Session bus name of the KWin service.
    constexpr auto KWIN_SERVICE = "org.kde.KWin";
    /// Standard D-Bus property interface, used to read and write KWin's properties.
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

  // ------------------------------------------------ display combination (phase 2 of the topology work)

  namespace {
    std::mutex g_topology_mutex;
    /// Outputs as they were before the session; kscreen output entries, the `name` is informational.
    std::vector<kscreen_output_t> g_topology_snapshot;
    bool g_topology_applied {};

    /// @brief Drop leading blanks from a kscreen-doctor line.
    std::string trimmed(std::string value) {
      const auto start = value.find_first_not_of(" \t");
      return start == std::string::npos ? std::string {} : value.substr(start);
    }

    /// @brief Parse the output list of `kscreen-doctor -o` (fallback only).
    std::vector<kscreen_output_t> kscreen_outputs_from_child() {
      std::vector<kscreen_output_t> outputs;
      std::istringstream stream {kscreen_output()};
      std::string line;
      kscreen_output_t current;
      bool open = false;

      while (std::getline(stream, line)) {
        if (line.rfind("Output:", 0) == 0) {
          if (open && !current.uuid.empty()) {
            outputs.push_back(current);
          }
          current = {};
          std::istringstream fields {line};
          std::string tag;
          std::string index;
          fields >> tag >> index >> current.name >> current.uuid;
          open = true;
          continue;
        }
        if (!open) {
          continue;
        }

        const auto value = trimmed(line);
        if (value == "enabled") {
          current.enabled = true;
        }
        else if (value == "disabled") {
          current.enabled = false;
        }
        else if (value.rfind("priority ", 0) == 0) {
          current.priority = std::atoi(value.c_str() + 9);
        }
        else if (value.rfind("Geometry:", 0) == 0) {
          current.geometry = trimmed(value.substr(9));
        }
      }

      if (open && !current.uuid.empty()) {
        outputs.push_back(current);
      }
      return outputs;
    }

    /// @brief One output as KWin's own configuration file describes it.
    ///
    /// Verified shape on the target host (the flat assumption this started with silently yielded
    /// zero outputs): the file is a list of named setups; a setup's `data` array holds one
    /// descriptor per output with `uuid` and the output name in **`connectorName`**, while the
    /// enabled/priority state lives in a separate `outputs` array whose entries carry an
    /// `outputIndex` pointing back into that descriptor array.
    struct raw_output_t {
      std::string uuid;
      std::string connector;
      std::size_t index {};
    };

    /// @brief Walk KWin's config: collect output descriptors (uuid + connectorName) and their state.
    void collect_outputs(const nlohmann::json &node, std::vector<raw_output_t> &descriptors,
                         std::vector<std::pair<std::size_t, std::pair<bool, int>>> &states) {
      if (node.is_array()) {
        std::size_t index = 0;
        for (const auto &child : node) {
          if (child.is_object()) {
            // Descriptors and state entries are told apart by their keys.
            if (child.contains("uuid") && child["uuid"].is_string() && !child["uuid"].get<std::string>().empty()) {
              raw_output_t descriptor;
              descriptor.uuid = child["uuid"].get<std::string>();
              descriptor.connector = child.contains("connectorName") && child["connectorName"].is_string()
                                       ? child["connectorName"].get<std::string>()
                                       : (child.contains("name") && child["name"].is_string() ? child["name"].get<std::string>() : std::string {});
              descriptor.index = index;
              descriptors.push_back(descriptor);
            }
            else if (child.contains("outputIndex") && child["outputIndex"].is_number_integer()) {
              const bool enabled = child.contains("enabled") && child["enabled"].is_boolean() && child["enabled"].get<bool>();
              const int priority = child.contains("priority") && child["priority"].is_number_integer() ? child["priority"].get<int>() : 0;
              states.push_back({static_cast<std::size_t>(child["outputIndex"].get<int>()), {enabled, priority}});
            }
          }
          ++index;
        }
      }

      if (node.is_object()) {
        for (const auto &item : node.items()) {
          collect_outputs(item.value(), descriptors, states);
        }
      }
      else if (node.is_array()) {
        for (const auto &child : node) {
          collect_outputs(child, descriptors, states);
        }
      }
    }

    /// @brief Where KWin keeps its output configuration (updated live while it runs).
    ///
    /// KWin writes it in the user's real home, which is not necessarily where this process looks:
    /// a test instance may run with a different XDG_CONFIG_HOME (that is how the revert snapshot
    /// once came out empty), so try the environment first and fall back to ~/.config.
    std::string kwin_output_config_path() {
      std::vector<std::string> candidates;
      if (const auto *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        candidates.push_back(std::string {xdg} + "/kwinoutputconfig.json");
      }
      if (const auto *home = std::getenv("HOME"); home && *home) {
        candidates.push_back(std::string {home} + "/.config/kwinoutputconfig.json");
      }

      for (const auto &candidate : candidates) {
        if (std::ifstream {candidate}) {
          return candidate;
        }
      }
      return candidates.empty() ? std::string {} : candidates.front();
    }

    /**
     * @brief Read the output list out of KWin's own configuration file.
     *
     * Reading a child process' output does not work from inside this process (three independent
     * attempts came back empty — see the note above kwin_call), and KWin exposes no outputs over
     * D-Bus (its object tree was checked), but it does keep this file current while it runs.
     *
     * @return Outputs as the file describes them, empty when it is unreadable.
     */
    std::vector<kscreen_output_t> kscreen_outputs_from_kwin_config() {
      const auto path = kwin_output_config_path();
      if (path.empty()) {
        return {};
      }

      std::ifstream file {path};
      if (!file) {
        return {};
      }

      std::ostringstream contents;
      contents << file.rdbuf();

      const auto outputs = kscreen_outputs_from_json_text(contents.str());
      if (outputs.empty()) {
        // An empty result was the shape of the earlier pitfall: the file was found but read with the
        // wrong structure, and every later step silently did nothing.
        BOOST_LOG(warning) << "topology: "sv << path << " lists no usable output"sv;
      }
      return outputs;
    }

    /// @brief Output list: KWin's config file first, the old child parser only as a fallback.
    std::vector<kscreen_output_t> kscreen_outputs() {
      if (auto outputs = kscreen_outputs_from_kwin_config(); !outputs.empty()) {
        return outputs;
      }
      return kscreen_outputs_from_child();
    }

    /// @brief Kill helper processes a previous session left behind (the hook does the same).
    ///
    /// One-way by design: this build must never read a child's output (see the note above kwin_call),
    /// so only the side effect is used. Called when no helper of ours is running.
    void cleanup_stale_helpers() {
      if (tool_path("pkill").empty()) {
        return;
      }
      // The upstream build compiles with -Werror=unused-result, so the return value has to be
      // consumed even though a failure here is uninteresting: a non-zero exit just means there was
      // nothing stale to kill (or the child was reaped before we could read it).
      if (std::system("pkill -f '[k]rfb-virtualmonitor' >/dev/null 2>&1") != 0) {
        BOOST_LOG(debug) << "topology: no stale virtual-monitor helper to clean up"sv;
      }
    }

    /// @brief Run one kscreen-doctor operation and wait for it.
    void run_kscreen(const std::string &argument) {
      const auto exe = tool_path(KSCREEN_HELPER);
      if (exe.empty()) {
        warn_helper_unavailable(KSCREEN_HELPER);
        return;
      }

      // Only ever called from the launch path, where spawning a helper is known to work. Do not
      // move this into a detached thread: there the process' own SIGCHLD handling reaps our child
      // first, so the wait fails and the output comes back empty (see the note above kwin_call).
      capture_stdout({exe, argument});
    }

    std::string kscreen_uuid_of(const std::string &name) {
      for (const auto &output : kscreen_outputs()) {
        if (output.name == name) {
          return output.uuid;
        }
      }
      return {};
    }

    /// @brief `config_option_e` -> the string kscreen/the hook use.
    std::string topology_mode_name(config::video_t::dd_t::config_option_e mode) {
      using e = config::video_t::dd_t::config_option_e;
      if (mode == e::verify_only) {
        return "verify_only";
      }
      if (mode == e::ensure_active) {
        return "ensure_active";
      }
      if (mode == e::ensure_primary) {
        return "ensure_primary";
      }
      if (mode == e::ensure_only_display) {
        return "ensure_only_display";
      }
      return "disabled";
    }

    /// How long to let the compositor finish its own output bookkeeping before asking again.
    constexpr auto TOPOLOGY_SETTLE = std::chrono::milliseconds {1000};

    /**
     * @brief Second, settled pass of the re-enable step in @ref apply_topology_mode.
     *
     * KWin disables the other screens by itself when a new output shows up (measured 2026-09-25), and
     * that bookkeeping can land after Sunshine's first pass — which then leaves the user's screens
     * off for the rest of the session. Ask again once the compositor has settled, but only for the
     * outputs the snapshot had on and that are off now, so the log stays honest.
     *
     * Stays on the launch thread on purpose: a detached thread cannot spawn kscreen-doctor from this
     * process (see the note above kwin_call).
     *
     * @param target_uuid uuid of the session's own output.
     */
    void reenable_outputs_after_settle(const std::string &target_uuid) {
      std::this_thread::sleep_for(TOPOLOGY_SETTLE);

      std::vector<kscreen_output_t> snapshot;
      {
        std::scoped_lock lock {g_topology_mutex};
        snapshot = g_topology_snapshot;
      }

      for (const auto &uuid : outputs_to_reenable(snapshot, kscreen_outputs(), target_uuid)) {
        BOOST_LOG(info) << "topology: re-enabling "sv << uuid << " after the compositor settled"sv;
        run_kscreen("output." + uuid + ".enable");
      }
    }

    /// @brief Shared body of @ref session_apply_topology.
    void apply_topology_mode(const std::string &target_name, const std::string &mode) {
      if (target_name.empty()) {
        return;
      }
      if (mode == "disabled"sv || mode == "verify_only"sv) {
        BOOST_LOG(info) << "topology: mode="sv << mode << " — leaving the topology untouched"sv;
        return;
      }

      // KWin registers a freshly created output in kscreen a moment later, so a query right after
      // the virtual display appears comes back empty (measured: 34 ms after creation). Wait for it
      // briefly — bounded, and on the launch thread on purpose: kscreen-doctor must not be spawned
      // from a detached thread, where the process' own SIGCHLD handling reaps our child first.
      // Snapshot the pre-session topology before touching anything, so revert restores what the
      // user had (and never the state this mode produced). Taken from KWin's own config file, which
      // lists every real output.
      {
        std::scoped_lock lock {g_topology_mutex};
        if (!g_topology_applied) {
          g_topology_snapshot.clear();
          for (const auto &output : kscreen_outputs()) {
            g_topology_snapshot.push_back(output);
          }
          g_topology_applied = true;
          // Tells us whether KWin's output configuration was actually found (an empty snapshot was
          // what exposed the XDG_CONFIG_HOME pitfall during testing).
          BOOST_LOG(info) << "topology: snapshot taken: "sv << g_topology_snapshot.size() << " output(s)"sv;
        }
      }

      auto uuid = kscreen_uuid_of(target_name);
      for (int attempt = 0; uuid.empty() && attempt < 12; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        uuid = kscreen_uuid_of(target_name);
      }
      if (uuid.empty()) {
        // A freshly created virtual output is not in KWin's output configuration (krfb's virtual
        // monitor is transient and never persisted), and it does not need to be: KWin enables and
        // gives priority 1 to a new output on its own, which is exactly what ensure_active and
        // ensure_primary ask for. Carry on to the guard below, which still protects the outputs
        // KWin does know about.
        BOOST_LOG(info) << "topology: target ["sv << target_name
                        << "] is not in KWin's output configuration; the compositor set it up (virtual output)"sv;
      }
      else {
        run_kscreen("output." + uuid + ".enable");
        if (mode == "ensure_primary"sv) {
          run_kscreen("output." + uuid + ".priority.1");
        }
      }

      if (mode == "ensure_only_display"sv) {
        // Runs even for a transient virtual target: "only" is the whole point of this mode, and it
        // acts on the outputs KWin knows about (the virtual one is the only enabled one from the
        // compositor's side already). Verified case that made this necessary: with the target
        // skipped, the mode silently degraded to ensure_primary and left eDP-1 on.
        for (const auto &output : kscreen_outputs()) {
          if (output.enabled && output.uuid != uuid) {
            BOOST_LOG(info) << "topology: disabling "sv << output.uuid << " ("sv << output.name
                            << ") for ensure_only_display"sv;
            run_kscreen("output." + output.uuid + ".disable");
          }
        }
      }

      if (mode == "ensure_active"sv || mode == "ensure_primary"sv) {
        // KWin re-ranks on its own when a new output shows up (it has been measured disabling the
        // other screens at that moment), while these two modes only ever change the target. Put the
        // outputs the snapshot had on back on — idempotent, so harmless when KWin left them alone —
        // and repeat that once the compositor has settled, because this first pass can beat KWin's
        // own bookkeeping by a frame (the race measured on 2026-09-25).
        std::vector<kscreen_output_t> snapshot;
        {
          std::scoped_lock lock {g_topology_mutex};
          snapshot = g_topology_snapshot;
        }
        for (const auto &state : snapshot) {
          if (state.enabled && state.uuid != uuid) {
            run_kscreen("output." + state.uuid + ".enable");
          }
        }

        reenable_outputs_after_settle(uuid);
      }

      BOOST_LOG(info) << "topology: mode="sv << mode << " applied to "sv << target_name << " ("sv << uuid << ')';
    }
  }  // namespace

  std::vector<kscreen_output_t> kscreen_outputs_from_json_text(const std::string &text) {
    std::vector<kscreen_output_t> outputs;

    try {
      const auto data = nlohmann::json::parse(text);

      std::vector<raw_output_t> descriptors;
      std::vector<std::pair<std::size_t, std::pair<bool, int>>> states;
      collect_outputs(data, descriptors, states);

      for (const auto &descriptor : descriptors) {
        kscreen_output_t output;
        output.uuid = descriptor.uuid;
        output.name = descriptor.connector;

        // Match the state by outputIndex; when a setup nests differently and no state matches, the
        // conservative defaults (off, no priority) keep the caller from acting on a wrong output.
        for (const auto &state : states) {
          if (state.first == descriptor.index) {
            output.enabled = state.second.first;
            output.priority = state.second.second;
            break;
          }
        }

        // The same uuid appears in more than one setup (lid open/closed): keep the enabled variant.
        bool merged = false;
        for (auto &existing : outputs) {
          if (existing.uuid != descriptor.uuid) {
            continue;
          }
          if (!existing.enabled && output.enabled) {
            existing = output;
          }
          merged = true;
          break;
        }
        if (!merged) {
          outputs.push_back(output);
        }
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "topology: could not parse the KWin output configuration: "sv << e.what();
    }

    return outputs;
  }

  std::vector<std::string> outputs_to_reenable(const std::vector<kscreen_output_t> &snapshot,
                                              const std::vector<kscreen_output_t> &current,
                                              const std::string &target_uuid) {
    std::vector<std::string> uuids;

    for (const auto &before : snapshot) {
      if (!before.enabled || before.uuid.empty() || before.uuid == target_uuid) {
        continue;
      }

      const auto now = std::find_if(current.begin(), current.end(), [&](const kscreen_output_t &output) {
        return output.uuid == before.uuid;
      });

      // An output the compositor no longer lists counts as off: the enable is a no-op there, while
      // staying silent would hide a screen that did not come back.
      if (now == current.end() || !now->enabled) {
        uuids.push_back(before.uuid);
      }
    }

    return uuids;
  }

  void session_apply_topology(const std::string &target_name) {
    apply_topology_mode(target_name, topology_mode_name(config::video.dd.configuration_option));
  }

  void session_revert_topology() {
    std::vector<kscreen_output_t> snapshot;
    {
      std::scoped_lock lock {g_topology_mutex};
      if (!g_topology_applied) {
        return;
      }
      snapshot = g_topology_snapshot;
      g_topology_snapshot.clear();
      g_topology_applied = false;
    }

    if (snapshot.empty()) {
      BOOST_LOG(info) << "topology: revert: no snapshot — nothing to restore"sv;
      return;
    }

    for (const auto &state : snapshot) {
      if (!state.enabled) {
        run_kscreen("output." + state.uuid + ".disable");
        continue;
      }

      run_kscreen("output." + state.uuid + ".enable");
      if (state.priority > 0) {
        run_kscreen("output." + state.uuid + ".priority." + std::to_string(state.priority));
      }
      const auto space = state.geometry.find(' ');
      if (space != std::string::npos) {
        run_kscreen("output." + state.uuid + ".position." + state.geometry.substr(0, space));
      }
    }

    BOOST_LOG(info) << "topology: revert: pre-session topology restored"sv;
  }

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
    const bool helper = !tool_path(VIRTUAL_DISPLAY_HELPER).empty();
    const bool kscreen = !tool_path(KSCREEN_HELPER).empty();

    // This probe backs the client display list, so it is the only place that can explain a missing
    // virtual display id: on a host without the helpers nothing else runs, and the ids simply are
    // not offered. Keep the message here rather than in the lookup itself, which also serves tools
    // that have nothing to do with the virtual display.
    if (!helper) {
      warn_helper_unavailable(VIRTUAL_DISPLAY_HELPER);
    }
    if (!kscreen) {
      warn_helper_unavailable(KSCREEN_HELPER);
    }

    return helper && kscreen;
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
