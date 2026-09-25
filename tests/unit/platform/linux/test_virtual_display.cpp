/**
 * @file tests/unit/platform/linux/test_virtual_display.cpp
 * @brief Tests for the Linux virtual-display helper lookup.
 */

#ifdef __linux__

  // standard includes
  #include <algorithm>
  #include <filesystem>
  #include <fstream>
  #include <string>
  #include <system_error>
  #include <unistd.h>

  // lib includes
  #include <gtest/gtest.h>

  // local includes
  #include "src/platform/linux/virtual_display.h"

namespace {
  /// Executable name that exists nowhere, so a lookup for it can only end empty.
  constexpr auto MISSING_TOOL = "sunshine-test-helper-that-does-not-exist";

  /**
   * @brief Directory holding fake helper executables for one test.
   *
   * The lookup must be testable without touching the machine: a host that has kscreen installed
   * would otherwise decide the outcome of a test that asserts an empty result.
   */
  class fake_tool_dir_t {
  public:
    /**
     * @brief Create an empty directory below the test temp directory.
     *
     * @param name Directory suffix; the process id is appended so parallel runs do not collide.
     */
    explicit fake_tool_dir_t(const std::string &name) {
      path_ = std::filesystem::temp_directory_path() /
              ("sunshine-vdisplay-" + name + "-" + std::to_string(::getpid()));
      std::error_code error;
      std::filesystem::remove_all(path_, error);
      std::filesystem::create_directories(path_);
    }

    /**
     * @brief Remove the directory and everything inside it.
     */
    ~fake_tool_dir_t() {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }

    fake_tool_dir_t(const fake_tool_dir_t &) = delete;
    fake_tool_dir_t &operator=(const fake_tool_dir_t &) = delete;

    /**
     * @brief Path of the directory, as used in a `PATH`-shaped search list.
     *
     * @return Directory path.
     */
    std::string path() const {
      return path_.string();
    }

    /**
     * @brief Create one file in the directory.
     *
     * @param name File name.
     * @param executable Whether the file is given an execute bit.
     * @return Absolute path of the created file.
     */
    std::string create(const std::string &name, bool executable) const {
      const auto file = path_ / name;
      std::ofstream stream {file};
      stream << "#!/bin/sh\n";
      stream.close();

      auto permissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
      if (executable) {
        permissions |= std::filesystem::perms::owner_exec;
      }
      std::filesystem::permissions(file, permissions, std::filesystem::perm_options::replace);

      return file.string();
    }

  private:
    std::filesystem::path path_;  ///< Directory that holds the fake helpers.
  };

  /**
   * @brief Whether a directory list contains @p directory.
   *
   * @param dirs Directory list to search.
   * @param directory Directory to look for.
   * @return True when present.
   */
  bool contains_dir(const std::vector<std::string> &dirs, const std::string &directory) {
    return std::ranges::find(dirs, directory) != dirs.end();
  }
}  // namespace

TEST(VirtualDisplayHelperLookup, PrefersTheConfiguredPath) {
  const fake_tool_dir_t configured_dir {"configured"};
  const fake_tool_dir_t path_dir {"configured-path"};
  const auto configured = configured_dir.create(platf::KSCREEN_HELPER, true);
  const auto from_path = path_dir.create(platf::KSCREEN_HELPER, true);

  EXPECT_EQ(platf::find_helper(platf::KSCREEN_HELPER, configured, path_dir.path()), configured);
  EXPECT_NE(configured, from_path);
}

TEST(VirtualDisplayHelperLookup, UsesThePathWhenNothingIsConfigured) {
  const fake_tool_dir_t path_dir {"path"};
  const auto from_path = path_dir.create(platf::VIRTUAL_DISPLAY_HELPER, true);

  EXPECT_EQ(platf::find_helper(platf::VIRTUAL_DISPLAY_HELPER, "", path_dir.path()), from_path);
}

TEST(VirtualDisplayHelperLookup, SkipsAnEntryThatNothingCanExecute) {
  const fake_tool_dir_t plain_dir {"not-executable"};
  const fake_tool_dir_t executable_dir {"executable"};
  plain_dir.create(platf::KSCREEN_HELPER, false);
  const auto executable = executable_dir.create(platf::KSCREEN_HELPER, true);

  const auto found = platf::find_helper(
    platf::KSCREEN_HELPER,
    "",
    plain_dir.path() + ":" + executable_dir.path()
  );

  EXPECT_EQ(found, executable);
}

TEST(VirtualDisplayHelperLookup, IgnoresAnUnusableConfiguredPathAndSearchesOn) {
  const fake_tool_dir_t configured_dir {"unusable-configured"};
  const fake_tool_dir_t path_dir {"fallback-path"};
  const auto not_executable = configured_dir.create(platf::KSCREEN_HELPER, false);
  const auto from_path = path_dir.create(platf::KSCREEN_HELPER, true);

  EXPECT_EQ(platf::find_helper(platf::KSCREEN_HELPER, not_executable, path_dir.path()), from_path);
}

TEST(VirtualDisplayHelperLookup, SkipsEmptyPathEntries) {
  const fake_tool_dir_t path_dir {"empty-entries"};
  const auto from_path = path_dir.create(platf::KSCREEN_HELPER, true);

  EXPECT_EQ(platf::find_helper(platf::KSCREEN_HELPER, "", "::" + path_dir.path() + ":"), from_path);
}

TEST(VirtualDisplayHelperLookup, ReturnsEmptyWhenNoCandidateCarriesTheTool) {
  const fake_tool_dir_t empty_dir {"empty"};

  EXPECT_TRUE(platf::find_helper(MISSING_TOOL, "", "").empty());
  EXPECT_TRUE(platf::find_helper(MISSING_TOOL, "", empty_dir.path()).empty());
  EXPECT_TRUE(platf::find_helper(MISSING_TOOL, "/nonexistent/" + std::string {MISSING_TOOL}, "").empty());
}

TEST(VirtualDisplayHelperLookup, SearchesTheStandardDirectories) {
  const auto dirs = platf::helper_fallback_dirs();

  EXPECT_TRUE(contains_dir(dirs, "/usr/bin"));
  EXPECT_TRUE(contains_dir(dirs, "/usr/local/bin"));
  EXPECT_TRUE(contains_dir(dirs, "/run/current-system/sw/bin"));
  // A portable install may ship the helpers beside the executable, so its own directory is a
  // candidate too (the directory itself resolves through /proc/self/exe on Linux).
  EXPECT_GE(dirs.size(), 4U);
}

TEST(KwinOutputConfigParsing, ReadsDescriptorsWithTheirConnectorNames) {
  const auto outputs = platf::kscreen_outputs_from_json_text(R"([
    {
      "name": "",
      "data": [
        {"uuid": "uuid-edp", "connectorName": "eDP-1", "mode": {"width": 2560, "height": 1600}},
        {"uuid": "uuid-dp", "connectorName": "DP-1"}
      ]
    }
  ])");

  ASSERT_EQ(outputs.size(), 2U);
  EXPECT_EQ(outputs[0].uuid, "uuid-edp");
  EXPECT_EQ(outputs[0].name, "eDP-1");
  EXPECT_EQ(outputs[1].uuid, "uuid-dp");
  EXPECT_EQ(outputs[1].name, "DP-1");
}

TEST(KwinOutputConfigParsing, AppliesTheStateMatchedByOutputIndex) {
  const auto outputs = platf::kscreen_outputs_from_json_text(R"([
    {
      "name": "",
      "data": [{"uuid": "uuid-dp", "connectorName": "DP-1"}],
      "outputs": [{"outputIndex": 0, "enabled": true, "priority": 2}]
    }
  ])");

  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_TRUE(outputs[0].enabled);
  EXPECT_EQ(outputs[0].priority, 2);
}

TEST(KwinOutputConfigParsing, KeepsTheConservativeDefaultsWithoutState) {
  // The document shape that has no state entries at all (the plain setup in the target host's
  // file); a caller must never act on an output the file did not mark as on.
  const auto outputs = platf::kscreen_outputs_from_json_text(R"([
    {"name": "", "data": [{"uuid": "uuid-dp", "connectorName": "DP-1"}]}
  ])");

  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_FALSE(outputs[0].enabled);
  EXPECT_EQ(outputs[0].priority, 0);
  EXPECT_TRUE(outputs[0].geometry.empty());
}

TEST(KwinOutputConfigParsing, UsesTheNameKeyWhenThereIsNoConnectorName) {
  const auto outputs = platf::kscreen_outputs_from_json_text(R"([
    {"name": "", "data": [{"uuid": "uuid-hdmi", "name": "HDMI-A-1"}]}
  ])");

  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs[0].name, "HDMI-A-1");
}

TEST(KwinOutputConfigParsing, IgnoresEntriesWithoutAUsableUuid) {
  const auto outputs = platf::kscreen_outputs_from_json_text(R"([
    {
      "name": "",
      "data": [
        {"uuid": "", "connectorName": "DP-1"},
        {"connectorName": "DP-2"},
        {"uuid": 7, "connectorName": "DP-3"},
        {"uuid": "uuid-good", "connectorName": "DP-4"}
      ]
    }
  ])");

  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs[0].name, "DP-4");
}

TEST(KwinOutputConfigParsing, KeepsTheEnabledVariantOfADuplicatedUuid) {
  // The same output appears once per setup (lid open/closed); only the enabled description may win.
  const auto outputs = platf::kscreen_outputs_from_json_text(R"([
    {
      "name": "first",
      "data": [{"uuid": "uuid-a", "connectorName": "DP-2"}, {"uuid": "uuid-shared", "connectorName": "DP-1"}],
      "outputs": [{"outputIndex": 1, "enabled": false, "priority": 0}]
    },
    {
      "name": "second",
      "data": [{"uuid": "uuid-shared", "connectorName": "DP-1"}],
      "outputs": [{"outputIndex": 0, "enabled": true, "priority": 1}]
    }
  ])");

  ASSERT_EQ(outputs.size(), 2U);
  EXPECT_EQ(outputs[1].uuid, "uuid-shared");
  EXPECT_TRUE(outputs[1].enabled);
  EXPECT_EQ(outputs[1].priority, 1);
}

TEST(KwinOutputConfigParsing, WalksTheNestedLidVariants) {
  // Measured shape of the target host's ~/.config/kwinoutputconfig.json: a first setup listing the
  // outputs, then a setup whose entries carry the per-lid state one level deeper.
  const auto outputs = platf::kscreen_outputs_from_json_text(R"([
    {
      "name": "",
      "data": [
        {"uuid": "uuid-edp", "connectorName": "eDP-1", "mode": {"width": 2560, "height": 1600, "refreshRate": 60002}},
        {"uuid": "uuid-krfb", "connectorName": "Virtual-SunshineVirt"}
      ]
    },
    {
      "name": "lid",
      "data": [
        {
          "lidClosed": false,
          "outputs": [
            {"outputIndex": 0, "enabled": true, "priority": 1},
            {"outputIndex": 1, "enabled": false, "priority": 0}
          ]
        }
      ]
    }
  ])");

  ASSERT_EQ(outputs.size(), 2U);
  EXPECT_EQ(outputs[0].name, "eDP-1");
  EXPECT_TRUE(outputs[0].enabled);
  EXPECT_EQ(outputs[0].priority, 1);
  EXPECT_EQ(outputs[1].name, "Virtual-SunshineVirt");
  EXPECT_FALSE(outputs[1].enabled);
}

TEST(KwinOutputConfigParsing, ReturnsEmptyForUnusableText) {
  EXPECT_TRUE(platf::kscreen_outputs_from_json_text("").empty());
  EXPECT_TRUE(platf::kscreen_outputs_from_json_text("{not json").empty());
  EXPECT_TRUE(platf::kscreen_outputs_from_json_text("[]").empty());
  EXPECT_TRUE(platf::kscreen_outputs_from_json_text(R"({"outputs": 3})").empty());
}

namespace {
  /**
   * @brief Build one output entry for the re-enable decision tests.
   *
   * @param uuid Output uuid.
   * @param enabled Whether the output is on.
   * @return Output entry.
   */
  platf::kscreen_output_t output(const std::string &uuid, bool enabled) {
    platf::kscreen_output_t entry;
    entry.uuid = uuid;
    entry.enabled = enabled;
    return entry;
  }
}  // namespace

TEST(TopologyReenable, PicksUpOutputsTheCompositorSwitchedOff) {
  const std::vector<platf::kscreen_output_t> snapshot {
    output("uuid-edp", true),
    output("uuid-hdmi", true),
  };
  // The measured race: KWin turned both off by itself once the new output appeared.
  const std::vector<platf::kscreen_output_t> current {
    output("uuid-edp", false),
    output("uuid-hdmi", false),
  };

  EXPECT_EQ(platf::outputs_to_reenable(snapshot, current, "uuid-virtual"),
            (std::vector<std::string> {"uuid-edp", "uuid-hdmi"}));
}

TEST(TopologyReenable, LeavesOutputsThatAreStillOnAlone) {
  const std::vector<platf::kscreen_output_t> snapshot {output("uuid-edp", true)};
  const std::vector<platf::kscreen_output_t> current {output("uuid-edp", true)};

  EXPECT_TRUE(platf::outputs_to_reenable(snapshot, current, "uuid-virtual").empty());
}

TEST(TopologyReenable, NeverTouchesTheSessionsOwnOutput) {
  const std::vector<platf::kscreen_output_t> snapshot {output("uuid-virtual", true)};
  const std::vector<platf::kscreen_output_t> current {output("uuid-virtual", false)};

  EXPECT_TRUE(platf::outputs_to_reenable(snapshot, current, "uuid-virtual").empty());
}

TEST(TopologyReenable, IgnoresOutputsTheSnapshotHadOff) {
  const std::vector<platf::kscreen_output_t> snapshot {
    output("uuid-off", false),
    output("", true),
  };
  const std::vector<platf::kscreen_output_t> current;

  EXPECT_TRUE(platf::outputs_to_reenable(snapshot, current, "uuid-virtual").empty());
}

TEST(TopologyReenable, IncludesAnOutputTheCompositorNoLongerLists) {
  const std::vector<platf::kscreen_output_t> snapshot {output("uuid-edp", true)};
  const std::vector<platf::kscreen_output_t> current {output("uuid-virtual", true)};

  EXPECT_EQ(platf::outputs_to_reenable(snapshot, current, "uuid-virtual"),
            (std::vector<std::string> {"uuid-edp"}));
}

TEST(TopologyReenable, KeepsTheSnapshotOrderAndSkipsTheOnesAlreadyBack) {
  const std::vector<platf::kscreen_output_t> snapshot {
    output("uuid-first", true),
    output("uuid-second", true),
    output("uuid-third", true),
  };
  const std::vector<platf::kscreen_output_t> current {
    output("uuid-second", true),
    output("uuid-first", false),
    output("uuid-third", false),
  };

  EXPECT_EQ(platf::outputs_to_reenable(snapshot, current, "uuid-virtual"),
            (std::vector<std::string> {"uuid-first", "uuid-third"}));
}

#endif  // __linux__
