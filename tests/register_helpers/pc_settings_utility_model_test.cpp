#include "pc_settings_utility_model.h"
#include "pc_settings_schema.h"
#include "runtime_camera_policy.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}
}  // namespace

int main() {
    bool passed = true;
    passed &= Check(!PcSettingsHavePendingChanges(false, 0), "saved unchanged state is not pending");
    passed &= Check(PcSettingsHavePendingChanges(true, 0), "profile-only selection must warn before discard");
    passed &= Check(PcSettingsHavePendingChanges(false, 1), "value-only edit remains pending");
    passed &= Check(PcSettingsHavePendingChanges(true, 2), "profile and value edits remain pending");
    std::error_code error;
    const auto root = std::filesystem::temp_directory_path() /
                      "darkness pc settings utility model";
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "presets", error);
    passed &= Check(!error, "temporary preset directory was not created");

    std::ofstream(root / "presets" / "quality_4k.toml") << "quality=4k\n";
    std::ofstream(root / "presets" / "quality_1440p.toml")
        << "quality=1440p\n";
    std::ofstream(root / "presets" / "original_720p.toml") << "quality=original720\n";
    std::ofstream(root / "presets" / "original.toml") << "quality=original\n";
    std::ofstream(root / "presets" / "performance.toml") << "quality=performance\n";
    std::ofstream(root / "presets" / "enhanced.toml") << "quality=enhanced\n";
    std::ofstream(root / "presets" / "custom_night.toml") << "quality=custom\n";
    std::ofstream(root / "presets" / "ignored.txt") << "ignore\n";
    std::filesystem::create_directories(root / "presets" / "folder.toml", error);

    const auto presets = EnumeratePcPresetOptions(root / "presets");
    passed &= Check(presets.size() == 7,
                    "preset enumeration did not filter by regular TOML file");
    // V330: the three player quality levels first, then the packaged
    // output-path/validation presets, then unknown ones.
    passed &= Check(presets.size() == 7 &&
                        presets[0].presetName == L"enhanced" &&
                        presets[1].presetName == L"performance" &&
                        presets[2].presetName == L"original" &&
                        presets[3].presetName == L"original_720p" &&
                        presets[4].presetName == L"quality_1440p" &&
                        presets[5].presetName == L"quality_4k",
                    "known presets lost their stable user-facing order");
    passed &= Check(!presets.empty() &&
                        presets[0].displayName == L"Enhanced (recommended)",
                    "Enhanced preset is not first with its recommended label");
    passed &= Check(presets.size() == 7 &&
                        presets[1].displayName == L"Performance (for weaker PCs)" &&
                        presets[2].displayName == L"Original (console look)",
                    "player presets lost their plain labels");
    passed &= Check(presets.size() == 7 &&
                        presets[3].displayName.find(L"Xbox-compatible") != std::wstring::npos &&
                        presets[4].displayName.find(L"1440p output") != std::wstring::npos &&
                        presets[5].displayName.find(L"output-path validation") !=
                            std::wstring::npos,
                    "output-path presets lost their explicit labels");
    passed &= Check(presets.size() == 7 &&
                        presets[6].displayName == L"Custom Night",
                    "unknown preset fallback label is not readable");
    passed &= Check(presets.size() == 7 &&
                        presets[0].audience == PcPresetAudience::Player &&
                        presets[1].audience == PcPresetAudience::Player &&
                        presets[2].audience == PcPresetAudience::Player &&
                        presets[3].audience == PcPresetAudience::Developer &&
                        presets[4].audience == PcPresetAudience::Developer &&
                        presets[5].audience == PcPresetAudience::Developer &&
                        presets[6].audience == PcPresetAudience::Developer,
                    "only the three quality levels may be offered to players by default");

    std::ofstream(root / "TheDarkness.pc.toml") << "quality=4k\n";
    const auto match = FindExactPcPresetMatch(
        root / "TheDarkness.pc.toml", root / "presets", presets);
    passed &= Check(match && presets[*match].presetName == L"quality_4k",
                    "exact active configuration match was not detected");
    std::ofstream(root / "TheDarkness.pc.toml", std::ios::app) << "custom=true\n";
    passed &= Check(!FindExactPcPresetMatch(root / "TheDarkness.pc.toml",
                                            root / "presets", presets),
                    "custom configuration was mislabeled as a preset");

    passed &= Check(QuoteWindowsCommandLineArgument(L"simple") == L"simple",
                    "simple Windows argument was unnecessarily changed");
    passed &= Check(QuoteWindowsCommandLineArgument(L"C:\\Program Files\\Game\\") ==
                        L"\"C:\\Program Files\\Game\\\\\"",
                    "Windows path quoting lost a trailing backslash");
    passed &= Check(QuoteWindowsCommandLineArgument(L"a\"b") == L"\"a\\\"b\"",
                    "embedded quote was not escaped correctly");
    const auto command = BuildPcPresetInstallCommandLine(
        L"C:\\The Darkness\\TheDarkness.exe", L"original_720p", true);
    passed &= Check(command ==
                        L"\"C:\\The Darkness\\TheDarkness.exe\" --install-preset original_720p --overwrite-config",
                    "preset installation command line is not deterministic");
    const auto customizedCommand = BuildPcPresetInstallCommandLine(
        L"C:\\The Darkness\\TheDarkness.exe", L"quality_1440p", true,
        {{L"resolution_scale", L"2"},
         {L"camera.field_of_view", L"105"}});
    passed &= Check(
        customizedCommand ==
            L"\"C:\\The Darkness\\TheDarkness.exe\" --install-preset quality_1440p --set-config resolution_scale=2 --set-config camera.field_of_view=105 --overwrite-config",
        "custom preset command line lost deterministic schema overrides");

    const auto& schema = PcEditableSettingsSchema();
    passed &= Check(BuildPcPresetInstallCommandLine(
                        L"C:\\Game\\TheDarkness.exe", L"", true,
                        {{L"audio.master_volume", L"0.5"}}) ==
                        L"C:\\Game\\TheDarkness.exe --edit-config --set-config audio.master_volume=0.5 --overwrite-config",
                    "current-settings edit must not reload a packaged preset");
    passed &= Check(kPcEditableSettingsSchemaVersion == 1 &&
                        schema.size() == 55 &&
                        FindPcEditableSetting("graphics.msaa_mode") &&
                        FindPcEditableSetting("graphics.glow_reconstruction") &&
                        FindPcEditableSetting("input.button_prompts") &&
                        FindPcEditableSetting("display.widescreen") &&
                        FindPcEditableSetting("graphics.hd_textures") &&
                        FindPcEditableSetting("graphics.temporal_aa") &&
                        FindPcEditableSetting("display.menu_frame_rate") &&
                        FindPcEditableSetting("display.low_latency") &&
                        FindPcEditableSetting("present.effect") &&
                        FindPcEditableSetting("output_resolution") &&
                        FindPcEditableSetting("display.frame_rate") &&
                        FindPcEditableSetting("input.controller_invert_y") &&
                        !FindPcEditableSetting("graphics.unknown"),
                    "shared editable settings schema lost required Phase-1 fields");
    const auto* volume = FindPcEditableSetting("audio.master_volume");
    const auto* fov = FindPcEditableSetting("camera.field_of_view");
    passed &= Check(fov && fov->defaultNumber == kOriginalGameplayFovDegrees &&
                        ParsePcDisplayedNumber(*fov, L"105") == 105.0 &&
                        PcSliderValue(*fov, PcSliderPosition(*fov, 105.0)) == 105.0,
                    "FOV slider limits/default/conversion must come from the runtime contract");
    passed &= Check(volume && ParsePcDisplayedNumber(*volume, L"50") == 0.5 &&
                        FormatPcDisplayedNumber(*volume, 0.5) == L"50" &&
                        PcSliderValue(*volume, PcSliderPosition(*volume, 0.5)) == 0.5,
                    "volume percentage must preserve the runtime gain value");
    for (const auto text : {L"", L"nan", L"inf", L"105x", L"59", L"121"})
        passed &= Check(!ParsePcDisplayedNumber(*fov, text), "invalid FOV must not silently clamp");
    for (const auto& spec : schema) {
        if (spec.editor != PcSettingEditorKind::Number) continue;
        passed &= Check(spec.defaultNumber && *spec.defaultNumber >= spec.minimum &&
                            *spec.defaultNumber <= spec.maximum && spec.restartRequired &&
                            PcSliderValue(spec, 0) == spec.minimum &&
                            PcSliderValue(spec, PcSliderStepCount(spec)) == spec.maximum,
                        "numeric control must have valid schema defaults and reachable endpoints");
    }
    passed &= Check(volume && volume->editor == PcSettingEditorKind::Number &&
                        volume->minimum == 0.0 && volume->maximum == 1.0 &&
                        volume->restartRequired,
                    "audio editor must expose only the existing startup volume range");
    const auto sections = PcEditableSettingsSections();
    passed &= Check(sections == std::vector<std::wstring_view>{L"Language", L"Display", L"Graphics Quality",
                        L"Performance / Frame Rate", L"Camera", L"Controls", L"Key bindings",
                        L"Audio", L"Advanced"},
                    "settings categories must be unique, ordered and cover the schema");
    for (const auto section : sections) {
        size_t count = 0;
        for (const auto& setting : schema) if (setting.section == section) ++count;
        // V315 launcher and overlay scroll each section; only emptiness matters.
        passed &= Check(count > 0, "settings category has no settings");
    }
    const auto* mouse = FindPcEditableSetting("input.mouse_sensitivity");
    passed &= Check(mouse && mouse->minimum == 0.01 && mouse->maximum == 10.0 &&
                        FindPcEditableSetting("input.mouse_invert_y") &&
                        FindPcEditableSetting("general.language")->choices.size() == 7 &&
                        FindPcEditableSetting("general.language")->choices[6].value == "arabic" &&
                        FindPcEditableSetting("general.language")->choices[0].value == "auto",
                    "mouse bridge and language editors must match supported runtime fields");
    const auto* mouseLook = FindPcEditableSetting("input.mouse_look");
    passed &= Check(mouseLook && mouseLook->editor == PcSettingEditorKind::Choice &&
                        mouseLook->choices.size() == 2 &&
                        mouseLook->choices[0].value == "native" &&
                        mouseLook->choices[1].value == "stick",
                    "mouse look editor must offer native (default) and stick-bridge modes");

    const auto inspectCommand = BuildPcConfigInspectionCommandLine(
        L"C:\\The Darkness\\TheDarkness.exe",
        L"C:\\The Darkness\\presets\\quality 1440p.toml");
    passed &= Check(
        inspectCommand ==
            L"\"C:\\The Darkness\\TheDarkness.exe\" --inspect-config --pc-config \"C:\\The Darkness\\presets\\quality 1440p.toml\"",
        "configuration inspection command line is not deterministic");
    passed &= Check(
        BuildPcDefaultLaunchCommandLine(
            L"C:\\The Darkness\\TheDarkness.exe") ==
            L"\"C:\\The Darkness\\TheDarkness.exe\"",
        "default launch command line is not deterministic");
    passed &= Check(
        BuildPcSafeModeLaunchCommandLine(
            L"C:\\The Darkness\\TheDarkness.exe") ==
            L"\"C:\\The Darkness\\TheDarkness.exe\" --preset original_720p",
        "safe-mode launch must select Original without editing persistent config");

    const std::string inspection =
        "PC_CONFIG_ENTRY_0_KEY=output_resolution\r\n"
        "PC_CONFIG_ENTRY_0_TYPE=string\r\n"
        "PC_CONFIG_ENTRY_0_VALUE=1440p\r\n"
        "PC_CONFIG_ENTRY_0_RESTART_REQUIRED=1\r\n"
        "PC_CONFIG_ENTRY_1_KEY=resolution_scale\n"
        "PC_CONFIG_ENTRY_1_TYPE=integer\n"
        "PC_CONFIG_ENTRY_1_VALUE=2\n"
        "PC_CONFIG_ENTRY_1_RESTART_REQUIRED=1\n"
        "PC_CONFIG_ENTRY_2_KEY=graphics.motion_blur\n"
        "PC_CONFIG_ENTRY_2_TYPE=boolean\n"
        "PC_CONFIG_ENTRY_2_VALUE=false\n"
        "PC_CONFIG_ENTRY_2_RESTART_REQUIRED=1\n"
        "PC_CONFIG_ENTRY_3_KEY=encoded%2Ekey\n"
        "PC_CONFIG_ENTRY_3_TYPE=string\n"
        "PC_CONFIG_ENTRY_3_VALUE=a%25b%0Ac\n"
        "PC_CONFIG_ENTRY_3_RESTART_REQUIRED=0\n"
        "PC_CONFIG_ENTRY_4_KEY=incomplete\n";
    const auto entries = ParsePcConfigInspection(inspection);
    passed &= Check(entries.size() == 4 &&
                        entries[0].key == "output_resolution" &&
                        entries[0].value == "1440p" &&
                        entries[0].restartRequired &&
                        entries[3].key == "encoded.key" &&
                        entries[3].value == "a%b\nc" &&
                        !entries[3].restartRequired,
                    "runtime inspection contract was not parsed strictly");
    const auto summary = FormatPcConfigInspectionSummary(entries);
    passed &= Check(summary.find("Output: 1440p") != std::string::npos &&
                        summary.find("Internal scale: 2x") !=
                            std::string::npos &&
                        summary.find("Motion Blur: Off") !=
                            std::string::npos &&
                        summary.find("applied at the next game start") !=
                            std::string::npos,
                    "effective settings summary lost required Phase-1 fields");
    passed &= Check(ParsePcConfigInspection(
                        "PC_CONFIG_ENTRY_0_KEY=bad%Q0\n"
                        "PC_CONFIG_ENTRY_0_TYPE=string\n"
                        "PC_CONFIG_ENTRY_0_VALUE=value\n"
                        "PC_CONFIG_ENTRY_0_RESTART_REQUIRED=1\n")
                        .empty(),
                    "malformed percent encoding was accepted");

    std::ofstream(root / "presets" / "validation_1440p_internal_2x.toml")
        << "validation=2x\n";
    std::ofstream(root / "presets" / "validation_4k_windowed_internal_2x.toml")
        << "validation=4k_windowed\n";
    const auto validationPresets = EnumeratePcPresetOptions(root / "presets");
    passed &= Check(validationPresets.size() == 9 &&
                        validationPresets[6].displayName.find(L"VALIDATION:") == 0 &&
                        validationPresets[7].displayName.find(L"not physical 4K") !=
                            std::wstring::npos &&
                        validationPresets[6].audience == PcPresetAudience::Developer &&
                        validationPresets[7].audience == PcPresetAudience::Developer,
                    "internal2x validation presets must be obvious, developer-only and not "
                    "claim physical4K");
    auto gridEntries = entries;
    gridEntries.push_back({"draw_resolution_scale_native_grid_rules", "string",
                          "1:2:0:4:4:6;1:3:0:4:4:6;1:4:0:4:4:6", true});
    passed &= Check(FormatPcConfigInspectionSummary(gridEntries).find(
                        "Data-table grid: native (3 annotated passes") != std::string::npos,
                    "effective summary must expose retained native data-grid policy");
    gridEntries.push_back({"native_resolve_region_tracking", "bool", "true", true});
    gridEntries.push_back({"native_resolve_region_sampling", "bool", "true", true});
    passed &= Check(FormatPcConfigInspectionSummary(gridEntries).find(
                        "enabled for eligible 2x regions (1x unchanged; restart required)") != std::string::npos,
                    "effective summary must state native-region scope and restart requirement");

    std::filesystem::remove_all(root, error);
    if (passed) {
        std::cout << "PC settings utility model regression passed\n";
    }
    return passed ? 0 : 1;
}
