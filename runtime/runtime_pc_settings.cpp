#include "runtime_pc_settings.h"
#include "product_name.h"
#include "runtime_game_setup.h"
#include "pc_settings_schema.h"
#include "runtime_pc_config_snapshot.h"
#include "../external/ReXGlue/include/rex/graphics/pipeline/render_target/native_shader_scale_policy.h"
#include "../external/ReXGlue/include/rex/ui/settings_schema.h"
#include "../external/ReXGlue/include/rex/ui/frame_rate_policy.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cwchar>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
// Values a choice setting accepts beyond its listed choices: whole-number
// frame-rate caps (V330; the surfaces list the ones that suit the display).
bool IsExtraChoiceValue(std::string_view key, std::string_view value) {
    return key == "display.frame_rate" && rex::ui::IsFrameRateValue(value);
}

bool ConfigPresent(const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot) {
    if (snapshot) {
        if (snapshot->origin() != std::filesystem::absolute(path).lexically_normal())
            throw std::runtime_error("startup configuration snapshot origin mismatch");
        return snapshot->present();
    }
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}
// The 0.9.1 key binding upgrade (runtime_pc_settings.h), defined below.
RuntimeKeyBindingsUpgrade UpgradeKeyBindings(toml::table& config);
toml::table ParseConfig(const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot) {
    return snapshot ? toml::parse(snapshot->contents(), snapshot->origin().u8string())
                    : toml::parse_file(path.u8string());
}
// Saved configurations are read by people too: settings with 0.01 steps
// print as 0.91, not with full double precision (0.91000000000000003).
toml::toml_formatter PcConfigFormatter(const toml::table& config) {
    return toml::toml_formatter{config, toml::toml_formatter::default_flags |
                                            toml::format_flags::relaxed_float_precision};
}
constexpr const wchar_t* kDefaultGameDirectory =
    L"Darkness, The (USA, Europe) (En,Fr,De,Es,It)";
constexpr const wchar_t* kDefaultPcConfigName = L"TheDarkness.pc.toml";
constexpr const wchar_t* kDefaultModsConfigName = L"TheDarkness.mods.toml";

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::filesystem::path AbsoluteNormalized(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal();
}

std::filesystem::path ResolveDefaultXexPath(
    const std::filesystem::path& executableDirectory,
    const std::filesystem::path& dataFolder) {
    // The player's game from the launcher's setup (location file or the
    // extracted "game" folder, in the local data folder or beside the
    // executables), then the disc-dump folder name upwards.
    if (const std::filesystem::path configured = darkness::game_setup::ConfiguredGameXex(
            AbsoluteNormalized(dataFolder.empty() ? executableDirectory : dataFolder),
            AbsoluteNormalized(executableDirectory));
        !configured.empty()) {
        return configured.lexically_normal();
    }
    std::filesystem::path directory = AbsoluteNormalized(executableDirectory);
    for (;;) {
        const std::filesystem::path candidate =
            directory / kDefaultGameDirectory / L"default.xex";
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate.lexically_normal();
        }
        const std::filesystem::path parent = directory.parent_path();
        if (parent.empty() || parent == directory) break;
        directory = parent;
    }
    // Keep a deterministic, executable-relative path for a packaged build
    // whose extracted title has not been placed yet. The later file-open error
    // then names the exact expected location instead of depending on CWD.
    return AbsoluteNormalized(executableDirectory / kDefaultGameDirectory /
                              L"default.xex");
}

std::filesystem::path ResolvePresetPath(
    const std::filesystem::path& executableDirectory,
    std::string_view requestedName) {
    if (requestedName.empty()) {
        throw std::runtime_error("--preset requires exactly one non-empty name");
    }
    std::filesystem::path requested{std::string(requestedName)};
    if (requested.is_absolute() || requested.has_parent_path() ||
        requested.filename() != requested) {
        throw std::runtime_error(
            "--preset accepts a packaged preset name, not a path");
    }
    if (requested.extension().empty()) requested += ".toml";
    if (LowerAscii(requested.extension().string()) != ".toml") {
        throw std::runtime_error("--preset requires a TOML preset name");
    }

    const std::string requestedLower = LowerAscii(requested.filename().string());
    for (const auto& packaged : RuntimePresetPaths(executableDirectory)) {
        if (LowerAscii(packaged.filename().string()) == requestedLower) {
            return AbsoluteNormalized(executableDirectory / L"presets" /
                                      packaged.filename());
        }
    }
    throw std::runtime_error("packaged PC preset not found: " +
                             std::string(requestedName));
}

std::string WideToUtf8(const wchar_t* value) {
    if (!value || !value[0]) return {};
    const int characters = static_cast<int>(std::wcslen(value));
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                                          characters, nullptr, 0, nullptr,
                                          nullptr);
    if (bytes <= 0) return {};
    std::string converted(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, characters,
                            converted.data(), bytes, nullptr, nullptr) != bytes) {
        return {};
    }
    return converted;
}

std::string NormalizeResolution(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) || character == '_' || character == '-') continue;
        normalized.push_back(static_cast<char>(std::tolower(byte)));
    }
    return normalized;
}

bool IsValidOutputResolution(std::string_view value) {
    const std::string normalized = NormalizeResolution(value);
    constexpr std::array<std::string_view, 12> presets = {
        "native", "desktop", "nativedesktop", "480p", "540p", "720p",
        "900p", "1080p", "1440p", "1800p", "2160p", "4k"};
    if (std::find(presets.begin(), presets.end(), normalized) !=
        presets.end()) {
        return true;
    }
    const size_t separator = normalized.find('x');
    if (!separator || separator == std::string::npos ||
        separator + 1 >= normalized.size() ||
        normalized.find('x', separator + 1) != std::string::npos) {
        return false;
    }
    const auto parseDimension = [](std::string_view text, int& output) {
        if (text.empty()) return false;
        int value = 0;
        for (char character : text) {
            if (character < '0' || character > '9') return false;
            value = value * 10 + (character - '0');
            if (value > 8192) return false;
        }
        output = value;
        return value > 0;
    };
    int width = 0;
    int height = 0;
    return parseDimension(std::string_view(normalized).substr(0, separator),
                          width) &&
           parseDimension(std::string_view(normalized).substr(separator + 1),
                          height);
}

const toml::node* FindConfigNode(const toml::table& root,
                                 std::string_view dottedPath) {
    const toml::table* table = &root;
    size_t start = 0;
    for (;;) {
        const size_t separator = dottedPath.find('.', start);
        const std::string_view part = dottedPath.substr(
            start, separator == std::string_view::npos
                       ? dottedPath.size() - start
                       : separator - start);
        const toml::node* node = table->get(part);
        if (!node) return nullptr;
        if (separator == std::string_view::npos) return node;
        table = node->as_table();
        if (!table) return nullptr;
        start = separator + 1;
    }
}

void CollectConfigLeaves(const toml::table& table, const std::string& prefix,
                         std::vector<std::string>& leaves,
                         std::vector<std::string>& unsupportedTypes) {
    for (const auto& [key, value] : table) {
        const std::string path =
            prefix.empty() ? std::string(key.str())
                           : prefix + "." + std::string(key.str());
        if (const toml::table* child = value.as_table()) {
            CollectConfigLeaves(*child, path, leaves, unsupportedTypes);
        } else if (value.is_boolean() || value.is_integer() ||
                   value.is_floating_point() || value.is_string()) {
            leaves.push_back(path);
        } else {
            unsupportedTypes.push_back(path);
        }
    }
}

std::string SingleLine(std::string value) {
    for (char& character : value) {
        if (character == '\r' || character == '\n') character = ' ';
    }
    return value;
}

std::string PercentEncodeMachineField(std::string_view value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char byte : value) {
        if (byte >= 0x20 && byte <= 0x7E && byte != '%') {
            encoded.push_back(static_cast<char>(byte));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[byte >> 4]);
        encoded.push_back(kHex[byte & 0xF]);
    }
    return encoded;
}

// Parent table of a schema key. A configuration written before a key existed
// gets the missing tables (schema keys only; values are validated by kind).
std::pair<toml::table*, std::string> FindMutableConfigParent(
    toml::table& root, std::string_view dottedPath) {
    toml::table* table = &root;
    size_t start = 0;
    for (;;) {
        const size_t separator = dottedPath.find('.', start);
        const std::string part(dottedPath.substr(
            start, separator == std::string_view::npos
                       ? dottedPath.size() - start
                       : separator - start));
        if (part.empty()) {
            throw std::runtime_error("PC setting key contains an empty path component");
        }
        if (separator == std::string_view::npos) return {table, part};
        toml::node* node = table->get(part);
        if (!node) {
            table = table->insert_or_assign(part, toml::table{}).first->second.as_table();
        } else {
            table = node->as_table();
        }
        if (!table) {
            throw std::runtime_error(
                "PC setting key's parent is not a table: " + std::string(dottedPath));
        }
        start = separator + 1;
    }
}

int64_t ParseExactInteger(std::string_view text, std::string_view key) {
    int64_t value = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
        throw std::runtime_error(
            "PC setting override requires an integer: " + std::string(key));
    }
    return value;
}

double ParseExactNumber(std::string_view text, std::string_view key) {
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    input >> std::noskipws;
    double value = 0.0;
    input >> value;
    if (!input || input.peek() != std::char_traits<char>::eof() ||
        !std::isfinite(value)) {
        throw std::runtime_error(
            "PC setting override requires a finite number: " +
            std::string(key));
    }
    return value;
}

void ApplyPcConfigOverride(
    toml::table& config,
    const RuntimeLaunchOptions::PcConfigOverride& override) {
    const PcEditableSettingSpec* setting =
        FindPcEditableSetting(override.key);
    if (!setting) {
        throw std::runtime_error(
            "PC setting override is not exposed by the versioned schema: " +
            override.key);
    }
    auto [parent, leaf] = FindMutableConfigParent(config, override.key);
    switch (setting->editor) {
        case PcSettingEditorKind::Boolean:
            if (override.value == "true") {
                parent->insert_or_assign(leaf, true);
            } else if (override.value == "false") {
                parent->insert_or_assign(leaf, false);
            } else {
                throw std::runtime_error(
                    "PC setting override requires true or false: " +
                    override.key);
            }
            break;
        case PcSettingEditorKind::Integer:
            parent->insert_or_assign(
                leaf, ParseExactInteger(override.value, override.key));
            break;
        case PcSettingEditorKind::IntegerChoice: {
            const bool allowed = std::any_of(
                setting->choices.begin(), setting->choices.end(),
                [&](const PcSettingChoice& choice) {
                    return choice.value == override.value;
                });
            if (!allowed) {
                throw std::runtime_error(
                    "PC setting override has an unsupported choice: " +
                    override.key);
            }
            parent->insert_or_assign(
                leaf, ParseExactInteger(override.value, override.key));
            break;
        }
        case PcSettingEditorKind::Number:
            parent->insert_or_assign(
                leaf, ParseExactNumber(override.value, override.key));
            break;
        case PcSettingEditorKind::Choice: {
            const bool allowed = std::any_of(
                setting->choices.begin(), setting->choices.end(),
                [&](const PcSettingChoice& choice) {
                    return choice.value == override.value;
                }) || IsExtraChoiceValue(setting->key, override.value);
            if (!allowed) {
                throw std::runtime_error(
                    "PC setting override has an unsupported choice: " +
                    override.key);
            }
            parent->insert_or_assign(leaf, override.value);
            break;
        }
        case PcSettingEditorKind::Resolution:
            parent->insert_or_assign(leaf, override.value);
            break;
        case PcSettingEditorKind::Key:
            // Empty unbinds; otherwise a name the input driver understands.
            if (!override.value.empty() &&
                !rex::ui::settings::IsBindingKeyName(override.value)) {
                throw std::runtime_error(
                    "PC setting override is not a key or mouse button name: " +
                    override.key);
            }
            parent->insert_or_assign(leaf, override.value);
            break;
    }
}
}

RuntimeLaunchOptions ParseRuntimeLaunchOptions(
    int argc, const char* const* argv,
    const std::filesystem::path& executableDirectory) {
    return ParseRuntimeLaunchOptions(
        argc, argv, executableDirectory,
        RuntimeGameFolderUserDataLayout(executableDirectory,
                                        RuntimeUserDataMode::kPortable, {}));
}

RuntimeLaunchOptions ParseRuntimeLaunchOptions(
    int argc, const char* const* argv,
    const std::filesystem::path& executableDirectory,
    const RuntimeUserDataLayout& userData) {
    RuntimeLaunchOptions options{};
    options.xexPath = ResolveDefaultXexPath(executableDirectory, userData.localData);
    options.pcConfigPath = userData.configPath;
    options.pcConfigInstallPath = userData.configPath;
    options.modsConfigPath = executableDirectory / kDefaultModsConfigName;
    options.userDataRoot = userData.root;
    // Paths arrive as UTF-8 (main converts the wide command line).
    const auto argumentPath = [](std::string_view text) {
        return std::filesystem::u8path(text.begin(), text.end());
    };

    bool hasXexPath = false;
    bool hasPcConfigPath = false;
    bool hasPreset = false;
    bool hasModsConfigPath = false;
    bool hasUserDataRoot = false;
    bool hasInputDiagnostics = false;
    bool hasFrameCadenceDiagnostics = false;
    bool hasHitchDiagnostics = false;
    bool hasMovementPacketCompaction = false;
    bool hasAudioDiagnostics = false;
    bool hasOverwritePcConfig = false;
    bool hasExclusiveAction = false;
    bool editsInstalledConfig = false;
    const auto setExclusiveAction = [&](RuntimeLaunchAction action,
                                        std::string_view name) {
        if (hasExclusiveAction) {
            throw std::runtime_error(
                "runtime actions are mutually exclusive: " +
                std::string(name));
        }
        options.action = action;
        hasExclusiveAction = true;
    };
    const auto addConfigOverride = [&](std::string_view encoded) {
        const size_t separator = encoded.find('=');
        if (separator == 0 || separator == std::string_view::npos) {
            throw std::runtime_error(
                "--set-config requires one editable KEY=VALUE pair");
        }
        RuntimeLaunchOptions::PcConfigOverride override{
            std::string(encoded.substr(0, separator)),
            std::string(encoded.substr(separator + 1))};
        if (!FindPcEditableSetting(override.key)) {
            throw std::runtime_error(
                "--set-config key is not exposed by the versioned PC settings schema: " +
                override.key);
        }
        const auto duplicate = std::find_if(
            options.pcConfigOverrides.begin(),
            options.pcConfigOverrides.end(), [&](const auto& existing) {
                return existing.key == override.key;
            });
        if (duplicate != options.pcConfigOverrides.end()) {
            throw std::runtime_error(
                "--set-config key was specified more than once: " +
                override.key);
        }
        options.pcConfigOverrides.push_back(std::move(override));
    };
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index] ? argv[index] : "";
        constexpr std::string_view kPcConfigPrefix = "--pc-config=";
        constexpr std::string_view kPresetPrefix = "--preset=";
        constexpr std::string_view kInstallPresetPrefix = "--install-preset=";
        constexpr std::string_view kSetConfigPrefix = "--set-config=";
        constexpr std::string_view kModsConfigPrefix = "--mods-config=";
        constexpr std::string_view kUserDataRootPrefix = "--user-data-root=";
        if (argument == "--help" || argument == "-h") {
            setExclusiveAction(RuntimeLaunchAction::Help, argument);
            continue;
        }
        if (argument == "--print-paths") {
            setExclusiveAction(RuntimeLaunchAction::PrintPaths, argument);
            continue;
        }
        if (argument == "--list-presets") {
            setExclusiveAction(RuntimeLaunchAction::ListPresets, argument);
            continue;
        }
        if (argument == "--print-capabilities") {
            setExclusiveAction(RuntimeLaunchAction::PrintCapabilities,
                               argument);
            continue;
        }
        if (argument == "--validate-config") {
            setExclusiveAction(RuntimeLaunchAction::ValidateConfig,
                               argument);
            continue;
        }
        if (argument == "--inspect-config") {
            setExclusiveAction(RuntimeLaunchAction::InspectConfig, argument);
            continue;
        }
        if (argument == "--validate-mods") {
            setExclusiveAction(RuntimeLaunchAction::ValidateMods, argument);
            continue;
        }
        if (argument == "--verify-package") {
            setExclusiveAction(RuntimeLaunchAction::VerifyPackage, argument);
            continue;
        }
        if (argument == "--edit-config") {
            if (hasPcConfigPath || hasPreset) {
                throw std::runtime_error("--edit-config uses only the adjacent installed configuration");
            }
            setExclusiveAction(RuntimeLaunchAction::InstallPreset, argument);
            hasPcConfigPath = true;
            options.pcConfigExplicit = true;
            editsInstalledConfig = true;
            continue;
        }
        if (argument == "--install-preset") {
            if (hasPcConfigPath || hasPreset || index + 1 >= argc ||
                !argv[index + 1] || !argv[index + 1][0]) {
                throw std::runtime_error(
                    "--install-preset requires one packaged preset name and cannot be combined with --pc-config or --preset");
            }
            options.pcConfigPath = ResolvePresetPath(
                executableDirectory, argv[++index]);
            hasPreset = true;
            options.pcConfigExplicit = true;
            setExclusiveAction(RuntimeLaunchAction::InstallPreset, argument);
            continue;
        }
        if (argument == "--overwrite-config") {
            if (hasOverwritePcConfig) {
                throw std::runtime_error(
                    "--overwrite-config may be specified only once");
            }
            options.overwritePcConfig = true;
            hasOverwritePcConfig = true;
            continue;
        }
        if (argument == "--set-config") {
            if (index + 1 >= argc || !argv[index + 1]) {
                throw std::runtime_error(
                    "--set-config requires one editable KEY=VALUE pair");
            }
            addConfigOverride(argv[++index]);
            continue;
        }
        if (argument == "--input-diagnostics") {
            if (hasInputDiagnostics) {
                throw std::runtime_error(
                    "--input-diagnostics may be specified only once");
            }
            options.inputDiagnostics = true;
            hasInputDiagnostics = true;
            continue;
        }
        if (argument == "--frame-cadence-diagnostics") {
            if (hasFrameCadenceDiagnostics) {
                throw std::runtime_error(
                    "--frame-cadence-diagnostics may be specified only once");
            }
            options.frameCadenceDiagnostics = true;
            hasFrameCadenceDiagnostics = true;
            continue;
        }
        if (argument == "--experimental-one-vblank") {
            if (options.oneVblankExperiment || options.immediateDeadlineExperiment) {
                throw std::runtime_error("--experimental-one-vblank may be specified only once");
            }
            options.oneVblankExperiment = true;
            continue;
        }
        if (argument == "--experimental-immediate-deadline") {
            if (options.oneVblankExperiment || options.immediateDeadlineExperiment) {
                throw std::runtime_error("render deadline experiments are mutually exclusive and may appear only once");
            }
            options.immediateDeadlineExperiment = true;
            continue;
        }
        if (argument == "--experimental-movement-packet-compaction" ||
            argument == "--no-movement-packet-compaction") {
            // Default on since V367; the old opt-in spelling stays accepted.
            if (hasMovementPacketCompaction) {
                throw std::runtime_error("movement packet compaction may be specified only once");
            }
            hasMovementPacketCompaction = true;
            options.movementPacketCompaction = argument != "--no-movement-packet-compaction";
            continue;
        }
        if (argument == "--hitch-diagnostics") {
            if (hasHitchDiagnostics) {
                throw std::runtime_error(
                    "--hitch-diagnostics may be specified only once");
            }
            options.hitchDiagnostics = true;
            hasHitchDiagnostics = true;
            continue;
        }
        if (argument == "--audio-diagnostics") {
            if (hasAudioDiagnostics) {
                throw std::runtime_error(
                    "--audio-diagnostics may be specified only once");
            }
            options.audioDiagnostics = true;
            hasAudioDiagnostics = true;
            continue;
        }
        if (argument == "--pc-config") {
            if (hasPcConfigPath || hasPreset || index + 1 >= argc || !argv[index + 1] ||
                !argv[index + 1][0]) {
                throw std::runtime_error(
                    "--pc-config requires one path and cannot be combined with --preset");
            }
            options.pcConfigPath = argumentPath(argv[++index]);
            hasPcConfigPath = true;
            options.pcConfigExplicit = true;
            continue;
        }
        if (argument == "--preset") {
            if (hasPcConfigPath || hasPreset || index + 1 >= argc ||
                !argv[index + 1] || !argv[index + 1][0]) {
                throw std::runtime_error(
                    "--preset requires one name and cannot be combined with --pc-config");
            }
            options.pcConfigPath = ResolvePresetPath(
                executableDirectory, argv[++index]);
            hasPreset = true;
            options.pcConfigExplicit = true;
            continue;
        }
        if (argument == "--mods-config") {
            if (hasModsConfigPath || index + 1 >= argc || !argv[index + 1] ||
                !argv[index + 1][0]) {
                throw std::runtime_error(
                    "--mods-config requires exactly one non-empty path");
            }
            options.modsConfigPath = argumentPath(argv[++index]);
            hasModsConfigPath = true;
            continue;
        }
        if (argument == "--user-data-root") {
            if (hasUserDataRoot || index + 1 >= argc || !argv[index + 1] ||
                !argv[index + 1][0]) {
                throw std::runtime_error(
                    "--user-data-root requires exactly one non-empty path");
            }
            options.userDataRoot = argumentPath(argv[++index]);
            hasUserDataRoot = true;
            continue;
        }
        if (argument.rfind(kModsConfigPrefix, 0) == 0) {
            if (hasModsConfigPath || argument.size() == kModsConfigPrefix.size()) {
                throw std::runtime_error(
                    "--mods-config requires exactly one non-empty path");
            }
            options.modsConfigPath =
                argumentPath(argument.substr(kModsConfigPrefix.size()));
            hasModsConfigPath = true;
            continue;
        }
        if (argument.rfind(kPcConfigPrefix, 0) == 0) {
            if (hasPcConfigPath || hasPreset ||
                argument.size() == kPcConfigPrefix.size()) {
                throw std::runtime_error(
                    "--pc-config requires one path and cannot be combined with --preset");
            }
            options.pcConfigPath =
                argumentPath(argument.substr(kPcConfigPrefix.size()));
            hasPcConfigPath = true;
            options.pcConfigExplicit = true;
            continue;
        }
        if (argument.rfind(kPresetPrefix, 0) == 0) {
            if (hasPcConfigPath || hasPreset ||
                argument.size() == kPresetPrefix.size()) {
                throw std::runtime_error(
                    "--preset requires one name and cannot be combined with --pc-config");
            }
            options.pcConfigPath = ResolvePresetPath(
                executableDirectory, argument.substr(kPresetPrefix.size()));
            hasPreset = true;
            options.pcConfigExplicit = true;
            continue;
        }
        if (argument.rfind(kInstallPresetPrefix, 0) == 0) {
            if (hasPcConfigPath || hasPreset ||
                argument.size() == kInstallPresetPrefix.size()) {
                throw std::runtime_error(
                    "--install-preset requires one packaged preset name and cannot be combined with --pc-config or --preset");
            }
            options.pcConfigPath = ResolvePresetPath(
                executableDirectory,
                argument.substr(kInstallPresetPrefix.size()));
            hasPreset = true;
            options.pcConfigExplicit = true;
            setExclusiveAction(RuntimeLaunchAction::InstallPreset, argument);
            continue;
        }
        if (argument.rfind(kSetConfigPrefix, 0) == 0) {
            addConfigOverride(argument.substr(kSetConfigPrefix.size()));
            continue;
        }
        if (argument.rfind(kUserDataRootPrefix, 0) == 0) {
            if (hasUserDataRoot ||
                argument.size() == kUserDataRootPrefix.size()) {
                throw std::runtime_error(
                    "--user-data-root requires exactly one non-empty path");
            }
            options.userDataRoot =
                argumentPath(argument.substr(kUserDataRootPrefix.size()));
            hasUserDataRoot = true;
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            throw std::runtime_error("unknown runtime option: " +
                                     std::string(argument));
        }
        if (hasXexPath || argument.empty()) {
            throw std::runtime_error(
                "expected at most one positional XEX path");
        }
        options.xexPath = argumentPath(argument);
        hasXexPath = true;
    }

    if (editsInstalledConfig && (!options.overwritePcConfig ||
                                options.pcConfigOverrides.empty() || hasXexPath)) {
        throw std::runtime_error("--edit-config requires --overwrite-config and at least one --set-config; no XEX argument is allowed");
    }
    if (options.overwritePcConfig &&
        options.action != RuntimeLaunchAction::InstallPreset) {
        throw std::runtime_error(
            "--overwrite-config is valid only with --install-preset");
    }
    if (!options.pcConfigOverrides.empty() &&
        options.action != RuntimeLaunchAction::InstallPreset) {
        throw std::runtime_error(
            "--set-config is valid only with --install-preset");
    }

    // An explicit user data root keeps its own settings file.
    options.userDataRootExplicit = hasUserDataRoot;
    if (hasUserDataRoot) {
        options.pcConfigInstallPath = options.userDataRoot / kDefaultPcConfigName;
        if (!hasPreset && (!hasPcConfigPath || editsInstalledConfig)) {
            options.pcConfigPath = options.pcConfigInstallPath;
        }
    }

    options.xexPath = AbsoluteNormalized(options.xexPath);
    options.pcConfigPath = AbsoluteNormalized(options.pcConfigPath);
    options.pcConfigInstallPath =
        AbsoluteNormalized(options.pcConfigInstallPath);
    options.modsConfigPath = AbsoluteNormalized(options.modsConfigPath);
    options.userDataRoot = AbsoluteNormalized(options.userDataRoot);
    return options;
}

std::filesystem::path RuntimeExecutableDirectory() {
    std::vector<wchar_t> path(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length) {
            throw std::runtime_error(
                "unable to locate TheDarkness.exe for PC configuration");
        }
        if (length < path.size() - 1) {
            return std::filesystem::path(path.data(), path.data() + length)
                .parent_path();
        }
        if (path.size() >= 32768) {
            throw std::runtime_error(
                "TheDarkness.exe path exceeds the supported Windows path length");
        }
        path.resize(path.size() * 2);
    }
}

const char* RuntimeLaunchHelpText() {
    return
        DARKNESS_PRODUCT_NAME "\n"
        "Usage: TheDarkness.exe [options] [path-to-default.xex]\n\n"
        "Options:\n"
        "  --pc-config <path>       Select the PC graphics/settings TOML file.\n"
        "  --preset <name>          Select one packaged preset by name.\n"
        "  --install-preset <name>  Persist one packaged preset as TheDarkness.pc.toml.\n"
        "  --edit-config            Edit the existing adjacent config (requires overrides and overwrite).\n"
        "  --set-config <key=value> Customize an exposed setting while installing a preset.\n"
        "  --overwrite-config       Permit an install/edit action to replace that config.\n"
        "  --mods-config <path>     Select the optional mod manifest.\n"
        "  --user-data-root <path>  Select the writable profile/save root.\n"
        "  --input-diagnostics      Record bounded physical/guest input transitions.\n"
        "  --frame-cadence-diagnostics\n"
        "  --experimental-one-vblank (unvalidated render deadline; no simulation-rate change)\n"
        "  --experimental-immediate-deadline (unvalidated title immediate interval; NOT verified uncapped gameplay)\n"
        "  --no-movement-packet-compaction (developer A/B: the title's same-instant movement input unmerged)\n"
        "                            Record sparse guest frame/simulation timing.\n"
        "  --hitch-diagnostics      Record bounded file, wait, GPU and Present stalls.\n"
        "  --audio-diagnostics      Record bounded callback/device timing and PCM evidence.\n"
        "  --print-paths            Print resolved paths without starting the game.\n"
        "  --list-presets           List packaged PC setting presets and exit.\n"
        "  --print-capabilities     Print implemented features and host displays.\n"
        "  --validate-config        Validate the selected PC settings and exit.\n"
        "  --inspect-config         Report declared PC settings and restart policy.\n"
        "  --validate-mods          Validate the selected loose-mod manifest and exit.\n"
        "  --verify-package         Verify packaged executable, DLLs and static data.\n"
        "  --help, -h               Show this help and exit.\n\n"
        "The original extracted game files are read-only. If no writable root is\n"
        "selected, saves and profile state remain beside the executable in\n"
        "runtime_data. Information options never initialize guest execution.\n"
        "Preset installation is serialized by the title lock and also exits\n"
        "before guest execution. TheDarknessSettings.exe provides the native\n"
        "schema-driven front end and delegates to that same validated action.\n";
}

std::vector<std::filesystem::path> RuntimePresetPaths(
    const std::filesystem::path& executableDirectory) {
    const std::filesystem::path root = executableDirectory / L"presets";
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) return {};

    std::vector<std::filesystem::path> presets;
    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto& entry = *iterator;
        std::error_code statusError;
        if (!entry.is_regular_file(statusError) || statusError ||
            entry.path().extension() != L".toml") {
            continue;
        }
        presets.push_back(entry.path().filename());
    }
    if (error) {
        throw std::runtime_error("unable to enumerate packaged PC presets");
    }
    std::sort(presets.begin(), presets.end(),
              [](const auto& left, const auto& right) {
                  return left.native() < right.native();
              });
    return presets;
}

std::vector<std::string> RuntimePcCapabilityLines() {
    return {
        "CAPABILITY_SCHEMA=1",
        "PC_EDITABLE_SETTINGS_SCHEMA=" +
            std::to_string(kPcEditableSettingsSchemaVersion),
        "PC_CONFIG_APPLICATION=startup_only",
        "PC_CONFIG_RESTART_METADATA=global_and_per_entry",
        "SETTINGS_UTILITY=TheDarknessSettings.exe_schema_frontend_v1",
        "RUNTIME_INSTANCE_POLICY=single_writer",
        "GRAPHICS_BACKEND=D3D12",
        "WINDOW_MODES=windowed,borderless",
        "ALT_ENTER=windowed_borderless_toggle",
        "MONITOR_SELECTION=0..16",
        "OUTPUT_RESOLUTIONS=native,720p,1080p,1440p,4k,custom",
        "OUTPUT_CUSTOM_MAX=8192x8192",
        "INTERNAL_RESOLUTION_SCALE=1..7_integer",
        "SPATIAL_AA=none,fxaa,fxaa_extreme,smaa",
        "SPATIAL_UPSCALING=auto,fsr1,bilinear,cas",
        "ANISOTROPIC_OVERRIDE=title,off,1x,2x,4x,8x,16x",
        "PRESENT_MODES=vsync,immediate,vrr",
        "MAX_FRAME_LATENCY=1..3",
        "HOST_FRAME_LIMIT=0..240",
        "GAMEPLAY_FOV=60..120_main_viewport_only",
        "INPUT=xinput,keyboard_mouse_optional",
        "MOUSE_ACCELERATION=0..1_bounded_per_event_default_off",
        "MOUSE_SMOOTHING=0..1_two_sample_fir_default_off",
        "CONTROLLER_HOTPLUG=xinput_live_poll",
        "CONTROLLER_BUTTON_REMAP=digital_15_to_digital_or_none",
        "CONTROLLER_PROFILE=sensitivity_medium_low_high,invert_y",
        "CONTROLLER_VIBRATION_SCALE=0..1",
        "AUDIO_MASTER_VOLUME=0..1_host_pcm_output",
        "LANGUAGES=english,german,french,spanish,italian",
        "SUBTITLES=title_owned_interactive,cutscene,casual,fighting,darkness",
        "USER_DATA=saved_games_default,portable_txt_game_folder,user_data_root_override,verified_copy_from_game_folder_once",
        "SCREENSHOT=F12_guest_output_bmp",
        "PERFORMANCE_OVERLAY=F3_guest_swap_fps_frame_time",
        "MOTION_BLUR=original,off_shader_pack",
        "MODS=ordered_loose_file_layers",
        "STATUS_EXCLUSIVE_FULLSCREEN=not_supported",
        "STATUS_GUEST_HIGH_FRAME_RATE_TIMING=deferred",
        "STATUS_ULTRAWIDE_ASPECT_EXPANSION=deferred",
        "STATUS_VIEWMODEL_FOV=deferred",
        "STATUS_TEMPORAL_AA=deferred",
        "STATUS_TEMPORAL_UPSCALING=deferred",
        "STATUS_HDR_OUTPUT=deferred",
        "STATUS_STEAM_DECK=experimental_preset_only",
    };
}

std::vector<std::string> RuntimeHostDisplayLines() {
    struct DisplayRecord {
        std::string device;
        std::string description;
        bool primary{};
        DWORD width{};
        DWORD height{};
        DWORD refresh{};
        DWORD bitsPerPixel{};
        std::set<DWORD> refreshRates;
    };

    std::vector<DisplayRecord> displays;
    for (DWORD deviceIndex = 0;; ++deviceIndex) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, deviceIndex, &device, 0)) break;
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;

        DEVMODEW current{};
        current.dmSize = sizeof(current);
        if (!EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS,
                                    &current, 0)) {
            continue;
        }

        DisplayRecord record{};
        record.device = WideToUtf8(device.DeviceName);
        record.description = WideToUtf8(device.DeviceString);
        record.primary =
            (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        record.width = current.dmPelsWidth;
        record.height = current.dmPelsHeight;
        record.refresh = current.dmDisplayFrequency;
        record.bitsPerPixel = current.dmBitsPerPel;

        for (DWORD modeIndex = 0;; ++modeIndex) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (!EnumDisplaySettingsExW(device.DeviceName, modeIndex, &mode,
                                        0)) {
                break;
            }
            if (mode.dmPelsWidth == current.dmPelsWidth &&
                mode.dmPelsHeight == current.dmPelsHeight &&
                mode.dmDisplayFrequency > 1) {
                record.refreshRates.insert(mode.dmDisplayFrequency);
            }
        }
        displays.push_back(std::move(record));
    }

    std::vector<std::string> lines;
    lines.reserve(1 + displays.size() * 6);
    lines.push_back("HOST_DISPLAY_COUNT=" + std::to_string(displays.size()));
    for (size_t index = 0; index < displays.size(); ++index) {
        const DisplayRecord& display = displays[index];
        const std::string prefix =
            "HOST_DISPLAY_" + std::to_string(index) + "_";
        lines.push_back(prefix + "DEVICE=" + display.device);
        lines.push_back(prefix + "DESCRIPTION=" + display.description);
        lines.push_back(prefix + "PRIMARY=" +
                        std::to_string(display.primary ? 1 : 0));
        lines.push_back(prefix + "CURRENT_MODE=" +
                        std::to_string(display.width) + "x" +
                        std::to_string(display.height) + "@" +
                        std::to_string(display.refresh) + "Hz_" +
                        std::to_string(display.bitsPerPixel) + "bpp");
        std::ostringstream refreshList;
        bool first = true;
        for (DWORD refresh : display.refreshRates) {
            if (!first) refreshList << ',';
            first = false;
            refreshList << refresh;
        }
        lines.push_back(prefix + "CURRENT_RESOLUTION_REFRESH_HZ=" +
                        refreshList.str());
    }
    return lines;
}

RuntimePcConfigValidation ValidateRuntimePcConfig(
    const std::filesystem::path& path, bool requireFile, const RuntimePcConfigSnapshot* snapshot) {
    RuntimePcConfigValidation result{};
    result.exists = ConfigPresent(path, snapshot);
    if (!result.exists) {
        if (requireFile) result.errors.push_back("configuration file not found");
        result.valid = result.errors.empty();
        return result;
    }

    toml::table config;
    try {
        config = ParseConfig(path, snapshot);
    } catch (const toml::parse_error& error) {
        result.errors.push_back(
            "TOML parse error: " +
            SingleLine(std::string(error.description())));
        result.valid = false;
        return result;
    }

    const auto addTypeError = [&](std::string_view name,
                                  std::string_view expected) {
        result.errors.push_back(std::string(name) + " must be " +
                                std::string(expected));
    };
    const auto validateInteger = [&](std::string_view name, int64_t minimum,
                                     int64_t maximum, bool required = false) {
        const toml::node* node = FindConfigNode(config, name);
        if (!node) {
            if (required) result.errors.push_back(std::string(name) + " is required");
            return;
        }
        const auto value = node->value<int64_t>();
        if (!value) {
            addTypeError(name, "an integer");
        } else if (*value < minimum || *value > maximum) {
            result.errors.push_back(std::string(name) + " is outside " +
                                    std::to_string(minimum) + ".." +
                                    std::to_string(maximum));
        }
    };
    const auto validateNumber = [&](std::string_view name, double minimum,
                                    double maximum) {
        const toml::node* node = FindConfigNode(config, name);
        if (!node) return;
        std::optional<double> value = node->value<double>();
        if (!value) {
            if (const auto integer = node->value<int64_t>()) {
                value = static_cast<double>(*integer);
            }
        }
        if (!value) {
            addTypeError(name, "a number");
        } else if (!std::isfinite(*value) || *value < minimum ||
                   *value > maximum) {
            std::ostringstream range;
            range << name << " is outside " << minimum << ".." << maximum;
            result.errors.push_back(range.str());
        }
    };
    const auto validateBoolean = [&](std::string_view name) {
        const toml::node* node = FindConfigNode(config, name);
        if (node && !node->is_boolean()) addTypeError(name, "a boolean");
    };
    const auto validateString = [&](std::string_view name, bool allowEmpty) {
        const toml::node* node = FindConfigNode(config, name);
        if (!node) return;
        const auto value = node->value<std::string>();
        if (!value) {
            addTypeError(name, "a string");
        } else if (!allowEmpty && value->empty()) {
            result.errors.push_back(std::string(name) + " must not be empty");
        }
    };
    const auto validateAllowed = [&](std::string_view name,
                                     std::initializer_list<std::string_view> allowed) {
        const toml::node* node = FindConfigNode(config, name);
        if (!node) return;
        const auto value = node->value<std::string>();
        if (!value) {
            addTypeError(name, "a string");
            return;
        }
        if (std::find(allowed.begin(), allowed.end(), *value) == allowed.end()) {
            result.errors.push_back(std::string(name) + " has unsupported value '" +
                                    *value + "'");
        }
    };

    validateInteger("pc_config_version", 1, 1, true);
    validateInteger("window_width", 0, 8192);
    validateInteger("window_height", 0, 8192);
    validateBoolean("fullscreen");
    validateInteger("draw_resolution_scale_threshold", 0, 16384);
    validateBoolean("native_resolve_region_tracking");
    validateBoolean("native_resolve_region_sampling");
    const auto* nativeTracking = FindConfigNode(config, "native_resolve_region_tracking");
    const auto* nativeSampling = FindConfigNode(config, "native_resolve_region_sampling");
    if (nativeSampling && nativeSampling->value<bool>().value_or(false) &&
        !(nativeTracking && nativeTracking->value<bool>().value_or(false))) {
        result.errors.push_back("native_resolve_region_sampling requires native_resolve_region_tracking");
    }
    validateString("draw_resolution_scale_native_grid_rules", true);
    if (const auto* node = FindConfigNode(config, "draw_resolution_scale_native_grid_rules")) {
        if (const auto text = node->value<std::string>()) {
            rex::graphics::render_target::native_shader_scale_policy::Rules rules;
            if (!rex::graphics::render_target::native_shader_scale_policy::Parse(*text, rules)) {
                result.errors.push_back("draw_resolution_scale_native_grid_rules has invalid or duplicate pass annotations");
            }
        }
    }
    // Empty = the built-in patch of the composite shaders (no pack file).
    validateString("graphics.motion_blur_off_shader_pack", true);
    validateBoolean("present.letterbox");
    validateBoolean("present.allow_overscan_cutoff");
    validateInteger("present.safe_area_x", 0, 100);
    validateInteger("present.safe_area_y", 0, 100);
    validateBoolean("diagnostics.camera_state");
    validateInteger("input.bindings_revision", 1, kRuntimeKeyBindingsRevision);

    // The settings utility and runtime consume this same versioned schema.
    // The runtime stays authoritative for type/range/choice validation.
    for (const PcEditableSettingSpec& setting : PcEditableSettingsSchema()) {
        switch (setting.editor) {
            case PcSettingEditorKind::Boolean:
                validateBoolean(setting.key);
                break;
            case PcSettingEditorKind::Integer:
            case PcSettingEditorKind::IntegerChoice:
                validateInteger(setting.key,
                                static_cast<int64_t>(setting.minimum),
                                static_cast<int64_t>(setting.maximum));
                break;
            case PcSettingEditorKind::Number:
                validateNumber(setting.key, setting.minimum, setting.maximum);
                break;
            case PcSettingEditorKind::Choice: {
                const toml::node* node = FindConfigNode(config, setting.key);
                if (!node) break;
                const auto value = node->value<std::string>();
                if (!value) {
                    addTypeError(setting.key, "a string");
                    break;
                }
                const bool allowed = std::any_of(
                    setting.choices.begin(), setting.choices.end(),
                    [&](const PcSettingChoice& choice) {
                        return choice.value == *value;
                    }) || IsExtraChoiceValue(setting.key, *value);
                if (!allowed) {
                    result.errors.push_back(
                        std::string(setting.key) +
                        " has unsupported value '" + *value + "'");
                }
                break;
            }
            case PcSettingEditorKind::Resolution: {
                const toml::node* node = FindConfigNode(config, setting.key);
                if (!node) break;
                const auto value = node->value<std::string>();
                if (!value) {
                    addTypeError(setting.key, "a string");
                } else if (!IsValidOutputResolution(*value)) {
                    result.errors.push_back(
                        std::string(setting.key) +
                        " must be native, a supported preset, or WIDTHxHEIGHT up to 8192");
                }
                break;
            }
            case PcSettingEditorKind::Key: {
                // Unknown names never press anything (hand-edited files keep
                // loading); the settings writer only stores known names.
                const toml::node* node = FindConfigNode(config, setting.key);
                if (node && !node->value<std::string>()) addTypeError(setting.key, "a string");
                break;
            }
        }
    }

    constexpr std::array<std::string_view, 21> bindingNames = {
        "a", "b", "x", "y", "left_trigger", "right_trigger",
        "left_shoulder", "right_shoulder", "lstick_up", "lstick_down",
        "lstick_left", "lstick_right", "lstick_press", "rstick_press",
        "dpad_up", "dpad_down", "dpad_left", "dpad_right", "back",
        "start", "guide"};
    for (std::string_view binding : bindingNames) {
        validateString("input.bind." + std::string(binding), true);
    }
    for (std::string_view button : kRuntimeInputButtonNames) {
        validateAllowed(
            "input.controller_bind." + std::string(button),
            {"dpad_up", "dpad_down", "dpad_left", "dpad_right", "start",
             "back", "left_stick", "right_stick", "left_shoulder",
             "right_shoulder", "guide", "a", "b", "x", "y", "none"});
    }

    std::set<std::string> knownLeaves = {
        "pc_config_version", "window_width",
        "window_height", "fullscreen", "draw_resolution_scale_threshold",
        "draw_resolution_scale_native_grid_rules",
        "native_resolve_region_tracking", "native_resolve_region_sampling",
        "graphics.motion_blur_off_shader_pack", "present.letterbox",
        "present.allow_overscan_cutoff", "present.safe_area_x",
        "present.safe_area_y",
        "input.keyboard_mouse_user_index", "input.mouse_sensitivity",
        "input.mouse_acceleration", "input.mouse_smoothing",
        "input.mouse_invert_y",
        "audio.master_volume", "general.language",
        "input.bind.a", "input.bind.b",
        "input.bind.x", "input.bind.y", "input.bind.left_trigger",
        "input.bind.right_trigger", "input.bind.left_shoulder",
        "input.bind.right_shoulder", "input.bind.lstick_up",
        "input.bind.lstick_down", "input.bind.lstick_left",
        "input.bind.lstick_right", "input.bind.lstick_press",
        "input.bind.rstick_press", "input.bind.dpad_up",
        "input.bind.dpad_down", "input.bind.dpad_left",
        "input.bind.dpad_right", "input.bind.back", "input.bind.start",
        "input.bind.guide", "diagnostics.camera_state", "input.bindings_revision",
        // Pre-V376 field of view key (never changed the view); ignored.
        "camera.gameplay_fov"};
    for (const PcEditableSettingSpec& setting : PcEditableSettingsSchema()) {
        knownLeaves.insert(std::string(setting.key));
    }
    for (std::string_view button : kRuntimeInputButtonNames) {
        knownLeaves.insert("input.controller_bind." + std::string(button));
    }
    std::vector<std::string> leaves;
    std::vector<std::string> unsupportedTypes;
    CollectConfigLeaves(config, {}, leaves, unsupportedTypes);
    for (const std::string& pathName : unsupportedTypes) {
        result.errors.push_back(pathName + " uses an unsupported TOML value type");
    }
    for (const std::string& pathName : leaves) {
        if (!knownLeaves.count(pathName)) {
            result.warnings.push_back(
                pathName + " is not in the packaged schema; ReXGlue will validate it at startup");
        }
    }
    result.valid = result.errors.empty();
    return result;
}

std::vector<std::string> RuntimePcConfigValidationLines(
    const std::filesystem::path& path,
    const RuntimePcConfigValidation& validation) {
    std::vector<std::string> lines;
    lines.push_back("PC_CONFIG_PATH=" + path.u8string());
    lines.push_back("PC_CONFIG_EXISTS=" +
                    std::to_string(validation.exists ? 1 : 0));
    lines.push_back("PC_CONFIG_VALID=" +
                    std::to_string(validation.valid ? 1 : 0));
    lines.push_back("PC_CONFIG_ERROR_COUNT=" +
                    std::to_string(validation.errors.size()));
    for (const std::string& error : validation.errors) {
        lines.push_back("PC_CONFIG_ERROR=" + SingleLine(error));
    }
    lines.push_back("PC_CONFIG_WARNING_COUNT=" +
                    std::to_string(validation.warnings.size()));
    for (const std::string& warning : validation.warnings) {
        lines.push_back("PC_CONFIG_WARNING=" + SingleLine(warning));
    }
    return lines;
}

std::map<std::string, std::string> RuntimePcConfigSettingValues(
    const std::filesystem::path& path) {
    std::map<std::string, std::string> values;
    toml::table config = toml::parse_file(path.u8string());
    // A 0.9.0 configuration shows the keys its next start or save gives it.
    UpgradeKeyBindings(config);
    for (const PcEditableSettingSpec& setting : PcEditableSettingsSchema()) {
        const toml::node* node = FindConfigNode(config, setting.key);
        if (!node) continue;
        std::string value;
        if (node->is_string()) {
            value = node->value<std::string>().value_or(std::string{});
        } else if (node->is_boolean()) {
            value = node->value<bool>().value_or(false) ? "true" : "false";
        } else if (node->is_integer()) {
            value = std::to_string(node->value<int64_t>().value_or(0));
        } else if (node->is_floating_point()) {
            std::ostringstream formatted;
            formatted.imbue(std::locale::classic());
            formatted << std::setprecision(std::numeric_limits<double>::max_digits10)
                      << node->value<double>().value_or(0.0);
            value = formatted.str();
        } else {
            continue;
        }
        values[std::string(setting.key)] = value;
    }
    return values;
}

RuntimePcScaleTarget RuntimePcScaleTargetFromContents(const std::string& contents,
                                                      const std::string& sourceName) {
    const toml::table config = toml::parse(contents, sourceName);
    RuntimePcScaleTarget target;
    target.resolutionScale = config["resolution_scale"].value<int64_t>().value_or(1);
    target.outputResolution = config["output_resolution"].value<std::string>().value_or("native");
    target.monitor = config["monitor"].value<int64_t>().value_or(0);
    target.frameRate = config["display"]["frame_rate"].value<std::string>().value_or("refresh");
    target.frameLimit = uint32_t(
        std::clamp<int64_t>(config["display"]["frame_limit"].value<int64_t>().value_or(0), 0, 1000));
    return target;
}

std::string RuntimePcConfigWithAutomaticScale(const std::string& contents,
                                              const std::string& sourceName,
                                              uint32_t automaticScale, bool* changed) {
    if (changed) *changed = false;
    toml::table config = toml::parse(contents, sourceName);
    const auto scale = config["resolution_scale"].value<int64_t>();
    if (!scale || *scale != 0 || !automaticScale) return contents;
    config.insert_or_assign("resolution_scale", int64_t(automaticScale));
    std::ostringstream serialized;
    serialized.imbue(std::locale::classic());
    serialized << PcConfigFormatter(config) << '\n';
    if (changed) *changed = true;
    return serialized.str();
}

std::string RuntimePcConfigWithShelvedFeatures(const std::string& contents,
                                               const std::string& sourceName,
                                               bool temporalAaOffered, std::string* shelved) {
    if (shelved) shelved->clear();
    if (temporalAaOffered || contents.empty()) return contents;
    toml::table config = toml::parse(contents, sourceName);
    toml::table* graphics = config["graphics"].as_table();
    const auto value =
        graphics ? (*graphics)["temporal_aa"].value<std::string>() : std::optional<std::string>();
    if (!value || *value == "off") return contents;
    graphics->insert_or_assign("temporal_aa", std::string("off"));
    std::ostringstream serialized;
    serialized.imbue(std::locale::classic());
    serialized << PcConfigFormatter(config) << '\n';
    if (shelved) *shelved = *value;
    return serialized.str();
}

namespace {
bool SameKeyName(std::string_view value, std::string_view name) {
    return value.size() == name.size() &&
           std::equal(value.begin(), value.end(), name.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

// runtime_pc_settings.h: 0.9.0's A = Space / Y = E become E / Space unless
// the player chose otherwise.
RuntimeKeyBindingsUpgrade UpgradeKeyBindings(toml::table& config) {
    RuntimeKeyBindingsUpgrade upgrade;
    toml::table* input = config["input"].as_table();
    toml::table* bind = input ? (*input)["bind"].as_table() : nullptr;
    const auto revision = input ? (*input)["bindings_revision"].value<int64_t>() : std::nullopt;
    if (revision && *revision >= kRuntimeKeyBindingsRevision) {
        upgrade.outcome = "current";
        return upgrade;
    }
    if (!bind) {
        // Nothing saved: the new defaults apply. Left unmarked, so a
        // configuration installed from a preset stays byte-identical to it.
        upgrade.outcome = "no_bindings";
        return upgrade;
    }
    const auto keyIs = [&](std::string_view action, std::string_view old) {
        const toml::node* node = bind->get(action);
        if (!node) return true;  // absent: the old default applied
        const auto value = node->value<std::string>();
        return value && SameKeyName(*value, old);
    };
    bool conflict = false;
    for (const auto& [action, node] : *bind) {
        if (action.str() == "a" || action.str() == "y") continue;
        const auto value = node.value<std::string>();
        if (value && (SameKeyName(*value, "Space") || SameKeyName(*value, "E"))) conflict = true;
    }
    if (keyIs("a", "Space") && keyIs("y", "E") && !conflict) {
        bind->insert_or_assign("a", std::string("E"));
        bind->insert_or_assign("y", std::string("Space"));
        upgrade.swapped = true;
        upgrade.outcome = "swapped";
    } else {
        // The player's own choice: an absent A or Y keeps its old default.
        if (!bind->contains("a")) bind->insert("a", std::string("Space"));
        if (!bind->contains("y")) bind->insert("y", std::string("E"));
        upgrade.outcome = conflict ? "conflict" : "custom";
    }
    input->insert_or_assign("bindings_revision", kRuntimeKeyBindingsRevision);
    upgrade.changed = true;
    return upgrade;
}

// A configuration saved now with bindings: absent keys mean the new defaults.
void MarkKeyBindingsCurrent(toml::table& config) {
    toml::table* input = config["input"].as_table();
    if (input && (*input)["bind"].as_table() && !input->contains("bindings_revision")) {
        input->insert("bindings_revision", kRuntimeKeyBindingsRevision);
    }
}
}  // namespace

std::string RuntimePcConfigWithKeyBindingsUpgrade(const std::string& contents,
                                                  const std::string& sourceName,
                                                  RuntimeKeyBindingsUpgrade* result) {
    toml::table config = toml::parse(contents, sourceName);
    const RuntimeKeyBindingsUpgrade upgrade = UpgradeKeyBindings(config);
    if (result) *result = upgrade;
    if (!upgrade.changed) return contents;
    std::ostringstream serialized;
    serialized.imbue(std::locale::classic());
    serialized << PcConfigFormatter(config) << '\n';
    return serialized.str();
}

std::string RuntimePcConfigWithProgramCacheSource(const std::string& contents,
                                                  const std::filesystem::path& programCache) {
    if (programCache.empty()) return contents;
    // A TOML basic string; top-level keys precede every table.
    const auto path = programCache.u8string();  // UTF-8
    std::string line = "gpu_program_cache_source = \"";
    for (const auto c : path) {
        if (c == '\\' || c == '"') line.push_back('\\');
        line.push_back(char(c));
    }
    line += "\"\n";
    const bool bom = contents.rfind("\xEF\xBB\xBF", 0) == 0;
    return bom ? contents.substr(0, 3) + line + contents.substr(3) : line + contents;
}

std::string RuntimePcConfigWithTitleScaleRequirements(const std::string& contents,
                                                      const std::string& sourceName,
                                                      bool* changed) {
    if (changed) *changed = false;
    toml::table config = toml::parse(contents, sourceName);
    const auto scale = config["resolution_scale"].value<int64_t>();
    if (!scale || *scale <= 1) return contents;
    const bool hasThreshold = config.contains("draw_resolution_scale_threshold");
    const auto rules = config["draw_resolution_scale_native_grid_rules"].value<std::string>();
    const bool legacyRules = rules && (*rules == kLegacyTitleNativeGridRules ||
                                       *rules == kPreviousTitleNativeGridRules ||
                                       *rules == k091TitleNativeGridRules ||
                                       *rules == k094TitleNativeGridRules);
    const bool hasRules = config.contains("draw_resolution_scale_native_grid_rules") && !legacyRules;
    const bool hasTracking = config.contains("native_resolve_region_tracking");
    if (hasThreshold && hasRules && hasTracking) return contents;
    if (!hasThreshold) config.insert("draw_resolution_scale_threshold", kTitleScaleThreshold);
    if (!hasRules) {
        config.insert_or_assign("draw_resolution_scale_native_grid_rules",
                                std::string(kTitleNativeGridRules));
    }
    if (!hasTracking) config.insert("native_resolve_region_tracking", true);
    std::ostringstream serialized;
    serialized.imbue(std::locale::classic());
    serialized << PcConfigFormatter(config) << '\n';
    if (changed) *changed = true;
    return serialized.str();
}

std::vector<std::string> RuntimePcConfigInspectionLines(
    const std::filesystem::path& path,
    const RuntimePcConfigValidation& validation) {
    std::vector<std::string> lines = {
        "PC_CONFIG_INSPECTION_SCHEMA=1",
        "PC_CONFIG_APPLICATION=startup_only",
        "PC_CONFIG_RESTART_REQUIRED=1",
        "PC_CONFIG_VALUE_ENCODING=percent_utf8",
    };
    const auto validationLines =
        RuntimePcConfigValidationLines(path, validation);
    lines.insert(lines.end(), validationLines.begin(), validationLines.end());
    if (!validation.valid) {
        lines.push_back("PC_CONFIG_ENTRY_COUNT=0");
        return lines;
    }

    toml::table config = toml::parse_file(path.u8string());
    // A 0.9.0 configuration with saved keys shows the keys its next start or
    // save gives it (the launcher reads its values from here).
    UpgradeKeyBindings(config);
    std::vector<std::string> leaves;
    std::vector<std::string> unsupportedTypes;
    CollectConfigLeaves(config, {}, leaves, unsupportedTypes);
    std::sort(leaves.begin(), leaves.end());
    lines.push_back("PC_CONFIG_ENTRY_COUNT=" + std::to_string(leaves.size()));
    for (size_t index = 0; index < leaves.size(); ++index) {
        const std::string& key = leaves[index];
        const toml::node* node = FindConfigNode(config, key);
        std::string type = "unsupported";
        std::string value;
        if (node) {
            if (node->is_string()) {
                const auto stringValue = node->value<std::string>();
                type = "string";
                value = stringValue.value_or(std::string{});
            } else if (node->is_boolean()) {
                const auto booleanValue = node->value<bool>();
                type = "boolean";
                value = booleanValue.value_or(false) ? "true" : "false";
            } else if (node->is_integer()) {
                const auto integerValue = node->value<int64_t>();
                type = "integer";
                value = std::to_string(integerValue.value_or(0));
            } else if (node->is_floating_point()) {
                const auto floatValue = node->value<double>();
                type = "float";
                std::ostringstream formatted;
                formatted.imbue(std::locale::classic());
                formatted << std::setprecision(
                    std::numeric_limits<double>::max_digits10)
                          << floatValue.value_or(0.0);
                value = formatted.str();
            }
        }
        const std::string prefix =
            "PC_CONFIG_ENTRY_" + std::to_string(index) + "_";
        lines.push_back(prefix + "KEY=" + PercentEncodeMachineField(key));
        lines.push_back(prefix + "TYPE=" + type);
        lines.push_back(prefix + "VALUE=" +
                        PercentEncodeMachineField(value));
        // Every packaged setting is consumed before subsystem/guest startup;
        // front ends need per-entry metadata so they never imply a live edit.
        lines.push_back(prefix + "RESTART_REQUIRED=1");
    }
    return lines;
}

double RuntimeInputVibrationScaleFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot) {
    if (!ConfigPresent(path, snapshot)) return 1.0;
    const toml::table config = ParseConfig(path, snapshot);
    const toml::node* node = FindConfigNode(config, "input.vibration_scale");
    if (!node) return 1.0;
    std::optional<double> value = node->value<double>();
    if (!value) {
        if (const auto integer = node->value<int64_t>()) {
            value = static_cast<double>(*integer);
        }
    }
    if (!value || !std::isfinite(*value) || *value < 0.0 || *value > 1.0) {
        throw std::runtime_error("input.vibration_scale must be within 0..1");
    }
    return *value;
}

RuntimeInputButtonMap RuntimeInputButtonMapFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot) {
    RuntimeInputButtonMap result;
    if (!ConfigPresent(path, snapshot)) return result;
    const toml::table config = ParseConfig(path, snapshot);
    for (size_t guest = 0; guest < kRuntimeInputButtonNames.size(); ++guest) {
        const std::string key = "input.controller_bind." +
                                std::string(kRuntimeInputButtonNames[guest]);
        const toml::node* node = FindConfigNode(config, key);
        if (!node) continue;
        const auto value = node->value<std::string>();
        RuntimeInputButton source{};
        if (!value || !ParseRuntimeInputButtonName(value.value_or(""), source)) {
            throw std::runtime_error(key + " has unsupported button name");
        }
        result.sourceForGuest[guest] = static_cast<uint8_t>(source);
    }
    return result;
}

RuntimeControllerProfile RuntimeControllerProfileFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot) {
    RuntimeControllerProfile result;
    if (!ConfigPresent(path, snapshot)) return result;
    const toml::table config = ParseConfig(path, snapshot);
    if (const toml::node* node =
            FindConfigNode(config, "input.controller_sensitivity")) {
        const auto value = node->value<std::string>();
        if (!value) {
            throw std::runtime_error(
                "input.controller_sensitivity must be medium, low, or high");
        }
        if (*value == "medium") result.sensitivity = 0;
        else if (*value == "low") result.sensitivity = 1;
        else if (*value == "high") result.sensitivity = 2;
        else {
            throw std::runtime_error(
                "input.controller_sensitivity must be medium, low, or high");
        }
    }
    if (const toml::node* node =
            FindConfigNode(config, "input.controller_invert_y")) {
        if (!node->is_boolean()) {
            throw std::runtime_error(
                "input.controller_invert_y must be a boolean");
        }
        const auto value = node->value<bool>();
        if (!value) {
            throw std::runtime_error(
                "input.controller_invert_y must be a boolean");
        }
        result.invertY = *value;
    }
    return result;
}

double RuntimeAudioMasterVolumeFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot) {
    if (!ConfigPresent(path, snapshot)) return 1.0;
    const toml::table config = ParseConfig(path, snapshot);
    const toml::node* node = FindConfigNode(config, "audio.master_volume");
    if (!node) return 1.0;
    std::optional<double> value = node->value<double>();
    if (!value) {
        if (const auto integer = node->value<int64_t>()) {
            value = static_cast<double>(*integer);
        }
    }
    if (!value || !std::isfinite(*value) || *value < 0.0 || *value > 1.0) {
        throw std::runtime_error("audio.master_volume must be within 0..1");
    }
    return *value;
}

msaa_mode::Policy RuntimeMsaaModeFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot) {
    msaa_mode::Policy policy = msaa_mode::Policy::kAlways4x;
    if (!ConfigPresent(path, snapshot)) return policy;
    const toml::table config = ParseConfig(path, snapshot);
    const toml::node* node = FindConfigNode(config, "graphics.msaa_mode");
    if (!node) return policy;
    const auto value = node->value<std::string>();
    if (!value || !msaa_mode::ParsePolicy(*value, policy)) {
        throw std::runtime_error("graphics.msaa_mode must be 4x, 2x, or auto");
    }
    return policy;
}

bool RuntimeFrameRateUsesHostPacingFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot) {
    if (!ConfigPresent(path, snapshot)) return false;
    const toml::table config = ParseConfig(path, snapshot);
    const toml::node* node = FindConfigNode(config, "display.frame_rate");
    if (!node) return false;
    const auto value = node->value<std::string>();
    if (!value) throw std::runtime_error("display.frame_rate must be a string");
    if (*value == "original") return false;
    // 60, half_refresh, refresh, custom, uncapped and whole-number caps.
    if (rex::ui::IsFrameRateValue(*value)) return true;
    throw std::runtime_error("display.frame_rate has unsupported value '" + *value + "'");
}

uint32_t RuntimeXboxLanguageForWindowsLanguage(uint16_t windowsLanguage) {
    // An Xbox title uses the dashboard language when it ships it and English
    // otherwise; the extracted release ships these five.
    switch (PRIMARYLANGID(windowsLanguage)) {
        case LANG_GERMAN: return 3;
        case LANG_FRENCH: return 4;
        case LANG_SPANISH: return 5;
        case LANG_ITALIAN: return 6;
        default: return 1;
    }
}

bool RuntimeWidescreenFromPcConfig(const std::filesystem::path& path,
                                   const RuntimePcConfigSnapshot* snapshot) {
    if (!ConfigPresent(path, snapshot)) return false;
    const toml::table config = ParseConfig(path, snapshot);
    const toml::node* node = FindConfigNode(config, "display.widescreen");
    if (!node) return false;
    const auto value = node->value<bool>();
    if (!value) throw std::runtime_error("display.widescreen must be a boolean");
    return *value;
}

uint32_t RuntimeXboxLanguageFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot) {
    if (!ConfigPresent(path, snapshot)) return 1;
    const toml::table config = ParseConfig(path, snapshot);
    const toml::node* node = FindConfigNode(config, "general.language");
    if (!node) return 1;
    const auto value = node->value<std::string>();
    if (!value) throw std::runtime_error("general.language must be a string");
    if (*value == "auto") return RuntimeXboxLanguageForWindowsLanguage(GetUserDefaultUILanguage());
    if (*value == "english") return 1;
    if (*value == "german") return 3;
    if (*value == "french") return 4;
    if (*value == "spanish") return 5;
    if (*value == "italian") return 6;
    // The game has no Arabic: English speech, text from an Arabic language
    // pack when one is installed.
    if (*value == "arabic") return 1;
    throw std::runtime_error("general.language has unsupported value '" +
                             *value + "'");
}

std::string RuntimeLanguagePackFromPcConfig(
    const std::filesystem::path& path, const RuntimePcConfigSnapshot* snapshot) {
    if (!ConfigPresent(path, snapshot)) return {};
    const toml::table config = ParseConfig(path, snapshot);
    const toml::node* node = FindConfigNode(config, "general.language");
    if (!node) return {};
    const auto value = node->value<std::string>();
    if (!value) throw std::runtime_error("general.language must be a string");
    return *value == "arabic" ? *value : std::string();
}

RuntimePcConfigInstallResult InstallRuntimePcPreset(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath, bool overwrite) {
    const std::filesystem::path source = AbsoluteNormalized(sourcePath);
    const std::filesystem::path destination =
        AbsoluteNormalized(destinationPath);

    const RuntimePcConfigValidation validation =
        ValidateRuntimePcConfig(source, true);
    if (!validation.valid) {
        throw std::runtime_error(
            "source preset is invalid: " +
            (validation.errors.empty() ? std::string("unknown validation error")
                                       : validation.errors.front()));
    }

    std::error_code filesystemError;
    if (std::filesystem::equivalent(source, destination, filesystemError) &&
        !filesystemError) {
        throw std::runtime_error(
            "source preset and destination configuration are the same file");
    }

    const DWORD sourceAttributes = GetFileAttributesW(source.c_str());
    if (sourceAttributes == INVALID_FILE_ATTRIBUTES ||
        (sourceAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (sourceAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error(
            "source preset must be a regular non-reparse file");
    }

    const std::filesystem::path parent = destination.parent_path();
    // The per-user folder (Saved Games) exists only once something is saved.
    std::filesystem::create_directories(parent, filesystemError);
    const DWORD parentAttributes = GetFileAttributesW(parent.c_str());
    if (parentAttributes == INVALID_FILE_ATTRIBUTES ||
        !(parentAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (parentAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error(
            "configuration destination directory is unavailable or unsafe");
    }

    SetLastError(ERROR_SUCCESS);
    const DWORD destinationAttributes = GetFileAttributesW(destination.c_str());
    const DWORD destinationAttributeError = GetLastError();
    if (destinationAttributes == INVALID_FILE_ATTRIBUTES &&
        destinationAttributeError != ERROR_FILE_NOT_FOUND &&
        destinationAttributeError != ERROR_PATH_NOT_FOUND) {
        throw std::runtime_error(
            "unable to inspect persistent configuration destination");
    }
    const bool destinationExists =
        destinationAttributes != INVALID_FILE_ATTRIBUTES;
    if (destinationExists &&
        ((destinationAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
         (destinationAttributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
        throw std::runtime_error(
            "configuration destination must be a regular non-reparse file");
    }
    if (destinationExists && !overwrite) {
        throw std::runtime_error(
            "configuration already exists; use --overwrite-config to replace it");
    }

    std::ifstream input(source, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("unable to open source preset");
    const std::streamoff size = input.tellg();
    constexpr std::streamoff kMaximumPresetBytes = 1024 * 1024;
    if (size <= 0 || size > kMaximumPresetBytes) {
        throw std::runtime_error("source preset size is invalid");
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(size));
    if (!input) throw std::runtime_error("unable to read source preset");

    HANDLE temporary = INVALID_HANDLE_VALUE;
    std::filesystem::path temporaryPath;
    for (uint32_t attempt = 0; attempt < 16; ++attempt) {
        temporaryPath = destination;
        temporaryPath += L".tmp." + std::to_wstring(GetCurrentProcessId()) +
                         L"." + std::to_wstring(GetCurrentThreadId()) + L"." +
                         std::to_wstring(attempt);
        temporary = CreateFileW(
            temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (temporary != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (temporary == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "unable to create temporary configuration file");
    }

    bool committed = false;
    try {
        size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
                bytes.size() - offset, static_cast<size_t>(MAXDWORD)));
            DWORD written = 0;
            if (!WriteFile(temporary, bytes.data() + offset, chunk, &written,
                           nullptr) ||
                written != chunk) {
                throw std::runtime_error(
                    "unable to write temporary configuration file");
            }
            offset += written;
        }
        if (!FlushFileBuffers(temporary)) {
            throw std::runtime_error(
                "unable to flush temporary configuration file");
        }
        CloseHandle(temporary);
        temporary = INVALID_HANDLE_VALUE;

        // Validate the exact flushed snapshot that will become persistent. This
        // prevents a source-file change between the earlier preflight and read
        // from installing bytes that were never schema/range checked.
        const RuntimePcConfigValidation snapshotValidation =
            ValidateRuntimePcConfig(temporaryPath, true);
        if (!snapshotValidation.valid) {
            throw std::runtime_error(
                "configuration snapshot is invalid: " +
                (snapshotValidation.errors.empty()
                     ? std::string("unknown validation error")
                     : snapshotValidation.errors.front()));
        }

        DWORD moveFlags = MOVEFILE_WRITE_THROUGH;
        if (overwrite) moveFlags |= MOVEFILE_REPLACE_EXISTING;
        if (!MoveFileExW(temporaryPath.c_str(), destination.c_str(),
                         moveFlags)) {
            throw std::runtime_error(
                "unable to atomically install persistent configuration");
        }
        committed = true;
    } catch (...) {
        if (temporary != INVALID_HANDLE_VALUE) CloseHandle(temporary);
        DeleteFileW(temporaryPath.c_str());
        throw;
    }
    if (!committed) {
        throw std::runtime_error("persistent configuration was not installed");
    }
    return {source, destination, destinationExists, 0};
}

namespace {
// Stages serialized configuration bytes beside the destination and installs
// them through InstallRuntimePcPreset (validated, flushed, atomic).
RuntimePcConfigInstallResult InstallSerializedPcConfig(
    const std::string& bytes, const std::filesystem::path& destinationPath, bool overwrite) {
    if (bytes.empty() || bytes.size() > 1024 * 1024) {
        throw std::runtime_error(
            "customized PC configuration size is invalid");
    }
    const std::filesystem::path destination =
        AbsoluteNormalized(destinationPath);
    std::filesystem::path stagedPath;
    std::error_code directoryError;
    std::filesystem::create_directories(destination.parent_path(), directoryError);
    HANDLE staged = INVALID_HANDLE_VALUE;
    for (uint32_t attempt = 0; attempt < 16; ++attempt) {
        stagedPath = destination;
        stagedPath += L".custom." + std::to_wstring(GetCurrentProcessId()) +
                      L"." + std::to_wstring(GetCurrentThreadId()) + L"." +
                      std::to_wstring(attempt);
        staged = CreateFileW(
            stagedPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (staged != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (staged == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "unable to stage customized PC configuration");
    }

    try {
        size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
                bytes.size() - offset, static_cast<size_t>(MAXDWORD)));
            DWORD written = 0;
            if (!WriteFile(staged, bytes.data() + offset, chunk, &written,
                           nullptr) ||
                written != chunk) {
                throw std::runtime_error(
                    "unable to write customized PC configuration");
            }
            offset += written;
        }
        if (!FlushFileBuffers(staged)) {
            throw std::runtime_error(
                "unable to flush customized PC configuration");
        }
        CloseHandle(staged);
        staged = INVALID_HANDLE_VALUE;

        RuntimePcConfigInstallResult result = InstallRuntimePcPreset(
            stagedPath, destination, overwrite);
        DeleteFileW(stagedPath.c_str());
        return result;
    } catch (...) {
        if (staged != INVALID_HANDLE_VALUE) CloseHandle(staged);
        DeleteFileW(stagedPath.c_str());
        throw;
    }
}
}  // namespace

RuntimePcConfigInstallResult InstallRuntimePcPresetWithOverrides(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    const std::vector<RuntimeLaunchOptions::PcConfigOverride>& overrides,
    bool overwrite) {
    if (overrides.empty()) {
        return InstallRuntimePcPreset(sourcePath, destinationPath, overwrite);
    }

    const std::filesystem::path source = AbsoluteNormalized(sourcePath);
    const DWORD sourceAttributes = GetFileAttributesW(source.c_str());
    if (sourceAttributes == INVALID_FILE_ATTRIBUTES ||
        (sourceAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (sourceAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error(
            "source preset must be a regular non-reparse file");
    }
    const RuntimePcConfigValidation sourceValidation =
        ValidateRuntimePcConfig(source, true);
    if (!sourceValidation.valid) {
        throw std::runtime_error(
            "source preset is invalid: " +
            (sourceValidation.errors.empty()
                 ? std::string("unknown validation error")
                 : sourceValidation.errors.front()));
    }

    toml::table config = toml::parse_file(source.u8string());
    // An edit of a 0.9.0 configuration upgrades its keys like a game start
    // (before the overrides, so a key the player sets now wins); saved
    // bindings are marked current.
    UpgradeKeyBindings(config);
    std::set<std::string> seen;
    for (const auto& override : overrides) {
        if (!seen.insert(override.key).second) {
            throw std::runtime_error(
                "PC setting override key was specified more than once: " +
                override.key);
        }
        ApplyPcConfigOverride(config, override);
    }
    MarkKeyBindingsCurrent(config);

    std::ostringstream serialized;
    serialized.imbue(std::locale::classic());
    serialized << PcConfigFormatter(config) << '\n';
    RuntimePcConfigInstallResult result =
        InstallSerializedPcConfig(serialized.str(), destinationPath, overwrite);
    result.sourcePath = source;
    result.overrideCount = overrides.size();
    return result;
}

RuntimeKeyBindingsUpgrade RuntimeUpgradePcConfigFileKeyBindings(
    const std::filesystem::path& path, std::string* error) {
    RuntimeKeyBindingsUpgrade upgrade;
    if (error) error->clear();
    try {
        const std::filesystem::path file = AbsoluteNormalized(path);
        const DWORD attributes = GetFileAttributesW(file.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            upgrade.outcome = "no_file";
            return upgrade;
        }
        std::string contents;
        {
            // Closed before the install replaces the file.
            std::ifstream input(file, std::ios::binary);
            if (!input) throw std::runtime_error("unable to read the configuration");
            contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
        const std::string upgraded =
            RuntimePcConfigWithKeyBindingsUpgrade(contents, file.u8string(), &upgrade);
        if (upgrade.changed) InstallSerializedPcConfig(upgraded, file, true);
    } catch (const std::exception& exception) {
        upgrade = {};
        upgrade.outcome = "failed";
        if (error) *error = exception.what();
    }
    return upgrade;
}

std::vector<std::string> RuntimePcConfigInstallLines(
    const RuntimePcConfigInstallResult& result) {
    return {
        "PC_CONFIG_INSTALL_SOURCE=" + SingleLine(result.sourcePath.u8string()),
        "PC_CONFIG_INSTALL_DESTINATION=" +
            SingleLine(result.destinationPath.u8string()),
        "PC_CONFIG_INSTALL_OVERWROTE=" +
            std::to_string(result.overwritten ? 1 : 0),
        "PC_CONFIG_INSTALL_OVERRIDE_COUNT=" +
            std::to_string(result.overrideCount),
        "PC_CONFIG_INSTALL_VALID=1",
    };
}

std::vector<std::string> RuntimePcConfigInstallErrorLines(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath, const std::string& error) {
    return {
        "PC_CONFIG_INSTALL_SOURCE=" +
            SingleLine(AbsoluteNormalized(sourcePath).u8string()),
        "PC_CONFIG_INSTALL_DESTINATION=" +
            SingleLine(AbsoluteNormalized(destinationPath).u8string()),
        "PC_CONFIG_INSTALL_VALID=0",
        "PC_CONFIG_INSTALL_ERROR=" + SingleLine(error),
    };
}
