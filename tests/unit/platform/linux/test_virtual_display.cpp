/**
 * @file tests/unit/platform/linux/test_virtual_display.cpp
 * @brief Tests for the Linux virtual-display helper lookup.
 */

#ifdef __linux__

  // standard includes
  #include <algorithm>
  #include <chrono>
  #include <filesystem>
  #include <fstream>
  #include <numeric>
  #include <string>
  #include <system_error>
  #include <thread>
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

TEST(VirtualDisplayHelperLookup, UsesAConfiguredPathThatCannotBeVerified) {
  // A configured path is the operator's decision. The executability check is only a typo catcher and
  // it misfires on this host (a store path can miss the check for minutes while the answer
  // alternates), so a miss must not send the lookup to `$PATH` or declare the helper unavailable -
  // that removed the virtual display options from every client's list while the helper was fine.
  const fake_tool_dir_t configured_dir {"unverified-configured"};
  const fake_tool_dir_t path_dir {"fallback-path-ignored"};
  const auto not_executable = configured_dir.create(platf::KSCREEN_HELPER, false);
  path_dir.create(platf::KSCREEN_HELPER, true);

  EXPECT_EQ(platf::find_helper(platf::KSCREEN_HELPER, not_executable, path_dir.path()), not_executable);
}

TEST(VirtualDisplayHelperLookup, SkipsEmptyPathEntries) {
  const fake_tool_dir_t path_dir {"empty-entries"};
  const auto from_path = path_dir.create(platf::KSCREEN_HELPER, true);

  EXPECT_EQ(platf::find_helper(platf::KSCREEN_HELPER, "", "::" + path_dir.path() + ":"), from_path);
}

TEST(VirtualDisplayHelperLookup, ReturnsEmptyWhenNothingIsConfiguredAndNoCandidateCarriesTheTool) {
  const fake_tool_dir_t empty_dir {"empty"};

  EXPECT_TRUE(platf::find_helper(MISSING_TOOL, "", "").empty());
  EXPECT_TRUE(platf::find_helper(MISSING_TOOL, "", empty_dir.path()).empty());
}

TEST(VirtualDisplayHelperLookup, UsesAConfiguredPathThatDoesNotExist) {
  // Same policy as `UsesAConfiguredPathThatCannotBeVerified`: the check cannot be trusted on this
  // host, so a configured path is used even when nothing confirms it. A path that is really unusable
  // fails where the helper is started instead of hiding the virtual display from every client.
  const std::string configured = "/nonexistent/" + std::string {MISSING_TOOL};

  EXPECT_EQ(platf::find_helper(MISSING_TOOL, configured, ""), configured);
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

TEST(DisplayPickResolution, PassesAPhysicalConnectorNameThrough) {
  const auto pick = platf::resolve_display_pick("DP-1", "");

  EXPECT_EQ(pick.name, "DP-1");
  EXPECT_TRUE(pick.virtual_display.empty());
}

TEST(DisplayPickResolution, MapsTheVirtualKWinIdToTheCreatedOutput) {
  const auto pick = platf::resolve_display_pick(platf::VDISPLAY_KWIN_ID, "");

  EXPECT_EQ(pick.name, "Virtual-SunshineVirt");
  EXPECT_EQ(pick.virtual_display, platf::VIRTUAL_DISPLAY_HOOK_KWIN);
}

TEST(DisplayPickResolution, MapsTheVirtualKmsIdToTheCreatedOutput) {
  const auto pick = platf::resolve_display_pick(platf::VDISPLAY_KMS_ID, "");

  EXPECT_EQ(pick.name, "Virtual-SunshineVirt");
  EXPECT_EQ(pick.virtual_display, platf::VIRTUAL_DISPLAY_HOOK_KMS);
}

TEST(DisplayPickResolution, LeavesAnUnknownNameAlone) {
  const auto pick = platf::resolve_display_pick("虚拟-Other", "");

  EXPECT_EQ(pick.name, "虚拟-Other");
  EXPECT_TRUE(pick.virtual_display.empty());
}

TEST(DisplayPickResolution, AddsTheSwitchForTheHostConfigPickWhenItNamesTheVirtualOutput) {
  // The client's "默认" entry mirrors `output_name`; without the switch the do-hook no-ops and the
  // capture waits for a monitor nobody creates.
  const auto pick = platf::resolve_display_pick("", "Virtual-SunshineVirt");

  EXPECT_TRUE(pick.name.empty());
  EXPECT_EQ(pick.virtual_display, platf::VIRTUAL_DISPLAY_HOOK_KWIN);
}

TEST(DisplayPickResolution, AddsNothingForAHostConfigPickOnAPhysicalOutput) {
  const auto pick = platf::resolve_display_pick("", "DP-1");

  EXPECT_TRUE(pick.name.empty());
  EXPECT_TRUE(pick.virtual_display.empty());
}

TEST(DisplayPickResolution, AddsNothingWhenTheClientPickedNothingAndNoOutputIsConfigured) {
  const auto pick = platf::resolve_display_pick("", "");

  EXPECT_TRUE(pick.name.empty());
  EXPECT_TRUE(pick.virtual_display.empty());
}

TEST(VirtualDisplayIdentity, DefaultsToTheStandardHelperAndPort) {
  const auto identity = platf::resolve_virtual_display_identity("", "");

  EXPECT_EQ(identity.name, platf::VIRTUAL_DISPLAY_NAME);
  EXPECT_EQ(identity.output_name, "Virtual-SunshineVirt");
  EXPECT_EQ(identity.port, platf::VIRTUAL_DISPLAY_PORT);
  EXPECT_FALSE(identity.name_override_ignored);
  EXPECT_FALSE(identity.port_override_ignored);
}

TEST(VirtualDisplayIdentity, UsesTheNameOverrideForBothTheHelperAndTheOutput) {
  const auto identity = platf::resolve_virtual_display_identity("ProbeVirt", "");

  EXPECT_EQ(identity.name, "ProbeVirt");
  EXPECT_EQ(identity.output_name, "Virtual-ProbeVirt");
  EXPECT_EQ(identity.port, platf::VIRTUAL_DISPLAY_PORT);
  EXPECT_FALSE(identity.name_override_ignored);
}

TEST(VirtualDisplayIdentity, TrimsTheNameOverride) {
  const auto identity = platf::resolve_virtual_display_identity("  ProbeVirt\t", "");

  EXPECT_EQ(identity.name, "ProbeVirt");
  EXPECT_EQ(identity.output_name, "Virtual-ProbeVirt");
}

TEST(VirtualDisplayIdentity, FallsBackAndReportsABlankName) {
  const auto identity = platf::resolve_virtual_display_identity("   ", "");

  EXPECT_EQ(identity.name, platf::VIRTUAL_DISPLAY_NAME);
  EXPECT_EQ(identity.output_name, "Virtual-SunshineVirt");
  EXPECT_TRUE(identity.name_override_ignored);
}

TEST(VirtualDisplayIdentity, UsesThePortOverride) {
  const auto identity = platf::resolve_virtual_display_identity("", "5920");

  EXPECT_EQ(identity.port, 5920);
  EXPECT_FALSE(identity.port_override_ignored);
}

TEST(VirtualDisplayIdentity, FallsBackAndReportsAnUnusablePort) {
  for (const auto *value : {"abc", "0", "-5", "70000", "5910abc", "5920 5", "  "}) {
    const auto identity = platf::resolve_virtual_display_identity("", value);

    EXPECT_EQ(identity.port, platf::VIRTUAL_DISPLAY_PORT) << "for [" << value << ']';
    EXPECT_TRUE(identity.port_override_ignored) << "for [" << value << ']';
  }
}

TEST(VirtualDisplayIdentity, AppliesBothOverridesTogether) {
  const auto identity = platf::resolve_virtual_display_identity("ProbeVirt", "5920");

  EXPECT_EQ(identity.name, "ProbeVirt");
  EXPECT_EQ(identity.output_name, "Virtual-ProbeVirt");
  EXPECT_EQ(identity.port, 5920);
  EXPECT_FALSE(identity.name_override_ignored);
  EXPECT_FALSE(identity.port_override_ignored);
}

TEST(KscreenOutputEnabled, ReadsTheBlockOfTheOutputThatWasAskedAbout) {
  const std::string layout =
    "Output: 1 eDP-1 d0212254-4127-41d2-9894-898eabae7a38\n"
    "\tenabled\n"
    "\tpriority 2\n"
    "Output: 2 Virtual-SunshineVirt 90d4ef5c-f168-4ff4-ae20-c8d7009864cc\n"
    "\tdisabled\n"
    "\tpriority 1\n";

  EXPECT_TRUE(platf::kscreen_output_is_enabled(layout, "eDP-1"));
  EXPECT_FALSE(platf::kscreen_output_is_enabled(layout, "Virtual-SunshineVirt"));
}

TEST(KscreenOutputEnabled, DoesNotLetOtherStateLinesAnswerForTheOutput) {
  // The block carries per-feature states of its own (`HDR: disabled` above all); only a bare
  // `enabled` token means the output itself is on.
  const std::string layout =
    "Output: 2 Virtual-SunshineVirt 90d4ef5c-f168-4ff4-ae20-c8d7009864cc\n"
    "\tdisabled\n"
    "\tHDR: enabled\n";

  EXPECT_FALSE(platf::kscreen_output_is_enabled(layout, "Virtual-SunshineVirt"));
}

TEST(KscreenOutputEnabled, ToleratesCarriageReturnsAndTrailingBlanks) {
  const std::string layout = "Output: 2 Virtual-SunshineVirt 90d4ef5c\r\n\tenabled  \r\n";

  EXPECT_TRUE(platf::kscreen_output_is_enabled(layout, "Virtual-SunshineVirt"));
}

TEST(KscreenOutputEnabled, ReadsAColouredListing) {
  // Regression: `kscreen-doctor -o` colours its output even through a pipe, so the block line
  // started with an escape sequence rather than `Output: `, and the state line was never the bare
  // token `enabled`. Both checks silently failed for every real listing.
  const std::string coloured =
    "\x1b[01;32mOutput: \x1b[0;0m1 eDP-1 d0212254-4127-41d2-9894-898eabae7a38\n"
    "\t\x1b[01;31mdisabled\x1b[0;0m\n"
    "\x1b[01;32mOutput: \x1b[0;0m2 Virtual-SunshineVirt 90d4ef5c-f168-4ff4-ae20-c8d7009864cc\n"
    "\t\x1b[01;32menabled\x1b[0;0m\n";

  EXPECT_TRUE(platf::kscreen_output_is_enabled(coloured, "Virtual-SunshineVirt"));
  EXPECT_FALSE(platf::kscreen_output_is_enabled(coloured, "eDP-1"));
}

TEST(AnsiStrip, RemovesTheSequencesKscreenDoctorEmits) {
  const std::string coloured =
    "\x1b[01;32mOutput: \x1b[0;0m2 Virtual-SunshineVirt 90d4ef5c\n"
    "\t\x1b[01;31mdisabled\x1b[0;0m\n";

  EXPECT_EQ(platf::strip_ansi(coloured), "Output: 2 Virtual-SunshineVirt 90d4ef5c\n\tdisabled\n");
}

TEST(AnsiStrip, LeavesPlainTextAlone) {
  EXPECT_EQ(platf::strip_ansi("Output: 1 eDP-1\n\tdisabled\n"), "Output: 1 eDP-1\n\tdisabled\n");
  EXPECT_EQ(platf::strip_ansi(""), "");
}

TEST(AnsiStrip, HandlesTheTwoCharacterForm) {
  EXPECT_EQ(platf::strip_ansi("a\x1b" "b"), "a");
  EXPECT_EQ(platf::strip_ansi("\x1b[01;32menabled\x1b[0;0m"), "enabled");
}

TEST(KscreenOutputEnabled, RejectsAnOutputThatIsNotListed) {
  EXPECT_FALSE(platf::kscreen_output_is_enabled("Output: 1 eDP-1 abc\n\tenabled\n", "Virtual-Nope"));
  EXPECT_FALSE(platf::kscreen_output_is_enabled("", "eDP-1"));
}

TEST(VirtualDisplayStartBudget, RetriesMoreThanOnceAndStaysInsideTheClientWait) {
  const auto budgets = platf::helper_start_poll_budgets();

  // More than one attempt: a single one loses whenever the helper path is refused for a moment.
  EXPECT_GT(budgets.size(), 1u);

  // Attempts never shrink, so a slow-but-working helper is not cut short by a later attempt that
  // would get less time than the one before it.
  EXPECT_TRUE(std::is_sorted(budgets.begin(), budgets.end()));

  // The start window replaces a single 8 s wait: it must not be shorter than the few seconds a
  // helper may legitimately need, and it has to stay inside what a client waits for a session.
  const auto total = std::accumulate(budgets.begin(), budgets.end(), 0) * std::chrono::milliseconds {300};
  EXPECT_GE(total, std::chrono::seconds {6});
  EXPECT_LE(total, std::chrono::seconds {9});
}

TEST(DisplayPickResolution, TranslatesAVirtualPickToTheOverriddenOutput) {
  const auto identity = platf::resolve_virtual_display_identity("ProbeVirt", "5920");

  const auto kwin = platf::resolve_display_pick(platf::VDISPLAY_KWIN_ID, "", identity);
  EXPECT_EQ(kwin.name, "Virtual-ProbeVirt");
  EXPECT_EQ(kwin.virtual_display, platf::VIRTUAL_DISPLAY_HOOK_KWIN);

  const auto kms = platf::resolve_display_pick(platf::VDISPLAY_KMS_ID, "", identity);
  EXPECT_EQ(kms.name, "Virtual-ProbeVirt");
  EXPECT_EQ(kms.virtual_display, platf::VIRTUAL_DISPLAY_HOOK_KMS);
}

TEST(DisplayPickResolution, MatchesTheHostConfigPickAgainstTheOverriddenOutput) {
  const auto identity = platf::resolve_virtual_display_identity("ProbeVirt", "");

  EXPECT_EQ(platf::resolve_display_pick("", "Virtual-ProbeVirt", identity).virtual_display,
            platf::VIRTUAL_DISPLAY_HOOK_KWIN);
  EXPECT_TRUE(platf::resolve_display_pick("", "Virtual-SunshineVirt", identity).virtual_display.empty());
}

TEST(VirtualDisplayHelperLookup, PicksUpAHelperThatAppearsWhileTheCheckRetries) {
  // A store path can miss an executability check and answer the next one - which is why the lookup
  // retries - so a helper that shows up a moment later must still be found instead of being declared
  // unavailable (that declaration removes the virtual display options from the client's list).
  const fake_tool_dir_t dir {"retry"};
  const auto tool = dir.path() + "/" + platf::VIRTUAL_DISPLAY_HELPER;

  std::thread creator([&dir] {
    std::this_thread::sleep_for(std::chrono::milliseconds {40});
    dir.create(platf::VIRTUAL_DISPLAY_HELPER, true);
  });

  const auto found = platf::find_helper(platf::VIRTUAL_DISPLAY_HELPER, tool, dir.path());
  creator.join();

  EXPECT_EQ(found, tool);
}

#endif  // __linux__
