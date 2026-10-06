#include "runtime_pc_settings.h"
#include "runtime_auto_scale.h"
#include <toml++/toml.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace {
bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

template <typename Callback>
bool Throws(Callback&& callback) {
  try {
    callback();
  } catch (const std::runtime_error&) {
    return true;
  }
  return false;
}
}  // namespace

int main() {
  bool passed = true;
  const auto executable_directory =
      std::filesystem::temp_directory_path() / "darkness pc candidate";

  {
    const char* argv[] = {"TheDarkness.exe"};
    const auto options = ParseRuntimeLaunchOptions(
        1, argv, executable_directory);
    passed &= Check(options.xexPath ==
                        std::filesystem::absolute(
                            executable_directory /
                            "Darkness, The (USA, Europe) (En,Fr,De,Es,It)" /
                            "default.xex")
                            .lexically_normal(),
                    "missing default XEX must use an executable-relative path");
    passed &= Check(options.pcConfigPath ==
                        std::filesystem::absolute(
                            executable_directory / "TheDarkness.pc.toml")
                            .lexically_normal(),
                    "default PC config must be beside the executable");
    passed &= Check(options.modsConfigPath ==
                        std::filesystem::absolute(
                            executable_directory / "TheDarkness.mods.toml")
                            .lexically_normal(),
                    "default mod config must be beside the executable");
    passed &= Check(options.userDataRoot ==
                        std::filesystem::absolute(
                            executable_directory / "runtime_data")
                            .lexically_normal(),
                    "default user data must preserve portable runtime_data");
    passed &= Check(options.action == RuntimeLaunchAction::Run,
                    "default launch action must run the title");
    passed &= Check(!options.pcConfigExplicit,
                    "the optional adjacent config must not be marked explicit");
    passed &= Check(!options.inputDiagnostics,
                    "separate input observer must be disabled by default");
    passed &= Check(!options.frameCadenceDiagnostics,
                    "frame-cadence observer must be disabled by default");
    passed &= Check(!options.oneVblankExperiment,
                    "one-vblank experiment must be disabled by default");
    passed &= Check(!options.immediateDeadlineExperiment,
                    "immediate deadline experiment must be disabled by default");
    passed &= Check(options.movementPacketCompaction,
                    "movement packet compaction is on by default (V367)");
    passed &= Check(!options.hitchDiagnostics,
                    "hitch observers must be disabled by default");
    passed &= Check(!options.audioDiagnostics,
                    "audio observers must be disabled by default");
  }

  {
    const char* argv[] = {"TheDarkness.exe", "--input-diagnostics"};
    const auto options = ParseRuntimeLaunchOptions(
        2, argv, executable_directory);
    passed &= Check(options.inputDiagnostics,
                    "explicit input diagnostics must enable the bounded observer");
  }
  {
    const char* argv[] = {"TheDarkness.exe", "--experimental-one-vblank"};
    passed &= Check(ParseRuntimeLaunchOptions(2, argv, executable_directory).oneVblankExperiment,
                    "explicit experimental deadline flag accepted");
    const char* duplicate[] = {"TheDarkness.exe", "--experimental-one-vblank", "--experimental-one-vblank"};
    passed &= Check(Throws([&]() { ParseRuntimeLaunchOptions(3, duplicate, executable_directory); }),
                    "duplicate experimental flags rejected");
  }

  {
    const char* argv[] = {"TheDarkness.exe", "--experimental-immediate-deadline"};
    const auto options = ParseRuntimeLaunchOptions(2, argv, executable_directory);
    passed &= Check(options.immediateDeadlineExperiment && !options.oneVblankExperiment,
                    "immediate deadline opt-in is separate from one-vblank");
    for (const auto* first : {"--experimental-one-vblank", "--experimental-immediate-deadline"}) {
      const char* mixed[] = {"TheDarkness.exe", first,
          std::string(first) == "--experimental-one-vblank" ?
              "--experimental-immediate-deadline" : "--experimental-one-vblank"};
      passed &= Check(Throws([&]() { ParseRuntimeLaunchOptions(3, mixed, executable_directory); }),
                      "conflicting deadline experiments rejected in either order");
    }
    const char* duplicate[] = {"TheDarkness.exe", "--experimental-immediate-deadline",
                             "--experimental-immediate-deadline"};
    passed &= Check(Throws([&]() { ParseRuntimeLaunchOptions(3, duplicate, executable_directory); }),
                    "duplicate immediate deadline flag rejected");
  }

  {
    const char* argv[] = {"TheDarkness.exe", "--experimental-movement-packet-compaction"};
    const auto options = ParseRuntimeLaunchOptions(2, argv, executable_directory);
    passed &= Check(options.movementPacketCompaction &&
                    !options.immediateDeadlineExperiment && !options.oneVblankExperiment,
                    "the old opt-in spelling stays accepted and independent of render timing");
    const char* off[] = {"TheDarkness.exe", "--no-movement-packet-compaction"};
    passed &= Check(!ParseRuntimeLaunchOptions(2, off, executable_directory).movementPacketCompaction,
                    "developer A/B switch turns movement compaction off");
    const char* duplicate[] = {"TheDarkness.exe", "--experimental-movement-packet-compaction",
        "--no-movement-packet-compaction"};
    passed &= Check(Throws([&]() { ParseRuntimeLaunchOptions(3, duplicate, executable_directory); }),
                    "conflicting or duplicate movement compaction flags rejected");
  }

  passed &= Check(
      Throws([&]() {
        const char* argv[] = {"TheDarkness.exe", "--input-diagnostics",
                              "--input-diagnostics"};
        ParseRuntimeLaunchOptions(3, argv, executable_directory);
      }),
      "duplicate input diagnostics flags must fail clearly");

  {
    const char* argv[] = {"TheDarkness.exe",
                          "--frame-cadence-diagnostics"};
    const auto options = ParseRuntimeLaunchOptions(
        2, argv, executable_directory);
    passed &= Check(options.frameCadenceDiagnostics,
                    "explicit cadence diagnostics must enable the sparse observer");
  }

  passed &= Check(
      Throws([&]() {
        const char* argv[] = {"TheDarkness.exe",
                              "--frame-cadence-diagnostics",
                              "--frame-cadence-diagnostics"};
        ParseRuntimeLaunchOptions(3, argv, executable_directory);
      }),
      "duplicate cadence diagnostics flags must fail clearly");

  {
    const char* argv[] = {"TheDarkness.exe", "--hitch-diagnostics"};
    const auto options = ParseRuntimeLaunchOptions(
        2, argv, executable_directory);
    passed &= Check(options.hitchDiagnostics,
                    "explicit hitch diagnostics must enable bounded timing");
  }

  passed &= Check(
      Throws([&]() {
        const char* argv[] = {"TheDarkness.exe", "--hitch-diagnostics",
                              "--hitch-diagnostics"};
        ParseRuntimeLaunchOptions(3, argv, executable_directory);
      }),
      "duplicate hitch diagnostics flags must fail clearly");

  {
    const char* argv[] = {"TheDarkness.exe", "--audio-diagnostics"};
    const auto options = ParseRuntimeLaunchOptions(
        2, argv, executable_directory);
    passed &= Check(options.audioDiagnostics,
                    "explicit audio diagnostics must enable bounded evidence");
  }

  passed &= Check(
      Throws([&]() {
        const char* argv[] = {"TheDarkness.exe", "--audio-diagnostics",
                              "--audio-diagnostics"};
        ParseRuntimeLaunchOptions(3, argv, executable_directory);
      }),
      "duplicate audio diagnostics flags must fail clearly");

  {
    const auto discovery_root =
        std::filesystem::temp_directory_path() /
        "darkness xex ancestor discovery";
    const auto nested_executable = discovery_root / "build" / "candidate";
    const auto extracted =
        discovery_root / "Darkness, The (USA, Europe) (En,Fr,De,Es,It)";
    std::error_code error;
    std::filesystem::create_directories(nested_executable, error);
    std::filesystem::create_directories(extracted, error);
    std::ofstream(extracted / "default.xex", std::ios::binary) << "XEX2";
    const char* argv[] = {"TheDarkness.exe"};
    const auto options = ParseRuntimeLaunchOptions(1, argv, nested_executable);
    passed &= Check(options.xexPath ==
                        std::filesystem::absolute(extracted / "default.xex")
                            .lexically_normal(),
                    "default XEX discovery must walk executable ancestors");
    std::filesystem::remove_all(discovery_root, error);
  }

  {
    const char* argv[] = {"TheDarkness.exe", "--help"};
    const auto options = ParseRuntimeLaunchOptions(2, argv, executable_directory);
    passed &= Check(options.action == RuntimeLaunchAction::Help,
                    "--help must select information-only help");
    passed &= Check(std::string(RuntimeLaunchHelpText()).find("--user-data-root") !=
                        std::string::npos,
                    "help must describe the writable-root selection");
  }

  {
    const char* argv[] = {"TheDarkness.exe", "--print-paths",
                          "--user-data-root=userdata/proton"};
    const auto options = ParseRuntimeLaunchOptions(3, argv, executable_directory);
    passed &= Check(options.action == RuntimeLaunchAction::PrintPaths,
                    "--print-paths must remain information-only");
    passed &= Check(options.userDataRoot ==
                        std::filesystem::absolute("userdata/proton")
                            .lexically_normal(),
                    "information mode must still resolve explicit paths");
  }

  {
    const char* argv[] = {"TheDarkness.exe", "--list-presets"};
    const auto options = ParseRuntimeLaunchOptions(2, argv, executable_directory);
    passed &= Check(options.action == RuntimeLaunchAction::ListPresets,
                    "--list-presets must remain information-only");

    const auto preset_root = executable_directory / "presets";
    std::error_code error;
    std::filesystem::create_directories(preset_root, error);
    std::ofstream(preset_root / "z-last.toml") << "fullscreen = true\n";
    std::ofstream(preset_root / "a-first.toml") << "fullscreen = false\n";
    std::ofstream(preset_root / "ignored.txt") << "not a preset\n";
    const auto presets = RuntimePresetPaths(executable_directory);
    passed &= Check(presets.size() == 2 && presets[0] == "a-first.toml" &&
                        presets[1] == "z-last.toml",
                    "preset enumeration must be filtered and deterministic");

    const char* named_argv[] = {"TheDarkness.exe", "--preset", "A-FIRST"};
    const auto named = ParseRuntimeLaunchOptions(
        3, named_argv, executable_directory);
    passed &= Check(named.pcConfigPath ==
                        std::filesystem::absolute(
                            preset_root / "a-first.toml")
                            .lexically_normal(),
                    "named preset selection must be packaged and case-insensitive");

    const char* inline_argv[] = {"TheDarkness.exe", "--preset=z-last.toml"};
    const auto inline_named = ParseRuntimeLaunchOptions(
        2, inline_argv, executable_directory);
    passed &= Check(inline_named.pcConfigPath ==
                        std::filesystem::absolute(
                            preset_root / "z-last.toml")
                            .lexically_normal(),
                    "inline named preset selection must resolve packaged TOML");

    passed &= Check(
        Throws([&]() {
          const char* missing_argv[] = {"TheDarkness.exe", "--preset",
                                        "missing"};
          ParseRuntimeLaunchOptions(3, missing_argv, executable_directory);
        }),
        "missing packaged presets must fail instead of falling back");
    passed &= Check(
        Throws([&]() {
          const char* escaping_argv[] = {"TheDarkness.exe", "--preset",
                                         "../a-first"};
          ParseRuntimeLaunchOptions(3, escaping_argv, executable_directory);
        }),
        "preset selection must reject paths outside the package preset root");
    passed &= Check(
        Throws([&]() {
          const char* conflict_argv[] = {
              "TheDarkness.exe", "--preset", "a-first",
              "--pc-config", "settings/custom.toml"};
          ParseRuntimeLaunchOptions(5, conflict_argv, executable_directory);
        }),
        "named preset and explicit PC config selection must be exclusive");
    std::filesystem::remove_all(preset_root, error);
  }

  {
    const char* argv[] = {"TheDarkness.exe", "--print-capabilities"};
    const auto options = ParseRuntimeLaunchOptions(
        2, argv, executable_directory);
    passed &= Check(options.action == RuntimeLaunchAction::PrintCapabilities,
                    "--print-capabilities must remain information-only");
    const auto capabilities = RuntimePcCapabilityLines();
    const auto has_line = [&](const char* expected) {
      return std::find(capabilities.begin(), capabilities.end(), expected) !=
             capabilities.end();
    };
    passed &= Check(has_line("GRAPHICS_BACKEND=D3D12"),
                    "capabilities must name the implemented graphics backend");
    passed &= Check(has_line("PC_CONFIG_APPLICATION=startup_only"),
                    "capabilities must state that packaged PC settings are "
                    "applied only during startup");
    passed &= Check(has_line("PC_CONFIG_RESTART_METADATA=global_and_per_entry"),
                    "capabilities must expose per-setting restart metadata");
    passed &= Check(has_line("PC_EDITABLE_SETTINGS_SCHEMA=1"),
                    "capabilities must expose the shared editable-settings schema version");
    passed &= Check(has_line("ALT_ENTER=windowed_borderless_toggle"),
                    "capabilities must expose implemented Alt+Enter behavior");
    passed &= Check(has_line("CONTROLLER_HOTPLUG=xinput_live_poll"),
                    "capabilities must expose live XInput hot-plug polling");
    passed &= Check(has_line(
                        "CONTROLLER_BUTTON_REMAP=digital_15_to_digital_or_none"),
                    "capabilities must expose the exact physical digital-button remap scope");
    passed &= Check(has_line(
                        "CONTROLLER_PROFILE=sensitivity_medium_low_high,invert_y"),
                    "capabilities must expose only the title-native controller profile choices");
    passed &= Check(has_line(
                        "MOUSE_ACCELERATION=0..1_bounded_per_event_default_off"),
                    "capabilities must expose the exact optional mouse acceleration policy");
    passed &= Check(has_line(
                        "MOUSE_SMOOTHING=0..1_two_sample_fir_default_off"),
                    "capabilities must expose the finite optional mouse smoothing policy");
    passed &= Check(has_line(
                        "SUBTITLES=title_owned_interactive,cutscene,casual,fighting,darkness"),
                    "capabilities must preserve the title-owned subtitle controls");
    passed &= Check(has_line(
                        "USER_DATA=saved_games_default,portable_txt_game_folder,user_data_root_override,verified_copy_from_game_folder_once"),
                    "capabilities must expose the per-user, portable and managed data roots");
    passed &= Check(has_line(
                        "SETTINGS_UTILITY=TheDarknessSettings.exe_schema_frontend_v1"),
                    "capabilities must expose the packaged native settings utility");
    passed &= Check(has_line("RUNTIME_INSTANCE_POLICY=single_writer"),
                    "capabilities must disclose single-writer runtime ownership");
    passed &= Check(has_line("PRESENT_MODES=vsync,immediate,vrr"),
                    "capabilities must expose only implemented present modes");
    passed &= Check(has_line("INTERNAL_RESOLUTION_SCALE=1..7_integer"),
                    "capabilities must report the renderer's exact scale limit");
    passed &= Check(has_line("AUDIO_MASTER_VOLUME=0..1_host_pcm_output"),
                    "capabilities must expose only the implemented host-output volume control");
    passed &= Check(has_line("SCREENSHOT=F12_guest_output_bmp"),
                    "capabilities must expose the managed final-output capture action");
    passed &= Check(has_line(
                        "PERFORMANCE_OVERLAY=F3_guest_swap_fps_frame_time"),
                    "capabilities must expose the embedded guest-swap overlay");
    passed &= Check(has_line("STATUS_GUEST_HIGH_FRAME_RATE_TIMING=deferred"),
                    "capabilities must not imply a guest timing unlock");
    passed &= Check(has_line("STATUS_HDR_OUTPUT=deferred"),
                    "capabilities must not imply HDR output support");
    passed &= Check(std::string(RuntimeLaunchHelpText())
                            .find("--print-capabilities") != std::string::npos,
                    "help must describe capability reporting");
  }

  {
    const char* argv[] = {"TheDarkness.exe", "--verify-package"};
    const auto options = ParseRuntimeLaunchOptions(
        2, argv, executable_directory);
    passed &= Check(options.action == RuntimeLaunchAction::VerifyPackage,
                    "--verify-package must remain information-only");
    passed &= Check(std::string(RuntimeLaunchHelpText())
                            .find("--verify-package") != std::string::npos,
                    "help must describe package verification");
  }

  {
    const auto install_root = std::filesystem::temp_directory_path() /
                              "darkness pc preset install";
    const auto preset_root = install_root / "presets";
    std::error_code error;
    std::filesystem::remove_all(install_root, error);
    std::filesystem::create_directories(preset_root, error);
    const auto source = preset_root / "original_720p.toml";
    const auto destination = install_root / "TheDarkness.pc.toml";
    const std::string original =
        "pc_config_version = 1\noutput_resolution = \"720p\"\n";
    const std::string replacement =
        "pc_config_version = 1\noutput_resolution = \"1080p\"\n";
    std::ofstream(source, std::ios::binary) << original;

    const char* install_argv[] = {"TheDarkness.exe", "--install-preset",
                                  "original_720p"};
    const auto options = ParseRuntimeLaunchOptions(
        3, install_argv, install_root);
    passed &= Check(options.action == RuntimeLaunchAction::InstallPreset &&
                        options.pcConfigPath ==
                            std::filesystem::absolute(source).lexically_normal() &&
                        options.pcConfigInstallPath ==
                            std::filesystem::absolute(destination)
                                .lexically_normal() &&
                        !options.overwritePcConfig,
                    "--install-preset must select a contained source and adjacent destination");

    const auto first = InstallRuntimePcPreset(
        options.pcConfigPath, options.pcConfigInstallPath, false);
    passed &= Check(!first.overwritten &&
                        RuntimePcConfigInstallLines(first).back() ==
                            "PC_CONFIG_INSTALL_VALID=1",
                    "first preset installation must create a validated persistent config");
    passed &= Check(Throws([&]() {
                      InstallRuntimePcPreset(source, destination, false);
                    }),
                    "preset installation must not overwrite implicitly");
    passed &= Check(Throws([&]() {
                      InstallRuntimePcPreset(source, source, true);
                    }),
                    "preset installation must reject a source/destination alias");

    std::ofstream(source, std::ios::binary | std::ios::trunc) << replacement;
    const auto second = InstallRuntimePcPreset(source, destination, true);
    std::ifstream installed(destination, std::ios::binary);
    const std::string installedText(
        (std::istreambuf_iterator<char>(installed)),
        std::istreambuf_iterator<char>());
    installed.close();
    passed &= Check(second.overwritten && installedText == replacement,
                    "explicit overwrite must atomically replace the persistent config");

    const char* customize_argv[] = {
        "TheDarkness.exe", "--install-preset", "original_720p",
        "--set-config", "output_resolution=1440p", "--overwrite-config"};
    const auto customize_options = ParseRuntimeLaunchOptions(
        6, customize_argv, install_root);
    passed &= Check(
        customize_options.action == RuntimeLaunchAction::InstallPreset &&
            customize_options.pcConfigOverrides.size() == 1 &&
            customize_options.pcConfigOverrides[0].key ==
                "output_resolution" &&
            customize_options.pcConfigOverrides[0].value == "1440p",
        "custom preset command must retain one schema-owned override");
    const auto customized = InstallRuntimePcPresetWithOverrides(
        customize_options.pcConfigPath,
        customize_options.pcConfigInstallPath,
        customize_options.pcConfigOverrides, true);
    const auto customizedValidation =
        ValidateRuntimePcConfig(destination, true);
    const auto customizedInspection = RuntimePcConfigInspectionLines(
        destination, customizedValidation);
    passed &= Check(
        customized.overwritten && customized.overrideCount == 1 &&
            std::find(customizedInspection.begin(),
                      customizedInspection.end(),
                      "PC_CONFIG_ENTRY_0_VALUE=1440p") !=
                customizedInspection.end(),
        "schema-owned override was not atomically installed and validated");
    const auto customizedInstallLines =
        RuntimePcConfigInstallLines(customized);
    passed &= Check(
        std::find(customizedInstallLines.begin(),
                  customizedInstallLines.end(),
                  "PC_CONFIG_INSTALL_OVERRIDE_COUNT=1") !=
            customizedInstallLines.end(),
        "custom installation result lost its override count");
    std::ifstream beforeInvalidFile(destination, std::ios::binary);
    const std::string beforeInvalid(
        (std::istreambuf_iterator<char>(beforeInvalidFile)),
        std::istreambuf_iterator<char>());
    beforeInvalidFile.close();
    passed &= Check(Throws([&]() {
                      InstallRuntimePcPresetWithOverrides(
                          source, destination,
                          {{"output_resolution", "not-a-resolution"}}, true);
                    }),
                    "invalid customized value must fail runtime validation");
    std::ifstream afterInvalidFile(destination, std::ios::binary);
    const std::string afterInvalid(
        (std::istreambuf_iterator<char>(afterInvalidFile)),
        std::istreambuf_iterator<char>());
    afterInvalidFile.close();
    passed &= Check(beforeInvalid == afterInvalid,
                    "failed customization changed the installed configuration");
    bool stagedFileRemains = false;
    for (const auto& entry : std::filesystem::directory_iterator(install_root)) {
      if (entry.path().filename().string().find(
              "TheDarkness.pc.toml.custom.") == 0) {
        stagedFileRemains = true;
      }
    }
    passed &= Check(!stagedFileRemains,
                    "custom installation left a staging file behind");
    // UI-customized profiles must retain non-editable compatibility metadata.
    // Otherwise changing output/FOV in the GUI could silently drop the proven
    // native lookup-table grid policy from the consolidated2x profile.
    const std::string gridRules =
        "B29F0BF45937C4C4:FDC5E32EC6045BE1:1:324:18:6;"
        "B29F0BF45937C4C4:37AC93F53126ABB7:0:324:18:26;"
        "B29F0BF45937C4C4:54D655FC471D594A:0:324:18:26;"
        "B29F0BF45937C4C4:9F1D2D64E5F75924:0:1280:720:26:filter";
    std::ofstream(source, std::ios::binary | std::ios::trunc)
        << original << "resolution_scale = 2\n"
        << "native_resolve_region_tracking = true\nnative_resolve_region_sampling = true\n"
        << "draw_resolution_scale_native_grid_rules = \"" << gridRules << "\"\n"
        << "[camera]\nfield_of_view = 95.0\n"
        << "[audio]\nmaster_volume = 1.0\n"
        << "[input]\nmouse_sensitivity = 1.0\nmouse_invert_y = false\n"
        << "keyboard_mouse_user_index = 0\nmouse_acceleration = 0.0\nmouse_smoothing = 0.0\n"
        << "[general]\nlanguage = \"english\"\n";
    InstallRuntimePcPresetWithOverrides(source, destination,
        {{"output_resolution", "1440p"}, {"camera.field_of_view", "105"},
         {"audio.master_volume", "0.5"}}, true);
    std::ifstream gridFile(destination, std::ios::binary);
    const std::string gridText((std::istreambuf_iterator<char>(gridFile)),
                               std::istreambuf_iterator<char>());
    gridFile.close();
    passed &= Check(gridText.find(gridRules) != std::string::npos &&
                        ValidateRuntimePcConfig(destination, true).valid,
                    "GUI profile customization lost native data-grid annotations");
    passed &= Check(gridText.find("native_resolve_region_tracking = true") != std::string::npos &&
                        gridText.find("native_resolve_region_sampling = true") != std::string::npos,
                    "GUI FOV/output customization lost native-region sampling policy");
    passed &= Check(RuntimeAudioMasterVolumeFromPcConfig(destination) == 0.5,
                    "GUI volume must reach the existing audio runtime config reader");
    for (const std::string badVolume : {"-0.01", "1.01", "nan", "inf", "loud"}) {
      passed &= Check(Throws([&]() {
          InstallRuntimePcPresetWithOverrides(source, destination,
              {{"audio.master_volume", badVolume}}, true);
        }), "invalid GUI volume must fail before replacing the active profile");
      std::ifstream unchangedFile(destination, std::ios::binary);
      const std::string unchanged((std::istreambuf_iterator<char>(unchangedFile)),
                                  std::istreambuf_iterator<char>());
      passed &= Check(unchanged == gridText,
                      "failed volume customization changed installed settings");
    }
    for (const std::string endpoint : {"0", "1"}) {
      InstallRuntimePcPresetWithOverrides(source, destination,
          {{"audio.master_volume", endpoint}}, true);
      passed &= Check(RuntimeAudioMasterVolumeFromPcConfig(destination) == std::stod(endpoint),
                      "GUI volume endpoints must persist exactly");
    }
    InstallRuntimePcPresetWithOverrides(source, destination,
        {{"input.mouse_sensitivity", "2.5"}, {"input.mouse_invert_y", "true"},
         {"general.language", "french"}, {"audio.master_volume", "0.5"},
         {"input.keyboard_mouse_user_index", "2"},
         {"input.mouse_acceleration", "0.2"}, {"input.mouse_smoothing", "0.3"}}, true);
    std::ifstream inputConfigFile(destination, std::ios::binary);
    const std::string inputConfig((std::istreambuf_iterator<char>(inputConfigFile)),
                                  std::istreambuf_iterator<char>());
    inputConfigFile.close();
    passed &= Check(RuntimeXboxLanguageFromPcConfig(destination) == 4 &&
                        RuntimeAudioMasterVolumeFromPcConfig(destination) == 0.5 &&
                        inputConfig.find("mouse_sensitivity = 2.5") != std::string::npos &&
                        inputConfig.find("mouse_invert_y = true") != std::string::npos &&
                        inputConfig.find(gridRules) != std::string::npos,
                    "multi-category edits must persist together without losing graphics policy");
    for (const auto& bad : std::vector<RuntimeLaunchOptions::PcConfigOverride>{
             {"input.mouse_sensitivity", "0"}, {"input.mouse_sensitivity", "10.1"},
             {"input.mouse_sensitivity", "nan"}, {"input.mouse_invert_y", "yes"},
             {"input.keyboard_mouse_user_index", "-1"}, {"input.keyboard_mouse_user_index", "4"},
             {"input.keyboard_mouse_user_index", "1.5"},
             {"input.mouse_acceleration", "-0.1"}, {"input.mouse_acceleration", "1.01"},
             {"input.mouse_smoothing", "1.01"}, {"input.mouse_smoothing", "nan"},
             {"general.language", "unsupported"}}) {
        passed &= Check(Throws([&]() {
            InstallRuntimePcPresetWithOverrides(source, destination, {bad}, true);
        }), "invalid input/language edits must be rejected");
        std::ifstream checkFile(destination, std::ios::binary);
        const std::string check((std::istreambuf_iterator<char>(checkFile)),
                                std::istreambuf_iterator<char>());
        passed &= Check(check == inputConfig, "rejected category edit changed active config");
    }
    const char* editArgv[] = {"TheDarkness.exe", "--edit-config",
                             "--set-config=audio.master_volume=0.25", "--overwrite-config"};
    const auto editOptions = ParseRuntimeLaunchOptions(4, editArgv, install_root);
    passed &= Check(editOptions.action == RuntimeLaunchAction::InstallPreset &&
                        editOptions.pcConfigPath == editOptions.pcConfigInstallPath &&
                        editOptions.pcConfigPath == std::filesystem::absolute(destination).lexically_normal(),
                    "installed-config edit must stay on the adjacent active file");
    InstallRuntimePcPresetWithOverrides(editOptions.pcConfigPath,
        editOptions.pcConfigInstallPath, editOptions.pcConfigOverrides,
        editOptions.overwritePcConfig);
    std::ifstream editedFile(destination, std::ios::binary);
    const std::string edited((std::istreambuf_iterator<char>(editedFile)),
                             std::istreambuf_iterator<char>());
    editedFile.close();
    passed &= Check(RuntimeAudioMasterVolumeFromPcConfig(destination) == 0.25 &&
                        RuntimeXboxLanguageFromPcConfig(destination) == 4 &&
                        edited.find("mouse_sensitivity = 2.5") != std::string::npos &&
                        edited.find("mouse_invert_y = true") != std::string::npos &&
                        edited.find(gridRules) != std::string::npos,
                    "second edit must retain prior custom input/language and graphics policy");
    const auto persistedInput = toml::parse_file(destination.string());
    passed &= Check(persistedInput["input"]["keyboard_mouse_user_index"].value_or(-1) == 2 &&
                        persistedInput["input"]["mouse_acceleration"].value_or(-1.0) == 0.2 &&
                        persistedInput["input"]["mouse_smoothing"].value_or(-1.0) == 0.3,
                    "later volume edit must preserve mouse bridge slot and response settings");
    for (const auto& args : std::vector<std::vector<const char*>>{
             {"TheDarkness.exe", "--edit-config"},
             {"TheDarkness.exe", "--edit-config", "--set-config=audio.master_volume=0.5"},
             {"TheDarkness.exe", "--edit-config", "--overwrite-config"},
             {"TheDarkness.exe", "--edit-config", "--pc-config=other.toml"},
             {"TheDarkness.exe", "--pc-config=other.toml", "--edit-config"},
             {"TheDarkness.exe", "--edit-config", "--install-preset=original_720p"}}) {
        passed &= Check(Throws([&]() {
            ParseRuntimeLaunchOptions(static_cast<int>(args.size()), args.data(), install_root);
        }), "installed-config editing must require explicit scoped write intent");
    }
    passed &= Check(Throws([&]() {
        InstallRuntimePcPresetWithOverrides(destination, destination,
            {{"audio.master_volume", "9"}}, true);
    }), "invalid in-place edit must fail validation");
    std::ifstream rejectedEditFile(destination, std::ios::binary);
    const std::string rejectedEdit((std::istreambuf_iterator<char>(rejectedEditFile)),
                                   std::istreambuf_iterator<char>());
    rejectedEditFile.close();
    passed &= Check(rejectedEdit == edited, "invalid in-place edit changed current config");
    {
        // V315: a configuration saved before a key existed (no overlay_key)
        // can still be edited; the schema key is inserted, validated by kind.
        toml::table older = toml::parse_file(destination.string());
        if (auto* input = older["input"].as_table()) input->erase("overlay_key");
        {
            std::ofstream rewrite(destination, std::ios::binary | std::ios::trunc);
            rewrite << older;
        }
        InstallRuntimePcPresetWithOverrides(destination, destination,
            {{"input.overlay_key", "F5"}}, true);
        const auto upgraded = toml::parse_file(destination.string());
        passed &= Check(upgraded["input"]["overlay_key"].value_or(std::string{}) == "F5",
                        "editing an older config inserts a newer schema key");
        passed &= Check(Throws([&]() {
            InstallRuntimePcPresetWithOverrides(destination, destination,
                {{"input.overlay_key", "F12"}}, true);
        }), "a reserved overlay key is rejected");
    }
    passed &= Check(Throws([&]() {
                      const char* invalid_argv[] = {
                          "TheDarkness.exe", "--install-preset",
                          "original_720p", "--set-config",
                          "unknown.setting=true"};
                      ParseRuntimeLaunchOptions(5, invalid_argv, install_root);
                    }),
                    "customization must reject keys outside the shared schema");
    passed &= Check(Throws([&]() {
                      const char* duplicate_argv[] = {
                          "TheDarkness.exe", "--install-preset",
                          "original_720p", "--set-config=monitor=0",
                          "--set-config=monitor=1"};
                      ParseRuntimeLaunchOptions(5, duplicate_argv, install_root);
                    }),
                    "customization must reject duplicate setting keys");
    passed &= Check(Throws([&]() {
                      const char* detached_argv[] = {
                          "TheDarkness.exe", "--set-config=monitor=0"};
                      ParseRuntimeLaunchOptions(2, detached_argv, install_root);
                    }),
                    "setting overrides must be scoped to preset installation");

    const auto errorLines = RuntimePcConfigInstallErrorLines(
        source, destination, "first line\r\nsecond line");
    passed &= Check(std::find(errorLines.begin(), errorLines.end(),
                              "PC_CONFIG_INSTALL_ERROR=first line  second line") !=
                        errorLines.end(),
                    "preset installation errors must remain machine-readable");

    const char* overwrite_argv[] = {"TheDarkness.exe",
                                    "--overwrite-config",
                                    "--install-preset=original_720p"};
    const auto overwrite_options = ParseRuntimeLaunchOptions(
        3, overwrite_argv, install_root);
    passed &= Check(overwrite_options.action ==
                            RuntimeLaunchAction::InstallPreset &&
                        overwrite_options.overwritePcConfig,
                    "explicit reset must require the install action and overwrite flag");
    passed &= Check(Throws([&]() {
                      const char* invalid_argv[] = {
                          "TheDarkness.exe", "--overwrite-config"};
                      ParseRuntimeLaunchOptions(2, invalid_argv, install_root);
                    }),
                    "overwrite permission must be rejected outside preset installation");
    passed &= Check(std::string(RuntimeLaunchHelpText())
                            .find("--install-preset") != std::string::npos &&
                        std::string(RuntimeLaunchHelpText())
                                .find("--overwrite-config") != std::string::npos &&
                        std::string(RuntimeLaunchHelpText())
                                .find("--set-config") != std::string::npos,
                    "help must document persistent preset installation and reset");
    std::filesystem::remove_all(install_root, error);
  }

  {
    const char* argv[] = {"TheDarkness.exe", "--validate-mods",
                          "--mods-config", "settings/mods.toml"};
    const auto options = ParseRuntimeLaunchOptions(
        4, argv, executable_directory);
    passed &= Check(options.action == RuntimeLaunchAction::ValidateMods,
                    "--validate-mods must remain information-only");
    passed &= Check(options.modsConfigPath ==
                        std::filesystem::absolute("settings/mods.toml")
                            .lexically_normal(),
                    "mod validation must honor the selected manifest");
    passed &= Check(std::string(RuntimeLaunchHelpText())
                            .find("--validate-mods") != std::string::npos,
                    "help must describe mod-manifest validation");
  }

  {
    const auto config_root =
        std::filesystem::temp_directory_path() / "darkness pc config validation";
    std::error_code error;
    std::filesystem::create_directories(config_root, error);
    const auto valid_path = config_root / "valid.toml";
    std::ofstream(valid_path)
        << "pc_config_version = 1\n"
           "output_resolution = \"2560x1080\"\n"
           "resolution_scale = 3\n"
           "[display]\n"
           "present_mode = \"vrr\"\n"
           "max_frame_latency = 1\n"
           "frame_limit = 144\n"
           "[camera]\n"
           "field_of_view = 110.0\n"
           "[general]\n"
           "language = \"spanish\"\n"
           "[audio]\n"
           "master_volume = 0.25\n"
           "[input]\n"
           "mouse_sensitivity = 0.5\n"
           "mouse_acceleration = 0.25\n"
           "mouse_smoothing = 0.5\n"
           "controller_sensitivity = \"high\"\n"
           "controller_invert_y = true\n"
           "vibration_scale = 0.5\n"
           "[input.bind]\n"
           "a = \"Key%One\"\n"
           "[input.controller_bind]\n"
           "a = \"b\"\n"
           "b = \"a\"\n"
           "guide = \"none\"\n";
    const auto valid = ValidateRuntimePcConfig(valid_path, true);
    passed &= Check(valid.exists && valid.valid && valid.errors.empty() &&
                        valid.warnings.empty(),
                    "valid schema-1 PC settings must pass preflight");
    const auto grid_path = config_root / "native_grid.toml";
    std::ofstream(grid_path)
        << "pc_config_version=1\n"
           "draw_resolution_scale_native_grid_rules=\"B29F0BF45937C4C4:FDC5E32EC6045BE1:1:324:18:6\"\n";
    const auto grid = ValidateRuntimePcConfig(grid_path, true);
    passed &= Check(grid.valid && grid.warnings.empty(),
                    "explicit native data-grid annotations must pass preflight");
    std::ofstream(grid_path)
        << "pc_config_version=1\n"
           "draw_resolution_scale_native_grid_rules=\"B29F0BF45937C4C4:9F1D2D64E5F75924:0:1280:720:26:filter\"\n";
    const auto filter_grid = ValidateRuntimePcConfig(grid_path, true);
    passed &= Check(filter_grid.valid && filter_grid.warnings.empty(),
                    "explicit native image-filter annotations must pass preflight");
    const auto region_path = config_root / "native_regions.toml";
    std::ofstream(region_path) << "pc_config_version=1\n"
        "native_resolve_region_tracking=true\nnative_resolve_region_sampling=true\n";
    const auto region = ValidateRuntimePcConfig(region_path, true);
    passed &= Check(region.valid && region.warnings.empty(),
                    "native-region profile settings must be recognized without warnings");
    std::ofstream(region_path) << "pc_config_version=1\nnative_resolve_region_sampling=true\n";
    passed &= Check(!ValidateRuntimePcConfig(region_path, true).valid,
                    "sampling cannot be enabled without tracking in the profile");
    std::ofstream(region_path) << "pc_config_version=1\n"
        "native_resolve_region_tracking=\"true\"\nnative_resolve_region_sampling=true\n";
    passed &= Check(!ValidateRuntimePcConfig(region_path, true).valid,
                    "native-region booleans must not accept strings");
    std::ofstream(region_path) << "pc_config_version=1\n"
        "native_resolve_region_tracking=true\nnative_resolve_region_sampling=false\n";
    passed &= Check(ValidateRuntimePcConfig(region_path, true).valid,
                    "tracking-only diagnostics remain valid");
    std::ofstream(grid_path)
        << "pc_config_version=1\n"
           "draw_resolution_scale_native_grid_rules=\"B29F0BF45937C4C4:FDC5E32EC6045BE1:99:324:18:6\"\n";
    passed &= Check(!ValidateRuntimePcConfig(grid_path, true).valid,
                    "malformed data-grid annotations must fail before guest startup");
    passed &= Check(RuntimeInputVibrationScaleFromPcConfig(valid_path) == 0.5 &&
                        RuntimeInputVibrationScaleFromPcConfig(
                            config_root / "absent.toml") == 1.0,
                    "runtime vibration setting must preserve explicit and absent defaults");
    const RuntimeInputButtonMap configuredMap =
        RuntimeInputButtonMapFromPcConfig(valid_path);
    const RuntimeInputButtonMap absentMap = RuntimeInputButtonMapFromPcConfig(
        config_root / "absent.toml");
    passed &= Check(
        configuredMap.sourceForGuest[static_cast<size_t>(RuntimeInputButton::A)] ==
                static_cast<uint8_t>(RuntimeInputButton::B) &&
            configuredMap.sourceForGuest[static_cast<size_t>(RuntimeInputButton::B)] ==
                static_cast<uint8_t>(RuntimeInputButton::A) &&
            configuredMap.sourceForGuest[static_cast<size_t>(RuntimeInputButton::Guide)] ==
                static_cast<uint8_t>(RuntimeInputButton::None) &&
            EncodeRuntimeInputButtonMap(absentMap) ==
                kRuntimeInputIdentityButtonMap,
        "controller button map must parse swap/none and preserve absent identity");
    const RuntimeControllerProfile configuredControllerProfile =
        RuntimeControllerProfileFromPcConfig(valid_path);
    const RuntimeControllerProfile absentControllerProfile =
        RuntimeControllerProfileFromPcConfig(config_root / "absent.toml");
    passed &= Check(configuredControllerProfile.sensitivity == 2 &&
                        configuredControllerProfile.invertY &&
                        absentControllerProfile.sensitivity == 0 &&
                        !absentControllerProfile.invertY,
                    "controller profile must parse title-native values and preserve absent defaults");
    passed &= Check(RuntimeAudioMasterVolumeFromPcConfig(valid_path) == 0.25 &&
                        RuntimeAudioMasterVolumeFromPcConfig(
                            config_root / "absent.toml") == 1.0,
                     "runtime master volume must preserve explicit and absent identity values");
    passed &= Check(RuntimeXboxLanguageFromPcConfig(valid_path) == 5 &&
                        RuntimeXboxLanguageFromPcConfig(
                            config_root / "absent.toml") == 1,
                    "runtime language must map Spanish and preserve absent English default");
    // V504 (#16): graphics.msaa_mode, absent = 4x; invalid values fail early.
    passed &= Check(RuntimeMsaaModeFromPcConfig(config_root / "absent.toml") ==
                        msaa_mode::Policy::kAlways4x,
                    "absent MSAA mode keeps 4x");
    for (const auto& [name, expected] :
         std::initializer_list<std::pair<const char*, msaa_mode::Policy>>{
             {"4x", msaa_mode::Policy::kAlways4x}, {"2x", msaa_mode::Policy::kAlways2x},
             {"auto", msaa_mode::Policy::kAutomatic}}) {
      const auto msaa_path = config_root / (std::string("msaa_") + name + ".toml");
      std::ofstream(msaa_path) << "pc_config_version = 1\n[graphics]\nmsaa_mode = \"" << name
                               << "\"\n";
      passed &= Check(ValidateRuntimePcConfig(msaa_path, true).valid &&
                          RuntimeMsaaModeFromPcConfig(msaa_path) == expected,
                      (std::string("MSAA mode failed for ") + name).c_str());
    }
    {
      const auto msaa_path = config_root / "msaa_bad.toml";
      std::ofstream(msaa_path) << "pc_config_version = 1\n[graphics]\nmsaa_mode = \"8x\"\n";
      bool threw = false;
      try {
        RuntimeMsaaModeFromPcConfig(msaa_path);
      } catch (const std::exception&) {
        threw = true;
      }
      passed &= Check(!ValidateRuntimePcConfig(msaa_path, true).valid && threw,
                      "an unsupported MSAA mode is rejected");
    }
    for (const auto& [name, expected] :
         std::initializer_list<std::pair<const char*, uint32_t>>{
             {"english", 1}, {"german", 3}, {"french", 4},
             {"spanish", 5}, {"italian", 6}}) {
      const auto language_path = config_root / (std::string(name) + ".toml");
      std::ofstream(language_path)
          << "pc_config_version = 1\n[general]\nlanguage = \"" << name
          << "\"\n";
      passed &= Check(ValidateRuntimePcConfig(language_path, true).valid &&
                          RuntimeXboxLanguageFromPcConfig(language_path) == expected,
                      (std::string("language mapping failed for ") + name).c_str());
    }
    const auto lines = RuntimePcConfigValidationLines(valid_path, valid);
    passed &= Check(std::find(lines.begin(), lines.end(),
                              "PC_CONFIG_VALID=1") != lines.end(),
                    "validation output must be machine readable");

    const auto maximum_scale_path = config_root / "maximum_scale.toml";
    std::ofstream(maximum_scale_path)
        << "pc_config_version = 1\nresolution_scale = 7\n";
    const auto maximum_scale =
        ValidateRuntimePcConfig(maximum_scale_path, true);
    passed &= Check(maximum_scale.valid && maximum_scale.errors.empty(),
                    "the renderer's exact 7x internal-scale limit must pass");

    const auto clamped_scale_path = config_root / "clamped_scale.toml";
    std::ofstream(clamped_scale_path)
        << "pc_config_version = 1\nresolution_scale = 8\n";
    const auto clamped_scale =
        ValidateRuntimePcConfig(clamped_scale_path, true);
    passed &= Check(!clamped_scale.valid && !clamped_scale.errors.empty(),
                    "an 8x scale that ReXGlue would clamp must fail preflight");

    const auto invalid_vibration_path = config_root / "invalid_vibration.toml";
    std::ofstream(invalid_vibration_path)
        << "pc_config_version = 1\n[input]\nvibration_scale = 1.01\n";
    passed &= Check(!ValidateRuntimePcConfig(invalid_vibration_path, true).valid,
                    "vibration output scale above identity must fail preflight");

    const auto invalid_mouse_acceleration_path =
        config_root / "invalid_mouse_acceleration.toml";
    std::ofstream(invalid_mouse_acceleration_path)
        << "pc_config_version = 1\n[input]\nmouse_acceleration = 1.01\n";
    passed &= Check(
        !ValidateRuntimePcConfig(invalid_mouse_acceleration_path, true).valid,
        "mouse acceleration above the bounded curve must fail preflight");

    const auto invalid_mouse_smoothing_path =
        config_root / "invalid_mouse_smoothing.toml";
    std::ofstream(invalid_mouse_smoothing_path)
        << "pc_config_version = 1\n[input]\nmouse_smoothing = -0.01\n";
    passed &= Check(
        !ValidateRuntimePcConfig(invalid_mouse_smoothing_path, true).valid,
        "negative mouse smoothing must fail preflight");

    const auto invalid_controller_bind_path =
        config_root / "invalid_controller_bind.toml";
    std::ofstream(invalid_controller_bind_path)
        << "pc_config_version = 1\n[input.controller_bind]\na = \"turbo\"\n";
    passed &= Check(
        !ValidateRuntimePcConfig(invalid_controller_bind_path, true).valid &&
            Throws([&]() {
              RuntimeInputButtonMapFromPcConfig(invalid_controller_bind_path);
            }),
        "unsupported physical controller button names must fail validation and parsing");

    const auto invalid_controller_sensitivity_path =
        config_root / "invalid_controller_sensitivity.toml";
    std::ofstream(invalid_controller_sensitivity_path)
        << "pc_config_version = 1\n[input]\ncontroller_sensitivity = \"extreme\"\n";
    passed &= Check(
        !ValidateRuntimePcConfig(invalid_controller_sensitivity_path, true).valid &&
            Throws([&]() {
              RuntimeControllerProfileFromPcConfig(
                  invalid_controller_sensitivity_path);
            }),
        "unsupported controller sensitivity profile names must fail validation and parsing");

    const auto invalid_controller_invert_path =
        config_root / "invalid_controller_invert.toml";
    std::ofstream(invalid_controller_invert_path)
        << "pc_config_version = 1\n[input]\ncontroller_invert_y = 1\n";
    passed &= Check(
        !ValidateRuntimePcConfig(invalid_controller_invert_path, true).valid &&
            Throws([&]() {
              RuntimeControllerProfileFromPcConfig(invalid_controller_invert_path);
            }),
        "non-boolean controller inversion must fail validation and parsing");

    const auto invalid_volume_path = config_root / "invalid_volume.toml";
    std::ofstream(invalid_volume_path)
        << "pc_config_version = 1\n[audio]\nmaster_volume = -0.01\n";
    passed &= Check(!ValidateRuntimePcConfig(invalid_volume_path, true).valid,
                    "negative host master volume must fail preflight");

    const auto invalid_language_path = config_root / "invalid_language.toml";
    std::ofstream(invalid_language_path)
        << "pc_config_version = 1\n[general]\nlanguage = \"japanese\"\n";
    passed &= Check(!ValidateRuntimePcConfig(invalid_language_path, true).valid &&
                        Throws([&]() {
                          RuntimeXboxLanguageFromPcConfig(invalid_language_path);
                        }),
                    "language without extracted title content must fail");
    const auto typed_language_path = config_root / "typed_language.toml";
    std::ofstream(typed_language_path)
        << "pc_config_version = 1\n[general]\nlanguage = 5\n";
    passed &= Check(!ValidateRuntimePcConfig(typed_language_path, true).valid &&
                        Throws([&]() {
                          RuntimeXboxLanguageFromPcConfig(typed_language_path);
                        }),
                    "numeric language must not bypass the named schema");

    const auto advanced_path = config_root / "advanced.toml";
    std::ofstream(advanced_path)
        << "pc_config_version = 1\nstore_shaders = true\n";
    const auto advanced = ValidateRuntimePcConfig(advanced_path, true);
    passed &= Check(advanced.valid && advanced.warnings.size() == 1,
                    "unknown advanced ReXGlue settings must warn, not fail");

    const auto invalid_path = config_root / "invalid.toml";
    std::ofstream(invalid_path)
        << "pc_config_version = 2\n"
           "output_resolution = \"9000x720\"\n"
           "window_mode = \"exclusive\"\n"
           "resolution_scale = 9\n"
           "[display]\n"
           "present_mode = \"mailbox\"\n"
           "max_frame_latency = 0\n"
           "[camera]\n"
           "field_of_view = 150.0\n";
    const auto invalid = ValidateRuntimePcConfig(invalid_path, true);
    passed &= Check(!invalid.valid && invalid.errors.size() >= 7,
                    "invalid declared settings must fail all applicable ranges");

    const auto malformed_path = config_root / "malformed.toml";
    std::ofstream(malformed_path) << "pc_config_version = [\n";
    const auto malformed = ValidateRuntimePcConfig(malformed_path, true);
    passed &= Check(!malformed.valid && !malformed.errors.empty(),
                    "malformed TOML must fail before guest startup");

    const auto missing_path = config_root / "missing.toml";
    passed &= Check(ValidateRuntimePcConfig(missing_path, false).valid,
                    "an absent optional adjacent config must retain defaults");
    passed &= Check(!ValidateRuntimePcConfig(missing_path, true).valid,
                    "an explicitly selected missing config must fail");

    const char* validate_argv[] = {"TheDarkness.exe", "--validate-config",
                                   "--pc-config", "valid.toml"};
    const auto validate_options = ParseRuntimeLaunchOptions(
        4, validate_argv, config_root);
    passed &= Check(validate_options.action ==
                            RuntimeLaunchAction::ValidateConfig &&
                        validate_options.pcConfigExplicit,
                    "--validate-config must select a pre-guest explicit preflight");
    passed &= Check(std::string(RuntimeLaunchHelpText())
                            .find("--validate-config") != std::string::npos,
                    "help must describe configuration validation");

    const char* inspect_argv[] = {"TheDarkness.exe", "--inspect-config",
                                  "--pc-config", "valid.toml"};
    const auto inspect_options = ParseRuntimeLaunchOptions(
        4, inspect_argv, config_root);
    passed &= Check(inspect_options.action ==
                            RuntimeLaunchAction::InspectConfig &&
                        inspect_options.pcConfigExplicit,
                    "--inspect-config must select a read-only explicit report");
    const auto inspection = RuntimePcConfigInspectionLines(valid_path, valid);
    const auto has_inspection_line = [&](const std::string& expected) {
      return std::find(inspection.begin(), inspection.end(), expected) !=
             inspection.end();
    };
    passed &= Check(has_inspection_line("PC_CONFIG_INSPECTION_SCHEMA=1") &&
                        has_inspection_line("PC_CONFIG_APPLICATION=startup_only") &&
                        has_inspection_line("PC_CONFIG_RESTART_REQUIRED=1") &&
                        has_inspection_line("PC_CONFIG_VALUE_ENCODING=percent_utf8"),
                    "inspection must publish a stable startup/restart contract");
    const auto camera_key = std::find_if(
        inspection.begin(), inspection.end(), [](const std::string& line) {
          return line.find("_KEY=camera.field_of_view") != std::string::npos;
        });
    bool camera_typed = false;
    if (camera_key != inspection.end()) {
      const std::string prefix =
          camera_key->substr(0, camera_key->find("KEY="));
      camera_typed = has_inspection_line(prefix + "TYPE=float") &&
                     has_inspection_line(prefix + "VALUE=110") &&
                     has_inspection_line(prefix + "RESTART_REQUIRED=1");
    }
    passed &= Check(has_inspection_line("PC_CONFIG_ENTRY_0_KEY=audio.master_volume") &&
                        camera_typed,
                    "inspection must sort, type, and mark restart-required settings");
    const auto binding_key = std::find_if(
        inspection.begin(), inspection.end(), [](const std::string& line) {
          return line.find("_KEY=input.bind.a") != std::string::npos;
        });
    bool binding_encoded = false;
    if (binding_key != inspection.end()) {
      const std::string prefix =
          binding_key->substr(0, binding_key->find("KEY="));
      binding_encoded = has_inspection_line(prefix + "TYPE=string") &&
                        has_inspection_line(prefix + "VALUE=Key%25One");
    }
    passed &= Check(binding_encoded,
                    "inspection must percent-encode string values unambiguously");
    passed &= Check(std::string(RuntimeLaunchHelpText())
                            .find("--inspect-config") != std::string::npos,
                    "help must describe configuration inspection");
    std::filesystem::remove_all(config_root, error);
  }

  {
    const char* argv[] = {"TheDarkness.exe", "--pc-config",
                          "settings/custom.toml", "--mods-config",
                          "settings/mods.toml", "--user-data-root",
                          "userdata/proton", "game/default.xex"};
    const auto options = ParseRuntimeLaunchOptions(
        8, argv, executable_directory);
    passed &= Check(options.pcConfigPath ==
                        std::filesystem::absolute("settings/custom.toml")
                            .lexically_normal(),
                    "separate --pc-config path must be honored");
    passed &= Check(options.pcConfigExplicit,
                    "explicit PC configuration selection must be tracked");
    passed &= Check(options.xexPath ==
                        std::filesystem::absolute("game/default.xex")
                            .lexically_normal(),
                    "the positional XEX path must be preserved");
    passed &= Check(options.modsConfigPath ==
                        std::filesystem::absolute("settings/mods.toml")
                            .lexically_normal(),
                    "separate --mods-config path must be honored");
    passed &= Check(options.userDataRoot ==
                        std::filesystem::absolute("userdata/proton")
                            .lexically_normal(),
                    "separate --user-data-root path must be honored");
  }

  {
    const char* argv[] = {"TheDarkness.exe",
                          "--mods-config=settings/inline-mods.toml"};
    const auto options = ParseRuntimeLaunchOptions(
        2, argv, executable_directory);
    passed &= Check(options.modsConfigPath ==
                        std::filesystem::absolute(
                            "settings/inline-mods.toml")
                            .lexically_normal(),
                    "inline --mods-config path must be honored");
  }

  {
    const char* argv[] = {"TheDarkness.exe",
                          "--pc-config=settings/inline.toml"};
    const auto options = ParseRuntimeLaunchOptions(
        2, argv, executable_directory);
    passed &= Check(options.pcConfigPath ==
                        std::filesystem::absolute("settings/inline.toml")
                            .lexically_normal(),
                    "inline --pc-config path must be honored");
  }

  {
    const char* argv[] = {"TheDarkness.exe",
                          "--user-data-root=userdata/inline"};
    const auto options = ParseRuntimeLaunchOptions(
        2, argv, executable_directory);
    passed &= Check(options.userDataRoot ==
                        std::filesystem::absolute("userdata/inline")
                            .lexically_normal(),
                    "inline --user-data-root path must be honored");
  }

  passed &= Check(
      Throws([&]() {
        const char* argv[] = {"TheDarkness.exe", "--help", "--print-paths"};
        ParseRuntimeLaunchOptions(3, argv, executable_directory);
      }),
      "multiple information actions must fail clearly");
  passed &= Check(
      Throws([&]() {
        const char* argv[] = {"TheDarkness.exe", "--print-capabilities",
                              "--list-presets"};
        ParseRuntimeLaunchOptions(3, argv, executable_directory);
      }),
      "capability reporting must be exclusive with other information actions");
  passed &= Check(
      Throws([&]() {
        const char* argv[] = {"TheDarkness.exe", "--unknown"};
        ParseRuntimeLaunchOptions(2, argv, executable_directory);
      }),
      "unknown options must fail clearly");
  passed &= Check(
      Throws([&]() {
        const char* argv[] = {"TheDarkness.exe", "--pc-config"};
        ParseRuntimeLaunchOptions(2, argv, executable_directory);
      }),
      "missing config values must fail clearly");
  passed &= Check(
      Throws([&]() {
        const char* argv[] = {"TheDarkness.exe", "--mods-config"};
        ParseRuntimeLaunchOptions(2, argv, executable_directory);
      }),
      "missing mod config values must fail clearly");
  passed &= Check(
      Throws([&]() {
        const char* argv[] = {"TheDarkness.exe", "--user-data-root"};
        ParseRuntimeLaunchOptions(2, argv, executable_directory);
      }),
      "missing user-data root values must fail clearly");
  passed &= Check(
      Throws([&]() {
        const char* argv[] = {"TheDarkness.exe", "first.xex",
                              "second.xex"};
        ParseRuntimeLaunchOptions(3, argv, executable_directory);
      }),
      "multiple title paths must fail clearly");

  // V320: internal scaling gets the title's data-pass annotations whenever a
  // configuration raises resolution_scale without them; explicit values win.
  {
    const std::string scale1 = "pc_config_version = 1\nresolution_scale = 1\n";
    bool changed = true;
    passed &= Check(RuntimePcConfigWithTitleScaleRequirements(scale1, "scale1", &changed) ==
                            scale1 &&
                        !changed,
                    "scale 1 configuration must reach the GPU unchanged");
    const std::string bare2 =
        "pc_config_version = 1\nresolution_scale = 2\n[display]\nframe_rate = \"refresh\"\n";
    const std::string derived =
        RuntimePcConfigWithTitleScaleRequirements(bare2, "bare2", &changed);
    const toml::table derived_table = toml::parse(derived);
    passed &= Check(changed &&
                        derived_table["draw_resolution_scale_threshold"].value_or(0) ==
                            kTitleScaleThreshold &&
                        derived_table["draw_resolution_scale_native_grid_rules"].value_or(
                            std::string{}) == kTitleNativeGridRules &&
                        derived_table["native_resolve_region_tracking"].value_or(false) &&
                        derived_table["display"]["frame_rate"].value_or(std::string{}) ==
                            "refresh",
                    "internal 2x without annotations must receive the title's validated ones");
    const std::string explicit2 =
        "pc_config_version = 1\nresolution_scale = 2\ndraw_resolution_scale_threshold = 0\n";
    const toml::table explicit_table = toml::parse(
        RuntimePcConfigWithTitleScaleRequirements(explicit2, "explicit2", &changed));
    passed &= Check(changed &&
                        explicit_table["draw_resolution_scale_threshold"].value_or(-1) == 0 &&
                        explicit_table["draw_resolution_scale_native_grid_rules"].value_or(
                            std::string{}) == kTitleNativeGridRules,
                    "an explicit threshold must win while missing rules are added");
    const std::string complete2 = explicit2 +
                                  "draw_resolution_scale_native_grid_rules = \"" +
                                  std::string(kTitleNativeGridRules) + "\"\n" +
                                  "native_resolve_region_tracking = false\n";
    passed &= Check(RuntimePcConfigWithTitleScaleRequirements(complete2, "complete2",
                                                              &changed) == complete2 &&
                        !changed,
                    "a configuration with all annotations (explicit values) must reach the GPU unchanged");
    const std::string legacy2 = std::string("pc_config_version = 1\nresolution_scale = 2\n") +
                                "draw_resolution_scale_threshold = 640\n" +
                                "draw_resolution_scale_native_grid_rules = \"" +
                                std::string(kLegacyTitleNativeGridRules) + "\"\n";
    const toml::table legacy_table = toml::parse(
        RuntimePcConfigWithTitleScaleRequirements(legacy2, "legacy2", &changed));
    passed &= Check(changed &&
                        legacy_table["draw_resolution_scale_native_grid_rules"].value_or(
                            std::string{}) == kTitleNativeGridRules &&
                        legacy_table["native_resolve_region_tracking"].value_or(false),
                    "the legacy lookup-table-only rules must be upgraded with region tracking");
    const std::string previous2 = std::string("pc_config_version = 1\nresolution_scale = 2\n") +
                                  "draw_resolution_scale_threshold = 640\n" +
                                  "draw_resolution_scale_native_grid_rules = \"" +
                                  std::string(kPreviousTitleNativeGridRules) + "\"\n" +
                                  "native_resolve_region_tracking = true\n";
    const toml::table previous_table = toml::parse(
        RuntimePcConfigWithTitleScaleRequirements(previous2, "previous2", &changed));
    passed &= Check(changed &&
                        previous_table["draw_resolution_scale_native_grid_rules"].value_or(
                            std::string{}) == kTitleNativeGridRules &&
                        std::string(kTitleNativeGridRules).find(
                            "B29F0BF45937C4C4:69779AD07425E356:0:324:18:26") != std::string::npos,
                    "the rules before the pause grading table (#10) must be upgraded");
    // #16: the published 0.9.1 rules lacked the motion-blur composite.
    const std::string published091 = std::string("pc_config_version = 1\nresolution_scale = 2\n") +
                                      "draw_resolution_scale_threshold = 640\n" +
                                      "draw_resolution_scale_native_grid_rules = \"" +
                                      std::string(k091TitleNativeGridRules) + "\"\n" +
                                      "native_resolve_region_tracking = true\n";
    const toml::table published_table = toml::parse(
        RuntimePcConfigWithTitleScaleRequirements(published091, "published091", &changed));
    passed &= Check(changed &&
                        published_table["draw_resolution_scale_native_grid_rules"].value_or(
                            std::string{}) == kTitleNativeGridRules,
                    "the published 0.9.1 rules must be upgraded (#16)");
    passed &= Check(std::string(kTitleNativeGridRules)
                            .find("4FA9486610B42A92:A59B41D0BD79484B:1:1280:720:26:filter_scaled:source=native") !=
                        std::string::npos &&
                        std::string(kTitleNativeGridRules)
                                .find("4FA9486610B42A92:22FC55CE134777AC:1:1280:720:26:filter_scaled:source=native") !=
                            std::string::npos,
                    "both final composites (motion blur on and off) enlarge the native glow (#16)");
    // #16: 0.9.4 rendered the bloom passes at the internal scale
    // (:filter_scaled); 0.9.5 returns to the 0.9.2-0.9.3.1 rules.
    const std::string published094 = std::string("pc_config_version = 1\nresolution_scale = 2\n") +
                                      "draw_resolution_scale_threshold = 640\n" +
                                      "draw_resolution_scale_native_grid_rules = \"" +
                                      std::string(k094TitleNativeGridRules) + "\"\n" +
                                      "native_resolve_region_tracking = true\n";
    const toml::table published094_table = toml::parse(
        RuntimePcConfigWithTitleScaleRequirements(published094, "published094", &changed));
    passed &= Check(changed &&
                        published094_table["draw_resolution_scale_native_grid_rules"].value_or(
                            std::string{}) == kTitleNativeGridRules,
                    "the 0.9.4 rules must be upgraded (#16)");
    {
        // The bloom passes (FAE3, 9F1D, 8F96) render on the native grid
        // (:filter); only the two glow consumers keep a scaled output and
        // reconstruct tracked native content (:filter_scaled:source=native).
        const std::string current(kTitleNativeGridRules);
        size_t filters = 0;
        size_t native = 0;
        for (size_t at = 0; (at = current.find(":filter", at)) != std::string::npos; ++at) {
            ++filters;
            if (current.compare(at, 14, ":filter_scaled") != 0) ++native;
        }
        passed &= Check(filters == 8 && native == 5,
                        "the five bloom passes render on the native grid (#16)");
        passed &= Check(current.rfind(std::string(kLegacyTitleNativeGridRules), 0) == 0 &&
                            current.find("B29F0BF45937C4C4:69779AD07425E356:0:324:18:26;") != std::string::npos,
                        "the lookup tables stay on the native grid");
        passed &= Check(current.rfind(std::string(k091TitleNativeGridRules), 0) == 0,
                        "the rules extend the 0.9.1 rules");
        passed &= Check(current != k094TitleNativeGridRules &&
                            std::string(k094TitleNativeGridRules).find(":filter;") == std::string::npos,
                        "0.9.4 rendered every bloom pass at the internal scale");
    }
    {
        // Every packaged preset and the example configuration carry exactly
        // the current rules wherever they set native-grid rules.
        for (const char* name : {"config/pc_presets/enhanced.toml", "config/pc_presets/original.toml",
                                 "config/pc_presets/performance.toml",
                                 "config/pc_presets/validation_1440p_internal_2x.toml",
                                 "config/pc_presets/validation_4k_windowed_internal_2x.toml",
                                 "config/pc_settings_v1.toml"}) {
            std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) + "/" + name);
            const std::string text((std::istreambuf_iterator<char>(file)), {});
            const bool hasRules = text.find("draw_resolution_scale_native_grid_rules") != std::string::npos;
            passed &= Check(!hasRules || text.find(std::string(kTitleNativeGridRules)) != std::string::npos,
                            (std::string(name) + " carries the current native-grid rules (#16)").c_str());
        }
    }
    const std::string base_config = "pc_config_version = 1\n[input]\n";
    const std::string with_program_cache = RuntimePcConfigWithProgramCacheSource(
        base_config,
        std::filesystem::path(u8"C:\\Games\\Dark \"ness\"\\System\\Xenon\\ProgramCache.xpc"));
    const toml::table program_cache_table = toml::parse(with_program_cache);
    passed &= Check(program_cache_table["gpu_program_cache_source"].value_or(std::string{}) ==
                            "C:\\Games\\Dark \"ness\"\\System\\Xenon\\ProgramCache.xpc" &&
                        program_cache_table["pc_config_version"].value_or(int64_t(0)) == 1 &&
                        program_cache_table["input"].as_table() != nullptr,
                    "the program cache source must be a top-level escaped TOML string");
    passed &= Check(RuntimePcConfigWithProgramCacheSource(base_config, {}) == base_config,
                    "no program cache must leave the configuration unchanged");
    const std::string custom2 = std::string("pc_config_version = 1\nresolution_scale = 2\n") +
                                "draw_resolution_scale_native_grid_rules = \"A:B:0:16:16:6\"\n";
    const toml::table custom_table = toml::parse(
        RuntimePcConfigWithTitleScaleRequirements(custom2, "custom2", &changed));
    passed &= Check(custom_table["draw_resolution_scale_native_grid_rules"].value_or(
                        std::string{}) == "A:B:0:16:16:6",
                    "custom rules must not be replaced");
    const auto derived_path = executable_directory / "title_scale_requirements.toml";
    std::filesystem::create_directories(executable_directory);
    std::ofstream(derived_path, std::ios::binary) << derived;
    passed &= Check(ValidateRuntimePcConfig(derived_path, true).valid,
                    "the derived configuration must pass the runtime validator");

    // V330 Automatic internal scale: the GPU receives the resolved scale,
    // then the annotations for it; other scales pass through.
    const std::string automatic =
        "pc_config_version = 1\nresolution_scale = 0\noutput_resolution = \"4k\"\n";
    const RuntimePcScaleTarget scaleTarget = RuntimePcScaleTargetFromContents(automatic, "automatic");
    passed &= Check(scaleTarget.resolutionScale == 0 && scaleTarget.outputResolution == "4k" &&
                        scaleTarget.monitor == 0,
                    "the automatic scale target must be read from the configuration");
    const std::string resolved =
        RuntimePcConfigWithAutomaticScale(automatic, "automatic", 3, &changed);
    passed &= Check(changed && toml::parse(resolved)["resolution_scale"].value_or(0) == 3,
                    "automatic scale must reach the GPU as the resolved scale");
    passed &= Check(RuntimePcConfigWithAutomaticScale(bare2, "bare2", 3, &changed) == bare2 &&
                        !changed,
                    "an explicit scale must reach the GPU unchanged");
    // V440: temporal AA is shelved; a saved choice reaches the GPU as off
    // unless DARKNESS_EXPERIMENTAL offers it.
    {
        const std::string taa =
            "pc_config_version = 1\nswap_post_effect = \"smaa\"\n[graphics]\ntemporal_aa = \"dlss\"\n";
        std::string shelved;
        const std::string gated = RuntimePcConfigWithShelvedFeatures(taa, "taa", false, &shelved);
        const toml::table gated_table = toml::parse(gated);
        passed &= Check(shelved == "dlss" &&
                            gated_table["graphics"]["temporal_aa"].value_or(std::string()) == "off" &&
                            gated_table["swap_post_effect"].value_or(std::string()) == "smaa",
                        "a shelved temporal AA choice must reach the GPU as off, other values kept");
        passed &= Check(RuntimePcConfigWithShelvedFeatures(taa, "taa", true, &shelved) == taa &&
                            shelved.empty(),
                        "the experimental switch must pass temporal AA through unchanged");
        const std::string off = "pc_config_version = 1\n[graphics]\ntemporal_aa = \"off\"\n";
        passed &= Check(RuntimePcConfigWithShelvedFeatures(off, "off", false, &shelved) == off &&
                            RuntimePcConfigWithShelvedFeatures(bare2, "bare2", false, &shelved) ==
                                bare2 &&
                            RuntimePcConfigWithShelvedFeatures({}, "none", false, &shelved).empty() &&
                            shelved.empty(),
                        "configurations without temporal AA must reach the GPU unchanged");
    }
    const auto automatic_path = executable_directory / "automatic_scale.toml";
    std::ofstream(automatic_path, std::ios::binary) << automatic;
    passed &= Check(ValidateRuntimePcConfig(automatic_path, true).valid,
                    "resolution_scale = 0 (Automatic) must pass the runtime validator");
    const auto rate_path = executable_directory / "numeric_frame_rate.toml";
    std::ofstream(rate_path, std::ios::binary)
        << "pc_config_version = 1\n[display]\nframe_rate = \"120\"\nmenu_frame_rate = \"full\"\n";
    passed &= Check(ValidateRuntimePcConfig(rate_path, true).valid &&
                        RuntimeFrameRateUsesHostPacingFromPcConfig(rate_path),
                    "a whole-number frame rate must validate and use host pacing");
    std::ofstream(rate_path, std::ios::binary)
        << "pc_config_version = 1\n[display]\nframe_rate = \"12\"\n";
    passed &= Check(!ValidateRuntimePcConfig(rate_path, true).valid,
                    "frame-rate caps below 20 must be rejected");
    // The pure rule behind Automatic.
    // Measured V330: 3x costs twice 2x (12.7 vs 6.4 ms of GPU per frame at the
    // heaviest street view on a 16 GB card).
    passed &= Check(RuntimeResolveAutomaticScale(2560, 1440, 16ull << 30).scale == 2 &&
                        RuntimeResolveAutomaticScale(1920, 1080, 8ull << 30).scale == 2 &&
                        RuntimeResolveAutomaticScale(3840, 2160, 15995ull << 20).scale == 3 &&
                        RuntimeResolveAutomaticScale(3840, 2160, 12ull << 30).scale == 2 &&
                        RuntimeResolveAutomaticScale(3840, 2160, 0).scale == 2 &&
                        RuntimeResolveAutomaticScale(1280, 800, 16ull << 30).scale == 1 &&
                        RuntimeResolveAutomaticScale(2560, 1440, 4ull << 30).scale == 1 &&
                        RuntimeResolveAutomaticScale(1920, 1080, 6ull << 30).scale == 2 &&
                        RuntimeResolveAutomaticScale(0, 0, 0).scale == 2,
                    "Automatic must pick 2x, 3x for 4K only with 16 GB, 1x on small screens or "
                    "cards below 6 GB");

    // general.language = "auto": the console-style dashboard language rule.
    passed &= Check(RuntimeXboxLanguageForWindowsLanguage(0x0407) == 3 &&  // de-DE
                        RuntimeXboxLanguageForWindowsLanguage(0x0C07) == 3 &&  // de-AT
                        RuntimeXboxLanguageForWindowsLanguage(0x040C) == 4 &&  // fr-FR
                        RuntimeXboxLanguageForWindowsLanguage(0x0C0A) == 5 &&  // es-ES
                        RuntimeXboxLanguageForWindowsLanguage(0x0410) == 6 &&  // it-IT
                        RuntimeXboxLanguageForWindowsLanguage(0x0409) == 1 &&  // en-US
                        RuntimeXboxLanguageForWindowsLanguage(0x0401) == 1 &&  // ar-SA
                        RuntimeXboxLanguageForWindowsLanguage(0x0411) == 1,    // ja-JP
                    "automatic language must map shipped languages and fall back to English");
    const auto auto_language = executable_directory / "auto_language.toml";
    std::ofstream(auto_language, std::ios::binary | std::ios::trunc)
        << "pc_config_version = 1\n[general]\nlanguage = \"auto\"\n";
    passed &= Check(ValidateRuntimePcConfig(auto_language, true).valid &&
                        RuntimeXboxLanguageFromPcConfig(auto_language) ==
                            RuntimeXboxLanguageForWindowsLanguage(GetUserDefaultUILanguage()),
                    "language auto must validate and follow the Windows display language");

    // Key bindings: the writer stores known key/mouse names (creating the
    // [input.bind] table) and refuses unknown ones.
    const auto bind_source = executable_directory / "bind_source.toml";
    const auto bind_target = executable_directory / "bind_saved.toml";
    std::ofstream(bind_source, std::ios::binary | std::ios::trunc) << "pc_config_version = 1\n";
    InstallRuntimePcPresetWithOverrides(bind_source, bind_target,
                                        {{"input.bind.x", "G"}, {"input.bind.a", "X1"}}, true);
    const toml::table bind_table = toml::parse_file(bind_target.string());
    passed &= Check(bind_table["input"]["bind"]["x"].value_or(std::string{}) == "G" &&
                        bind_table["input"]["bind"]["a"].value_or(std::string{}) == "X1" &&
                        ValidateRuntimePcConfig(bind_target, true).valid,
                    "key bindings must be written into [input.bind]");
    passed &= Check(Throws([&]() {
                      InstallRuntimePcPresetWithOverrides(
                          bind_source, bind_target, {{"input.bind.x", "Banana"}}, true);
                    }),
                    "unknown key names must be refused by the writer");

    // 0.9.1 keyboard defaults (use on E, jump on Space): a 0.9.0
    // configuration with the old pair untouched gets the new pair once; any
    // choice of the player stays; every upgraded file is marked.
    {
      const auto upgrade = [](const std::string& text, RuntimeKeyBindingsUpgrade* result) {
        return toml::parse(RuntimePcConfigWithKeyBindingsUpgrade(text, "upgrade.toml", result));
      };
      const auto key = [](const toml::table& table, const char* action) {
        return table["input"]["bind"][action].value_or(std::string{"<none>"});
      };
      const auto revision = [](const toml::table& table) {
        return table["input"]["bindings_revision"].value_or(int64_t{0});
      };
      RuntimeKeyBindingsUpgrade result;
      auto table = upgrade("pc_config_version = 1\n[input.bind]\na = 'Space'\nb = 'Shift'\n"
                           "x = 'R'\ny = 'E'\nstart = 'Escape'\n", &result);
      passed &= Check(result.changed && result.swapped && result.outcome == "swapped" &&
                          key(table, "a") == "E" && key(table, "y") == "Space" &&
                          key(table, "b") == "Shift" && revision(table) == 2,
                      "untouched 0.9.0 keys must move to use on E and jump on Space");
      table = upgrade("pc_config_version = 1\n[input.bind]\nstart = 'Return'\n", &result);
      passed &= Check(result.swapped && key(table, "a") == "E" && key(table, "y") == "Space",
                      "absent A and Y keys were the old defaults and must move explicitly");
      table = upgrade("pc_config_version = 1\n[input.bind]\na = 'G'\n", &result);
      passed &= Check(result.changed && !result.swapped && result.outcome == "custom" &&
                          key(table, "a") == "G" && key(table, "y") == "E" && revision(table) == 2,
                      "a custom use key must stay and the untouched jump key keep E");
      table = upgrade("pc_config_version = 1\n[input.bind]\na = 'space'\ny = 'e'\nx = 'E'\n", &result);
      passed &= Check(!result.swapped && result.outcome == "conflict" &&
                          key(table, "a") == "space" && key(table, "x") == "E",
                      "another key on E or Space must keep the configuration unchanged");
      const std::string no_bindings = "pc_config_version = 1\n[audio]\nmaster_volume = 1.0\n";
      passed &= Check(RuntimePcConfigWithKeyBindingsUpgrade(no_bindings, "plain.toml", &result) ==
                              no_bindings &&
                          !result.changed && result.outcome == "no_bindings",
                      "a configuration without saved keys (a preset copy) must stay byte-identical");
      const std::string current =
          "pc_config_version = 1\n[input]\nbindings_revision = 2\n[input.bind]\na = 'Space'\ny = 'E'\n";
      passed &= Check(RuntimePcConfigWithKeyBindingsUpgrade(current, "current.toml", &result) == current &&
                          !result.changed && result.outcome == "current",
                      "a marked configuration (the player's later choice) must never change");

      const auto old_config = executable_directory / "keys_090.toml";
      std::ofstream(old_config, std::ios::binary | std::ios::trunc)
          << "pc_config_version = 1\n[input]\nkeyboard_mouse = true\n[input.bind]\na = 'Space'\ny = 'E'\n";
      std::string error;
      const auto first = RuntimeUpgradePcConfigFileKeyBindings(old_config, &error);
      const toml::table upgraded = toml::parse_file(old_config.string());
      const auto second = RuntimeUpgradePcConfigFileKeyBindings(old_config, &error);
      passed &= Check(first.changed && first.swapped && key(upgraded, "a") == "E" &&
                          key(upgraded, "y") == "Space" && revision(upgraded) == 2 &&
                          upgraded["input"]["keyboard_mouse"].value_or(false) &&
                          ValidateRuntimePcConfig(old_config, true).valid &&
                          !second.changed && second.outcome == "current" && error.empty(),
                      "a 0.9.0 file must be upgraded once, validated and atomically");
      passed &= Check(RuntimeUpgradePcConfigFileKeyBindings(executable_directory / "missing_keys.toml",
                                                            &error).outcome == "no_file" &&
                          error.empty(),
                      "a missing configuration must be left alone");

      const auto edited = executable_directory / "keys_edit.toml";
      std::ofstream(edited, std::ios::binary | std::ios::trunc)
          << "pc_config_version = 1\n[input.bind]\na = 'Space'\ny = 'E'\n";
      InstallRuntimePcPresetWithOverrides(edited, edited, {{"input.bind.x", "G"}}, true);
      const toml::table edit_table = toml::parse_file(edited.string());
      passed &= Check(key(edit_table, "a") == "E" && key(edit_table, "y") == "Space" &&
                          key(edit_table, "x") == "G" && revision(edit_table) == 2,
                      "a settings save must upgrade a 0.9.0 configuration before its changes");
      const toml::table first_key = toml::parse_file(bind_target.string());
      passed &= Check(revision(first_key) == 2 && key(first_key, "y") == "<none>",
                      "the first saved key must mark the configuration (absent keys = new defaults)");
      std::ofstream(edited, std::ios::binary | std::ios::trunc)
          << "pc_config_version = 1\n[input]\nbindings_revision = 3\n";
      passed &= Check(!ValidateRuntimePcConfig(edited, true).valid,
                      "an unknown key binding revision must be refused");
      const auto inspected = executable_directory / "keys_inspect.toml";
      std::ofstream(inspected, std::ios::binary | std::ios::trunc)
          << "pc_config_version = 1\n[input.bind]\na = 'Space'\ny = 'E'\n";
      const auto values = RuntimePcConfigSettingValues(inspected);
      passed &= Check(values.count("input.bind.a") && values.at("input.bind.a") == "E" &&
                          values.at("input.bind.y") == "Space" &&
                          key(toml::parse_file(inspected.string()), "a") == "Space",
                      "inspection must show the upgraded keys without writing the file");
    }

    // Saved configurations stay readable: a 0.01-step value prints as typed.
    const auto source = executable_directory / "readable_source.toml";
    const auto target = executable_directory / "readable_saved.toml";
    std::ofstream(source, std::ios::binary | std::ios::trunc)
        << "pc_config_version = 1\n[audio]\nmaster_volume = 1.0\n";
    InstallRuntimePcPresetWithOverrides(source, target, {{"audio.master_volume", "0.91"}}, true);
    std::ifstream saved(target, std::ios::binary);
    const std::string saved_text((std::istreambuf_iterator<char>(saved)),
                                 std::istreambuf_iterator<char>());
    passed &= Check(saved_text.find("master_volume = 0.91\n") != std::string::npos &&
                        saved_text.find("0.9100000") == std::string::npos,
                    "saved floats must use the short readable form");
  }

  if (passed) {
    std::cout << "Runtime PC settings selection tests passed\n";
  }
  return passed ? 0 : 1;
}
