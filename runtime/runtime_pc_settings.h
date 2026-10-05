#pragma once

#include "runtime_input.h"
#include "runtime_user_data.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

enum class RuntimeLaunchAction {
    Run,
    Help,
    PrintPaths,
    ListPresets,
    PrintCapabilities,
    ValidateConfig,
    InspectConfig,
    ValidateMods,
    VerifyPackage,
    InstallPreset,
};

struct RuntimeLaunchOptions {
    RuntimeLaunchAction action{RuntimeLaunchAction::Run};
    std::filesystem::path xexPath;
    std::filesystem::path pcConfigPath;
    std::filesystem::path pcConfigInstallPath;
    std::filesystem::path modsConfigPath;
    std::filesystem::path userDataRoot;
    // --user-data-root: that folder also holds the settings unless
    // --pc-config/--preset name another file (isolated runs never touch the
    // player's own folder), and nothing is copied into it.
    bool userDataRootExplicit{};
    bool pcConfigExplicit{};
    bool inputDiagnostics{};
    bool frameCadenceDiagnostics{};
    bool oneVblankExperiment{};
    bool immediateDeadlineExperiment{};
    // V367: on by default (analog movement at high frame rates, see
    // runtime_movement_packet.h); --no-movement-packet-compaction for A/B.
    bool movementPacketCompaction{true};
    bool hitchDiagnostics{};
    bool audioDiagnostics{};
    bool overwritePcConfig{};
    struct PcConfigOverride {
        std::string key;
        std::string value;
    };
    std::vector<PcConfigOverride> pcConfigOverrides;
};

// Parses the runtime's title path and PC configuration selection without
// interpreting graphics settings in a second subsystem. ReXGlue remains the
// authoritative parser for graphics/display CVars. Arguments are UTF-8; the
// settings and saves default to the user data layout (runtime_user_data.h).
RuntimeLaunchOptions ParseRuntimeLaunchOptions(
    int argc, const char* const* argv,
    const std::filesystem::path& executableDirectory,
    const RuntimeUserDataLayout& userData);
// The same with the game folder layout (TheDarkness.pc.toml and runtime_data
// beside the executable).
RuntimeLaunchOptions ParseRuntimeLaunchOptions(
    int argc, const char* const* argv,
    const std::filesystem::path& executableDirectory);

std::filesystem::path RuntimeExecutableDirectory();
const char* RuntimeLaunchHelpText();
std::vector<std::filesystem::path> RuntimePresetPaths(
    const std::filesystem::path& executableDirectory);

// Returns stable, machine-readable declarations for features present in the
// current native runtime. Deferred features are reported explicitly so launchers
// do not infer support from a nearby or partially implemented subsystem.
std::vector<std::string> RuntimePcCapabilityLines();

// Queries Windows display topology without creating a title window or starting
// guest execution. A zero-display result is valid for a headless session.
std::vector<std::string> RuntimeHostDisplayLines();

struct RuntimePcConfigValidation {
    bool exists{};
    bool valid{};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Performs the packaged schema/range preflight before ReXGlue consumes the
// file. Unknown leaves are warnings because advanced ReXGlue CVars may be
// legitimate; malformed or invalid declared PC settings are errors.
class RuntimePcConfigSnapshot;
RuntimePcConfigValidation ValidateRuntimePcConfig(
    const std::filesystem::path& path, bool requireFile,
    const RuntimePcConfigSnapshot* snapshot = nullptr);
std::vector<std::string> RuntimePcConfigValidationLines(
    const std::filesystem::path& path,
    const RuntimePcConfigValidation& validation);

// Reports every declared scalar in stable key order for settings front ends.
// Values are percent-encoded UTF-8, and this remains a read-only pre-guest
// action; ReXGlue is still the authoritative settings consumer at startup.
std::vector<std::string> RuntimePcConfigInspectionLines(
    const std::filesystem::path& path,
    const RuntimePcConfigValidation& validation);

// Runtime-owned non-graphics setting. Missing optional config/value preserves
// the Original identity scale of 1.0; validated explicit values are 0..1.
double RuntimeInputVibrationScaleFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

// Startup-only physical XInput digital-button mapping. Missing keys preserve
// identity; each guest destination accepts one named physical button or none.
RuntimeInputButtonMap RuntimeInputButtonMapFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

struct RuntimeControllerProfile {
    // Standard Xbox profile enum: medium=0, low=1, high=2.
    uint32_t sensitivity{};
    bool invertY{};
};

// Title-native controller preferences. The runtime exposes them only through
// the standard XAM profile values that The Darkness already consumes.
RuntimeControllerProfile RuntimeControllerProfileFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

// Runtime-owned host-output setting. Missing optional config/value preserves
// exact identity volume; validated explicit values are 0..1.
double RuntimeAudioMasterVolumeFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

// Runtime-owned Xbox language selection shared by XGetLanguage and
// ExGetXConfigSetting. Missing optional config/value preserves English (1).
// Supported named values map only to language content shipped by this title;
// "auto" follows the Windows display language like a console dashboard.
uint32_t RuntimeXboxLanguageFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);
// Xbox language for a Windows LANGID: German 3, French 4, Spanish 5,
// Italian 6, anything else English 1.
uint32_t RuntimeXboxLanguageForWindowsLanguage(uint16_t windowsLanguage);
// The language-pack folder name for general.language (a language the game
// does not ship, currently "arabic"), or empty.
std::string RuntimeLanguagePackFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

// display.frame_rate (V288). "original" (default when absent) keeps the
// title's own presentation pacing; every other mode (60, half_refresh,
// refresh, custom, uncapped) selects the immediate presentation deadline and
// lets the GPU plugin pace frames. Returns true for host pacing.
bool RuntimeFrameRateUsesHostPacingFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot = nullptr);

struct RuntimePcConfigInstallResult {
    std::filesystem::path sourcePath;
    std::filesystem::path destinationPath;
    bool overwritten{};
    std::size_t overrideCount{};
};

// Installs one already-validated packaged preset as the persistent adjacent PC
// configuration. The replacement is written and flushed through a same-folder
// temporary file, and existing files are replaced only when explicitly asked.
RuntimePcConfigInstallResult InstallRuntimePcPreset(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath, bool overwrite);
RuntimePcConfigInstallResult InstallRuntimePcPresetWithOverrides(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    const std::vector<RuntimeLaunchOptions::PcConfigOverride>& overrides,
    bool overwrite);
// Schema settings present in a configuration file, as the text the
// inspection and --set-config use (throws when the file cannot be parsed).
std::map<std::string, std::string> RuntimePcConfigSettingValues(
    const std::filesystem::path& path);
// Title rendering annotations that internal resolution scaling requires
// (V162/V163, Probes 276-279): the three RGB lookup-table data passes keep the
// native grid and render targets narrower than 640 pixels stay native. They
// are correctness requirements of this title, not player settings: raising
// resolution_scale without them reproduces the verified atlas mis-sampling.
// The bloom chain (downsample FAE3, blurs 9F1D/8F96) is written for 720p:
// its taps cover a fixed texel footprint, so at internal scale it combs and
// bands. Each pass renders at the internal scale and samples its source with
// the console's footprint (:filter_scaled: S x S host texels box-reduced per
// native texel, native bilinear weights), which keeps the glow smooth. Its
// two consumers (the in-scene glow quad 207D and the final composite's tf1)
// keep enlarging any tracked native glow with native bilinear reconstruction
// (native_resolve_region_tracking, :source=native).
// 0.9.1-0.9.3.1 rendered these passes on the native grid (:filter). Their
// console-resolution render targets share EDRAM with the scaled scene, so
// image areas were copied between the two resolutions every frame (more so
// in the game's 2x-MSAA mode, where both use the same 16-tile pitch); on some
// NVIDIA RTX 20/30 cards that left black 2x4-pixel holes and flickering
// squares (issue #16, possibly #20), which no rule-free run showed.
// The pause menu blends its own grading table into the game's (6977, pause
// frames only; 0.9.1, issue #10): rendered at scale, the next table pass read
// it at native texel centres, which at even scales fall between two scaled
// texels and mixed neighbouring table entries into dark colours (the
// greenish haze, strongest at 2x, half at 4x, none at odd scales).
// The final composite has two pixel shaders: A59B41D0BD79484B with the game's
// motion blur and 22FC55CE134777AC with the built-in motion-blur-off patch;
// both read the glow through tf1 (0.9.2, issue #16: 0.9.1 enlarged it only in
// the motion-blur-off composite, so with motion blur on the native glow was
// stretched with plain bilinear - banded light flashes, a shimmering block
// grid over every glow at any internal scale above 1x).
inline constexpr int64_t kTitleScaleThreshold = 640;
inline constexpr std::string_view kTitleNativeGridRules =
    "B29F0BF45937C4C4:FDC5E32EC6045BE1:1:324:18:6;"
    "B29F0BF45937C4C4:37AC93F53126ABB7:0:324:18:26;"
    "B29F0BF45937C4C4:54D655FC471D594A:0:324:18:26;"
    "B29F0BF45937C4C4:69779AD07425E356:0:324:18:26;"
    "B29F0BF45937C4C4:FAE3BACA27F09CC9:0:1280:720:6:filter_scaled;"
    "B29F0BF45937C4C4:FAE3BACA27F09CC9:0:1280:720:26:filter_scaled;"
    "B29F0BF45937C4C4:9F1D2D64E5F75924:0:1280:720:26:filter_scaled;"
    "B29F0BF45937C4C4:9F1D2D64E5F75924:0:160:90:6:filter_scaled;"
    "B29F0BF45937C4C4:8F96D5C280D780BC:0:1280:720:26:filter_scaled;"
    "EC4685ADB9CCBC13:207D40E674A7C916:0:1280:720:26:filter_scaled:source=native;"
    "4FA9486610B42A92:22FC55CE134777AC:1:1280:720:26:filter_scaled:source=native;"
    "4FA9486610B42A92:A59B41D0BD79484B:1:1280:720:26:filter_scaled:source=native";
// The earlier built-in annotations: configurations that carry exactly one of
// these are upgraded to kTitleNativeGridRules. kLegacyTitleNativeGridRules =
// the lookup tables only (0.9.0 presets); kPreviousTitleNativeGridRules = with
// the bloom chain, before the pause table (0.9.1 development presets);
// k091TitleNativeGridRules = the published 0.9.1 (without the motion-blur
// composite); k092TitleNativeGridRules = 0.9.2 to 0.9.3.1 (bloom passes on
// the native grid, issue #16).
inline constexpr std::string_view k092TitleNativeGridRules =
    "B29F0BF45937C4C4:FDC5E32EC6045BE1:1:324:18:6;"
    "B29F0BF45937C4C4:37AC93F53126ABB7:0:324:18:26;"
    "B29F0BF45937C4C4:54D655FC471D594A:0:324:18:26;"
    "B29F0BF45937C4C4:69779AD07425E356:0:324:18:26;"
    "B29F0BF45937C4C4:FAE3BACA27F09CC9:0:1280:720:6:filter;"
    "B29F0BF45937C4C4:FAE3BACA27F09CC9:0:1280:720:26:filter;"
    "B29F0BF45937C4C4:9F1D2D64E5F75924:0:1280:720:26:filter;"
    "B29F0BF45937C4C4:9F1D2D64E5F75924:0:160:90:6:filter;"
    "B29F0BF45937C4C4:8F96D5C280D780BC:0:1280:720:26:filter;"
    "EC4685ADB9CCBC13:207D40E674A7C916:0:1280:720:26:filter_scaled:source=native;"
    "4FA9486610B42A92:22FC55CE134777AC:1:1280:720:26:filter_scaled:source=native;"
    "4FA9486610B42A92:A59B41D0BD79484B:1:1280:720:26:filter_scaled:source=native";
inline constexpr std::string_view k091TitleNativeGridRules =
    "B29F0BF45937C4C4:FDC5E32EC6045BE1:1:324:18:6;"
    "B29F0BF45937C4C4:37AC93F53126ABB7:0:324:18:26;"
    "B29F0BF45937C4C4:54D655FC471D594A:0:324:18:26;"
    "B29F0BF45937C4C4:69779AD07425E356:0:324:18:26;"
    "B29F0BF45937C4C4:FAE3BACA27F09CC9:0:1280:720:6:filter;"
    "B29F0BF45937C4C4:FAE3BACA27F09CC9:0:1280:720:26:filter;"
    "B29F0BF45937C4C4:9F1D2D64E5F75924:0:1280:720:26:filter;"
    "B29F0BF45937C4C4:9F1D2D64E5F75924:0:160:90:6:filter;"
    "B29F0BF45937C4C4:8F96D5C280D780BC:0:1280:720:26:filter;"
    "EC4685ADB9CCBC13:207D40E674A7C916:0:1280:720:26:filter_scaled:source=native;"
    "4FA9486610B42A92:22FC55CE134777AC:1:1280:720:26:filter_scaled:source=native";
inline constexpr std::string_view kLegacyTitleNativeGridRules =
    "B29F0BF45937C4C4:FDC5E32EC6045BE1:1:324:18:6;"
    "B29F0BF45937C4C4:37AC93F53126ABB7:0:324:18:26;"
    "B29F0BF45937C4C4:54D655FC471D594A:0:324:18:26";
inline constexpr std::string_view kPreviousTitleNativeGridRules =
    "B29F0BF45937C4C4:FDC5E32EC6045BE1:1:324:18:6;"
    "B29F0BF45937C4C4:37AC93F53126ABB7:0:324:18:26;"
    "B29F0BF45937C4C4:54D655FC471D594A:0:324:18:26;"
    "B29F0BF45937C4C4:FAE3BACA27F09CC9:0:1280:720:6:filter;"
    "B29F0BF45937C4C4:FAE3BACA27F09CC9:0:1280:720:26:filter;"
    "B29F0BF45937C4C4:9F1D2D64E5F75924:0:1280:720:26:filter;"
    "B29F0BF45937C4C4:9F1D2D64E5F75924:0:160:90:6:filter;"
    "B29F0BF45937C4C4:8F96D5C280D780BC:0:1280:720:26:filter;"
    "EC4685ADB9CCBC13:207D40E674A7C916:0:1280:720:26:filter_scaled:source=native;"
    "4FA9486610B42A92:22FC55CE134777AC:1:1280:720:26:filter_scaled:source=native";
// The configuration text the graphics plugin receives: unchanged at internal
// scale 1 or when all annotations are present; otherwise the missing ones
// are added, including native_resolve_region_tracking = true (explicit
// values in the configuration win, except an earlier built-in rule string,
// which is replaced). Throws when the text cannot be parsed (it was validated
// before).
std::string RuntimePcConfigWithTitleScaleRequirements(const std::string& contents,
                                                      const std::string& sourceName,
                                                      bool* changed = nullptr);
// The GPU configuration with gpu_program_cache_source (the title's compiled
// shader cache, System/Xenon/ProgramCache.xpc, from which the shader prewarm
// rebuilds the packaged shader list at startup, #5) as a top-level key.
std::string RuntimePcConfigWithProgramCacheSource(const std::string& contents,
                                                  const std::filesystem::path& programCache);
// resolution_scale = 0 is Automatic (V330, runtime_auto_scale.h): the GPU
// receives automaticScale instead; other values stay unchanged.
struct RuntimePcScaleTarget {
    int64_t resolutionScale = 1;
    std::string outputResolution = "native";
    int64_t monitor = 0;
    // The frame rate automatic scale should hold (display.frame_rate,
    // display.frame_limit for "custom").
    std::string frameRate = "refresh";
    uint32_t frameLimit = 0;
};
// display.widescreen (V407): fill non-16:9 screens (runtime_widescreen.h).
bool RuntimeWidescreenFromPcConfig(const std::filesystem::path& path,
                                   const RuntimePcConfigSnapshot* snapshot = nullptr);
RuntimePcScaleTarget RuntimePcScaleTargetFromContents(const std::string& contents,
                                                      const std::string& sourceName);
std::string RuntimePcConfigWithAutomaticScale(const std::string& contents,
                                              const std::string& sourceName,
                                              uint32_t automaticScale, bool* changed = nullptr);
// Shelved temporal AA (V440): graphics.temporal_aa reaches the GPU as "off"
// unless temporalAaOffered (DARKNESS_EXPERIMENTAL=temporal_aa); the other
// values stay unchanged. shelved receives the value that was turned off.
std::string RuntimePcConfigWithShelvedFeatures(const std::string& contents,
                                               const std::string& sourceName,
                                               bool temporalAaOffered,
                                               std::string* shelved = nullptr);
std::vector<std::string> RuntimePcConfigInstallLines(
    const RuntimePcConfigInstallResult& result);
std::vector<std::string> RuntimePcConfigInstallErrorLines(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath, const std::string& error);

// Keyboard defaults (0.9.1): use (A) on E and jump (Y) on Space; 0.9.0 had A
// on Space and Y on E. input.bindings_revision marks configurations written
// with the new defaults. One without it gets the new pair when its A and Y
// keys are still the old defaults (or absent) and no other binding uses Space
// or E; a configuration with its own choice keeps it (an absent A or Y key is
// written as its old default, so nothing moves). Either way it is marked, so
// a later choice is never changed again. Configurations without bindings use
// the new defaults.
inline constexpr int64_t kRuntimeKeyBindingsRevision = 2;
struct RuntimeKeyBindingsUpgrade {
    bool changed = false;  // the configuration text changed (marked)
    bool swapped = false;  // A and Y moved to E and Space
    std::string outcome;   // current, no_bindings, swapped, custom, conflict
};
// The upgrade of configuration text: unchanged text when it is already current.
std::string RuntimePcConfigWithKeyBindingsUpgrade(const std::string& contents,
                                                  const std::string& sourceName,
                                                  RuntimeKeyBindingsUpgrade* upgrade = nullptr);
// Rewrites a player's configuration file through the validated atomic install
// when the upgrade changes it (never for packaged presets or examples). Missing
// files are left alone; failures are returned in error (the file is unchanged).
RuntimeKeyBindingsUpgrade RuntimeUpgradePcConfigFileKeyBindings(
    const std::filesystem::path& path, std::string* error = nullptr);
