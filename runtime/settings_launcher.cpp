// Settings launcher (V315+).
//
// A Dear ImGui window (Win32 + Direct3D 11) around the shared settings panel
// (rex/ui/settings_panel.h, fed by pc_settings_ui.h), so the launcher and the
// in-game overlay show the same schema-driven settings. All writes still go through TheDarkness.exe's
// validated single-writer actions (--install-preset / --edit-config with
// --set-config); the launcher never edits the configuration file itself. Its
// first-run game setup (runtime_game_setup.h) writes only the game location
// file or extracts a disc image into the "game" folder.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include <toml++/toml.hpp>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "pc_settings_schema.h"
#include "pc_settings_arabic.h"
#include "pc_settings_ui.h"
#include "product_name.h"
#include "pc_settings_utility_model.h"
#include "runtime_auto_scale.h"
#include "runtime_game_setup.h"
#include "runtime_language_install.h"
#include "runtime_gpu_calibration.h"
#include "runtime_user_data.h"
#include "runtime_single_instance.h"

#include <rex/ui/rtl_text.h>
#include <rex/ui/settings_detection.h>
#include <rex/ui/settings_panel.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

namespace ui = rex::ui::settings;
namespace setup = darkness::game_setup;

// First-run game setup: the checks and the disc image extraction run on a
// worker thread; each frame draws their progress.
struct GameSetupJob {
    enum class Step { kIdle, kChecking, kExtracting, kDone, kFailed };
    std::atomic<Step> step{Step::kIdle};
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::string message;  // English error text (translated when shown)
    std::string detail;   // untranslated detail (paths, hashes, image errors)
    std::thread worker;
};

constexpr wchar_t kWindowClass[] = L"TheDarknessSettingsWindow";
constexpr wchar_t kWindowTitle[] = L"" DARKNESS_PRODUCT_NAME " - Settings";
// Without a saved configuration the game runs the Enhanced defaults (the
// example configuration); a first save starts from the matching preset.
constexpr std::wstring_view kBasePreset = L"enhanced";
// The optional Arabic language pack of this release (set by the release
// build; empty = install from a downloaded file only).
#ifndef DARKNESS_LANGUAGE_PACK_URL
#define DARKNESS_LANGUAGE_PACK_URL ""
#endif
#ifndef DARKNESS_LANGUAGE_PACK_SHA256
#define DARKNESS_LANGUAGE_PACK_SHA256 ""
#endif
constexpr char kLanguagePackUrl[] = DARKNESS_LANGUAGE_PACK_URL;
constexpr char kLanguagePackSha256[] = DARKNESS_LANGUAGE_PACK_SHA256;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_target = nullptr;
UINT g_resizeWidth = 0;
UINT g_resizeHeight = 0;
float g_dpiScale = 1.0f;
bool g_dpiChanged = false;

struct Launcher {
    std::filesystem::path directory;
    // Logs, language packs, an extracted disc image and the game-location
    // file: userData.localData (runtime_user_data.h).
    std::filesystem::path dataDirectory;
    std::filesystem::path runtimeExecutable;
    std::filesystem::path presetDirectory;
    std::filesystem::path activeConfig;
    std::filesystem::path exampleConfig;
    std::vector<PcPresetOption> presets;
    ui::Schema schema;
    ui::PanelModel model;
    // Preset whose values were loaded into the editor: Save installs it with
    // the remaining edits as overrides. -1 = editing the saved configuration.
    int loadedPreset = -1;
    ui::Values loadedPresetValues;
    int presetCombo = -1;
    bool configExists = false;
    std::string savedDescription;
    std::string status;
    bool statusError = false;
    std::string modalTitle;
    std::string modalText;
    bool openModal = false;
    bool confirmPlayWithChanges = false;
    ImFont* titleFont = nullptr;
    // Edited after the shown preset was loaded or saved: the header says Custom.
    bool presetEdited = false;
    bool onSteamDeck = false;
    // Refresh rate of each display in SDL order (0: unknown) and the monitor /
    // output the detected choices and notes were built for.
    std::vector<double> displayRefresh;
    std::string detectedFor;
    // Arabic interface (general.language arabic, or automatic on an Arabic
    // Windows): translated texts, right-to-left layout.
    bool arabic = false;
    // Shelved settings this folder offers (V440: Arabic with a pack, temporal
    // AA with DARKNESS_EXPERIMENTAL) and the ones the schema was built with.
    PcSettingsOffer offer;
    PcSettingsOffer schemaOffer;
    // Saves and settings (runtime_user_data.h): where they are, and what this
    // start copied from the game folder (shown once).
    RuntimeUserDataLayout userData;
    std::string userDataNotice;
    bool userDataNoticeError = false;
    // The game files the runtime will use (first-run setup when not ready).
    setup::GameStatus gameStatus = setup::GameStatus::kMissing;
    std::filesystem::path gameFolder;
    std::string gameSha256;
    setup::XexIdentity gameIdentity;
    std::unique_ptr<GameSetupJob> job = std::make_unique<GameSetupJob>();
    // About: credits, licence texts, crash reports.
    bool openAbout = false;
    std::string packageVersion;
    // Vendor upscaler runtimes in the game folder (temporal AA choices).
    ui::UpscalerRuntimes upscalers;
    // The optional Arabic language pack: installed, and its install job.
    bool arabicPackInstalled = false;
    std::unique_ptr<GameSetupJob> packJob = std::make_unique<GameSetupJob>();
};

// The preset a first save starts from: Steam Deck on a Deck, else Enhanced.
std::wstring_view BasePreset(const Launcher& app) {
    if (app.onSteamDeck) {
        for (const PcPresetOption& preset : app.presets) {
            if (preset.audience == PcPresetAudience::SteamDeck) return preset.presetName;
        }
    }
    return kBasePreset;
}

// The launcher's own texts in the interface language.
std::string Tr(const Launcher& app, std::string_view english) {
    if (app.arabic) {
        const std::string_view arabic = PcSettingsArabicText(english);
        if (!arabic.empty()) return std::string(arabic);
    }
    return std::string(english);
}

// Text as ImGui draws it: shaped and in visual order for Arabic.
std::string Shown(const Launcher& app, std::string_view text) {
    return app.arabic ? rex::ui::rtl::VisualLine(text) : std::string(text);
}

float TextWidth(const std::string& text) { return ImGui::CalcTextSize(text.c_str()).x; }

// Moves the cursor so an item `width` wide ends at the right edge.
void AlignRight(float width) {
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - width);
}

std::filesystem::path ExecutableDirectory() {
    std::vector<wchar_t> path(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (!length) return {};
        if (length < path.size() - 1) {
            return std::filesystem::path(path.data(), path.data() + length).parent_path();
        }
        if (path.size() >= 32768) return {};
        path.resize(path.size() * 2);
    }
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
    std::wstring result(size_t(std::max(length, 0)), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), result.data(), length);
    }
    return result;
}

std::string WideToUtf8(std::wstring_view value) { return PcSettingsUtf8(value); }

// Runs a TheDarkness.exe settings action without a window and captures its
// output (the runtime is the only validator and writer).
bool RunRuntimeAction(const Launcher& app, std::wstring command, std::string& output,
                      DWORD& exitCode, std::string& error) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        error = "Unable to create the settings result channel.";
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const BOOL started = CreateProcessW(app.runtimeExecutable.c_str(), mutableCommand.data(),
                                        nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                        app.directory.c_str(), &startup, &process);
    CloseHandle(writePipe);
    if (!started) {
        const DWORD startError = GetLastError();
        CloseHandle(readPipe);
        // Usually an antivirus that removed or blocked the new executable, or
        // a partly extracted zip: name Windows' reason and the way out.
        wchar_t* text = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, startError, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
        std::wstring reason = text ? text : L"";
        if (text) LocalFree(text);
        while (!reason.empty() && (reason.back() == L'\r' || reason.back() == L'\n' ||
                                   reason.back() == L' ' || reason.back() == L'.')) {
            reason.pop_back();
        }
        error = "Unable to start TheDarkness.exe: " +
                (reason.empty() ? std::string("unknown reason") : WideToUtf8(reason)) +
                " (Windows error " + std::to_string(startError) +
                "). If an antivirus removed or blocked it, restore it and allow the game "
                "folder, or extract the whole zip again into an empty folder.";
        return false;
    }
    output.clear();
    char buffer[2048];
    const auto drain = [&] {
        DWORD available = 0;
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            DWORD bytesRead = 0;
            if (!ReadFile(readPipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &bytesRead,
                          nullptr) ||
                !bytesRead) {
                break;
            }
            output.append(buffer, buffer + bytesRead);
        }
    };
    // An action takes well under a second; a first start can wait for a virus
    // scan of the new executable. A child that never finishes is stopped
    // instead of freezing the launcher.
    constexpr ULONGLONG kActionTimeoutMs = 90000;
    const ULONGLONG deadline = GetTickCount64() + kActionTimeoutMs;
    bool finished = false;
    while (!finished) {
        drain();
        finished = WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0;
        if (!finished && GetTickCount64() > deadline) break;
    }
    drain();
    CloseHandle(readPipe);
    if (!finished) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        error = "TheDarkness.exe did not finish the settings action within 90 seconds.";
        return false;
    }
    exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

// The runtime's own error line, not its startup banner.
std::string CompactError(const std::string& output) {
    std::string found;
    size_t start = 0;
    while (start < output.size()) {
        size_t end = output.find_first_of("\r\n", start);
        if (end == std::string::npos) end = output.size();
        const std::string line = output.substr(start, end - start);
        for (const char* marker : {"PC_CONFIG_INSTALL_ERROR=", "PC_CONFIG_ERROR=",
                                   "RUNTIME_BLOCKER "}) {
            const size_t at = line.find(marker);
            if (at != std::string::npos && found.empty()) {
                found = line.substr(at + std::char_traits<char>::length(marker));
            }
        }
        start = end + 1;
    }
    if (found.empty()) {
        const size_t last = output.find_last_not_of("\r\n");
        if (last != std::string::npos) {
            const size_t begin = output.find_last_of("\r\n", last);
            found = output.substr(begin == std::string::npos ? 0 : begin + 1,
                                  last - (begin == std::string::npos ? 0 : begin + 1) + 1);
        }
    }
    if (found.size() > 600) found.resize(600);
    return found.empty() ? std::string("The settings action failed.") : found;
}

// Schema values of a configuration file, normalized so numbers compare by
// value (the inspection prints full double precision).
std::optional<ui::Values> InspectConfig(const Launcher& app,
                                                      const std::filesystem::path& path,
                                                      std::string& error) {
    std::string output;
    DWORD exitCode = 1;
    if (!RunRuntimeAction(app, BuildPcConfigInspectionCommandLine(app.runtimeExecutable, path),
                          output, exitCode, error)) {
        return std::nullopt;
    }
    if (exitCode != 0) {
        error = CompactError(output);
        return std::nullopt;
    }
    ui::Values values;
    for (const PcConfigInspectionEntry& entry : ParsePcConfigInspection(output)) {
        const ui::Setting* setting = app.schema.Find(entry.key);
        if (!setting) continue;
        std::string value = entry.value;
        if (setting->editor == ui::Editor::kNumber) {
            char* end = nullptr;
            const double number = std::strtod(value.c_str(), &end);
            if (end && *end == '\0') value = ui::FormatNumberValue(*setting, number);
        }
        values[entry.key] = value;
    }
    return values;
}

void SetStatus(Launcher& app, std::string text, bool error = false) {
    app.status = std::move(text);
    app.statusError = error;
}

void ShowModal(Launcher& app, std::string title, std::string text) {
    app.modalTitle = std::move(title);
    app.modalText = std::move(text);
    app.openModal = true;
}

int PresetIndex(const Launcher& app, std::wstring_view name) {
    for (size_t index = 0; index < app.presets.size(); ++index) {
        if (app.presets[index].presetName == name) return int(index);
    }
    return -1;
}

void DescribeSaved(Launcher& app) {
    std::error_code error;
    app.configExists = std::filesystem::is_regular_file(app.activeConfig, error) && !error;
    if (!app.configExists) {
        app.savedDescription =
            Tr(app, app.onSteamDeck
                        ? "No settings saved yet: Steam Deck settings are ready to save."
                        : "No settings saved yet: the game starts with the Enhanced settings.");
        app.presetCombo = PresetIndex(app, BasePreset(app));
        return;
    }
    if (const auto match = FindExactPcPresetMatch(app.activeConfig, app.presetDirectory,
                                                  app.presets)) {
        app.savedDescription = ui::FormatText(
            Tr(app, "Saved: {}"), {Tr(app, WideToUtf8(app.presets[*match].displayName))});
        app.presetCombo = int(*match);
    } else {
        app.savedDescription = Tr(app, "Saved: custom settings");
        app.presetCombo = -1;
    }
}

bool Reload(Launcher& app) {
    std::string error;
    auto defaults = InspectConfig(app, app.exampleConfig, error);
    if (!defaults) {
        ShowModal(app, Tr(app, "Settings unavailable"),
                  "The default settings could not be read: " + error);
        return false;
    }
    app.model.defaults = std::move(*defaults);
    DescribeSaved(app);
    app.model.saved.clear();
    if (app.configExists) {
        auto saved = InspectConfig(app, app.activeConfig, error);
        if (!saved) {
            ShowModal(app, Tr(app, "Settings file problem"),
                      "TheDarkness.pc.toml could not be read: " + error +
                          "\n\nFix or delete the file, or load a preset and save.");
            return false;
        }
        app.model.saved = std::move(*saved);
    } else if (app.presetCombo >= 0) {
        auto base = InspectConfig(app, app.presetDirectory / app.presets[app.presetCombo].filename,
                                  error);
        if (base) app.model.saved = std::move(*base);
    }
    app.model.values.clear();
    app.loadedPreset = -1;
    app.loadedPresetValues.clear();
    app.presetEdited = false;
    return true;
}

void LoadPreset(Launcher& app, int index) {
    std::string error;
    auto values = InspectConfig(app, app.presetDirectory / app.presets[index].filename, error);
    if (!values) {
        ShowModal(app, Tr(app, "Preset unavailable"), error);
        return;
    }
    app.model.values = *values;
    app.loadedPreset = index;
    app.loadedPresetValues = std::move(*values);
    app.presetEdited = false;
    SetStatus(app, ui::FormatText(Tr(app, "Loaded preset {}. Save to use it."),
                                  {Tr(app, WideToUtf8(app.presets[index].displayName))}));
}

bool Save(Launcher& app) {
    std::vector<PcConfigOverrideArgument> overrides;
    std::wstring preset;
    bool overwrite = app.configExists;
    if (app.loadedPreset >= 0 || !app.configExists) {
        const int base = app.loadedPreset >= 0 ? app.loadedPreset : PresetIndex(app, BasePreset(app));
        if (base < 0) {
            ShowModal(app, Tr(app, "Settings not saved"),
                      Tr(app, "The Enhanced preset is missing from the package."));
            return false;
        }
        preset = app.presets[base].presetName;
        const ui::Values& baseValues = app.loadedPreset >= 0 ? app.loadedPresetValues
                                                             : app.model.saved;
        for (const ui::Setting& setting : app.schema.settings) {
            const std::string value = ui::ValueOf(app.model, setting.key);
            const auto it = baseValues.find(setting.key);
            if (!value.empty() && (it == baseValues.end() || it->second != value)) {
                overrides.push_back({Utf8ToWide(setting.key), Utf8ToWide(value)});
            }
        }
    } else {
        for (const std::string& key : ui::PendingKeys(app.model)) {
            overrides.push_back({Utf8ToWide(key), Utf8ToWide(app.model.values.at(key))});
        }
        if (overrides.empty()) {
            SetStatus(app, Tr(app, "Nothing to save."));
            return true;
        }
    }
    std::string output;
    DWORD exitCode = 1;
    std::string error;
    if (!RunRuntimeAction(app,
                          BuildPcPresetInstallCommandLine(app.runtimeExecutable, preset, overwrite,
                                                          overrides),
                          output, exitCode, error)) {
        ShowModal(app, Tr(app, "Settings not saved"), error);
        return false;
    }
    if (exitCode != 0) {
        ShowModal(app, Tr(app, "Settings not saved"), CompactError(output));
        return false;
    }
    Reload(app);
    SetStatus(app, Tr(app, "Saved. The game uses these settings the next time it starts."));
    return true;
}

bool Launch(Launcher& app, bool safeMode) {
    try {
        if (RuntimeSingleInstance::Exists(kRuntimeTitleLockName)) {
            ShowModal(app, Tr(app, "Already running"),
                      Tr(app, "The game is already running. Saved settings apply at its next "
                              "start."));
            return false;
        }
    } catch (const std::exception&) {
        ShowModal(app, Tr(app, "Launch failed"),
                  Tr(app, "Unable to check whether the game is running."));
        return false;
    }
    std::wstring command = safeMode ? BuildPcSafeModeLaunchCommandLine(app.runtimeExecutable)
                                    : BuildPcDefaultLaunchCommandLine(app.runtimeExecutable);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    if (!CreateProcessW(app.runtimeExecutable.c_str(), mutableCommand.data(), nullptr, nullptr,
                        FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, app.directory.c_str(),
                        &startup, &process)) {
        ShowModal(app, Tr(app, "Launch failed"), Tr(app, "Unable to start TheDarkness.exe."));
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

// ---- First-run game setup ----

// Which game the runtime will start and whether it is the supported version.
void CheckGame(Launcher& app) {
    const std::filesystem::path xex = setup::FindGameXex(app.dataDirectory, app.directory);
    app.gameFolder = xex.empty() ? std::filesystem::path() : xex.parent_path();
    if (xex.empty()) {
        app.gameStatus = setup::GameStatus::kMissing;
        app.gameSha256.clear();
        return;
    }
    const setup::GameCheck check = setup::CheckGameFolder(app.gameFolder);
    app.gameStatus = check.status;
    app.gameSha256 = check.sha256;
    app.gameIdentity = check.identity;
}

// A file (disc image) or folder picked with the Windows dialog.
enum class Pick { kGameFolder, kDiscImage, kLanguagePack, kSavesFolder };

std::optional<std::filesystem::path> PickPath(const Launcher& app, Pick pick) {
    const bool folder = pick == Pick::kGameFolder || pick == Pick::kSavesFolder;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::optional<std::filesystem::path> picked;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (folder) {
            options |= FOS_PICKFOLDERS;
        } else {
            options |= FOS_FILEMUSTEXIST;
            static const COMDLG_FILTERSPEC kImages[] = {{L"Xbox 360 disc image", L"*.iso"},
                                                        {L"All files", L"*.*"}};
            static const COMDLG_FILTERSPEC kPacks[] = {{L"Language pack", L"*.zip"},
                                                       {L"All files", L"*.*"}};
            dialog->SetFileTypes(2, pick == Pick::kLanguagePack ? kPacks : kImages);
        }
        dialog->SetOptions(options);
        const std::wstring title = Utf8ToWide(
            Tr(app, pick == Pick::kGameFolder    ? "Choose the game folder (it contains default.xex)"
                    : pick == Pick::kSavesFolder ? "Choose the folder of the older version (it "
                                                   "contains TheDarkness.exe)"
                    : pick == Pick::kDiscImage   ? "Choose the disc image"
                                                 : "Choose the language pack (.zip)"));
        dialog->SetTitle(title.c_str());
        if (SUCCEEDED(dialog->Show(GetActiveWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    picked = std::filesystem::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return picked;
}

void FinishJob(GameSetupJob& job, bool ok, std::string message, std::string detail = {}) {
    {
        std::lock_guard<std::mutex> lock(job.mutex);
        job.message = std::move(message);
        job.detail = std::move(detail);
    }
    job.step = ok ? GameSetupJob::Step::kDone : GameSetupJob::Step::kFailed;
}

void StartJob(GameSetupJob& job, std::function<void(GameSetupJob&)> work) {
    if (job.worker.joinable()) job.worker.join();
    job.cancel = false;
    job.done = 0;
    job.total = 0;
    job.step = GameSetupJob::Step::kChecking;
    job.worker = std::thread([&job, work = std::move(work)]() { work(job); });
}

// The detailed version report (names, sizes and hashes; no game data) in
// the logs folder, for a bug report if the player wants to attach it.
std::string WriteVersionReport(const std::filesystem::path& directory,
                               const setup::XexIdentity& identity, const std::string& source) {
    const std::filesystem::path logs = directory / L"logs";
    std::error_code error;
    std::filesystem::create_directories(logs, error);
    const std::filesystem::path report = logs / L"game_version_report.txt";
    std::ofstream out(report, std::ios::binary | std::ios::trunc);
    out << setup::XexIdentityReport(identity, source);
    out.close();
    return out ? "A detailed report is in " + report.u8string() +
                     "; you can attach it to a bug report. Never upload game files."
               : std::string();
}

// Why a copy of the game cannot be used, for the setup panel.
std::string DifferentVersionText(const setup::XexIdentity& identity) {
    return identity.match == setup::XexMatch::kNotReadable
               ? setup::XexMatchText(identity)
               : "This version of The Darkness runs different code, which this release cannot "
                 "run yet. Supported: the Xbox 360 USA/Europe disc and copies with the same code "
                 "(for example localised releases).";
}

// A folder with the extracted game: checked, then remembered.
void SetUpFromFolder(Launcher& app, std::filesystem::path folder) {
    const std::filesystem::path directory = app.dataDirectory;
    StartJob(*app.job, [directory, folder](GameSetupJob& job) {
        const setup::GameCheck check = setup::CheckGameFolder(folder);
        switch (check.status) {
            case setup::GameStatus::kMissing:
                return FinishJob(job, false,
                                 "No default.xex in that folder. Choose the folder that contains "
                                 "default.xex.");
            case setup::GameStatus::kUnreadable:
                return FinishJob(job, false, "Unable to read default.xex.");
            case setup::GameStatus::kWrongVersion:
                return FinishJob(job, false, DifferentVersionText(check.identity),
                                 WriteVersionReport(directory, check.identity,
                                                    (folder / L"default.xex").u8string()));
            case setup::GameStatus::kOk:
                break;
        }
        if (check.identity.match == setup::XexMatch::kSameCode) {
            WriteVersionReport(directory, check.identity, (folder / L"default.xex").u8string());
        }
        std::string error;
        if (!setup::WriteGameLocation(directory, folder, &error)) {
            return FinishJob(job, false, "Unable to save the game location.", error);
        }
        FinishJob(job, true, "Game files ready.");
    });
}

// A disc image: checked, then extracted into the "game" folder next to the
// launcher (through a temporary folder, so a cancelled extraction never
// looks like a game).
void SetUpFromImage(Launcher& app, std::filesystem::path image) {
    const std::filesystem::path directory = app.dataDirectory;
    StartJob(*app.job, [directory, image](GameSetupJob& job) {
        const setup::DiscImageInfo info = setup::InspectDiscImage(image);
        if (!info.ok) return FinishJob(job, false, "Unable to use this disc image.", info.error);
        if (info.xexSha256.empty()) {
            return FinishJob(job, false, "The disc image has no default.xex.");
        }
        if (info.xex.match != setup::XexMatch::kSupported &&
            info.xex.match != setup::XexMatch::kSameCode) {
            return FinishJob(job, false, DifferentVersionText(info.xex),
                             WriteVersionReport(directory, info.xex,
                                                image.u8string() + " (default.xex)"));
        }
        if (info.xex.match == setup::XexMatch::kSameCode) {
            WriteVersionReport(directory, info.xex, image.u8string() + " (default.xex)");
        }
        const std::filesystem::path target = directory / setup::kExtractedFolderName;
        const std::filesystem::path partial = directory / L"game.partial";
        std::error_code error;
        if (std::filesystem::exists(target, error)) {
            return FinishJob(job, false,
                             "A folder named game already exists next to the launcher. Rename or "
                             "remove it, then choose the disc image again.");
        }
        ULARGE_INTEGER available{};
        if (GetDiskFreeSpaceExW(directory.c_str(), &available, nullptr, nullptr) &&
            available.QuadPart < info.bytes + (256ull << 20)) {
            return FinishJob(job, false, "Not enough free disk space next to the launcher.",
                             std::to_string((info.bytes >> 20) + 256) + " MB needed");
        }
        std::filesystem::remove_all(partial, error);
        job.total = info.bytes;
        job.step = GameSetupJob::Step::kExtracting;
        std::string extractError;
        const bool extracted = setup::ExtractDiscImage(
            image, partial,
            [&job](uint64_t done, uint64_t total) {
                job.done = done;
                job.total = total;
                return !job.cancel.load();
            },
            &extractError);
        if (!extracted) {
            std::filesystem::remove_all(partial, error);
            return FinishJob(job, false,
                             job.cancel ? "Extraction cancelled." : "Extraction failed.",
                             job.cancel ? std::string() : extractError);
        }
        std::filesystem::rename(partial, target, error);
        if (error) {
            return FinishJob(job, false, "Unable to rename the extracted folder.", error.message());
        }
        std::string writeError;
        if (!setup::WriteGameLocation(directory, target, &writeError)) {
            return FinishJob(job, false, "Unable to save the game location.", writeError);
        }
        FinishJob(job, true, "Game files ready.");
    });
}

// The setup panel shown instead of the settings while no supported game is
// found.
void DrawGameSetup(Launcher& app) {
    GameSetupJob& job = *app.job;
    const GameSetupJob::Step step = job.step;
    const float width = ImGui::GetContentRegionAvail().x;
    auto paragraph = [&](const std::string& text, bool disabled = false) {
        if (app.arabic) {
            const float x = ImGui::GetCursorPosX();
            for (const std::string& line : rex::ui::rtl::WrapVisual(text, width, TextWidth)) {
                ImGui::SetCursorPosX(x + std::max(0.0f, width - TextWidth(line)));
                if (disabled) {
                    ImGui::TextDisabled("%s", line.c_str());
                } else {
                    ImGui::TextUnformatted(line.c_str());
                }
            }
            return;
        }
        ImGui::PushTextWrapPos(0.0f);
        if (disabled) {
            ImGui::TextDisabled("%s", text.c_str());
        } else {
            ImGui::TextUnformatted(text.c_str());
        }
        ImGui::PopTextWrapPos();
    };
    ImGui::Spacing();
    if (app.titleFont) ImGui::PushFont(app.titleFont, ImGui::GetStyle().FontSizeBase * 1.2f);
    const std::string title = Shown(app, Tr(app, "Game files"));
    if (app.arabic) AlignRight(TextWidth(title));
    ImGui::TextUnformatted(title.c_str());
    if (app.titleFont) ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
    paragraph(Tr(app, "The game needs your own copy of The Darkness for Xbox 360 (the "
                      "USA/Europe disc, or a release with the same code such as a localised "
                      "one). Choose a disc image (.iso) or a folder with the extracted game "
                      "files (the folder that contains default.xex). A disc image is extracted "
                      "next to the launcher (about 7 GB)."));
    if (app.gameStatus == setup::GameStatus::kWrongVersion && !app.gameSha256.empty()) {
        ImGui::Spacing();
        paragraph(Tr(app, DifferentVersionText(app.gameIdentity)));
        paragraph(app.gameFolder.u8string() + "  (default.xex SHA-256 " + app.gameSha256 + ")",
                  true);
    }
    ImGui::Spacing();
    ImGui::Spacing();
    const bool busy =
        step == GameSetupJob::Step::kChecking || step == GameSetupJob::Step::kExtracting;
    const float button = ImGui::GetFontSize() * 13.0f;
    ImGui::BeginDisabled(busy);
    if (app.arabic) AlignRight(button * 2.0f + ImGui::GetStyle().ItemSpacing.x);
    auto choose = [&](const char* english, const char* id, bool folder) {
        if (ImGui::Button((Shown(app, Tr(app, english)) + id).c_str(), ImVec2(button, 0.0f))) {
            if (const auto picked = PickPath(app, folder ? Pick::kGameFolder : Pick::kDiscImage)) {
                if (folder) {
                    SetUpFromFolder(app, *picked);
                } else {
                    SetUpFromImage(app, *picked);
                }
            }
        }
    };
    if (app.arabic) {
        choose("Choose game folder...", "##folder", true);
        ImGui::SameLine();
        choose("Choose disc image...", "##image", false);
    } else {
        choose("Choose disc image...", "##image", false);
        ImGui::SameLine();
        choose("Choose game folder...", "##folder", true);
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (step == GameSetupJob::Step::kChecking) {
        paragraph(Tr(app, "Checking the game files..."));
    } else if (step == GameSetupJob::Step::kExtracting) {
        const uint64_t done = job.done, total = job.total;
        const float fraction = total ? float(double(done) / double(total)) : 0.0f;
        paragraph(ui::FormatText(Tr(app, "Extracting the disc image: {} of {} MB"),
                                 {std::to_string(done >> 20), std::to_string(total >> 20)}));
        ImGui::ProgressBar(fraction, ImVec2(width, 0.0f));
        if (ImGui::Button((Shown(app, Tr(app, "Cancel")) + "##cancel").c_str())) job.cancel = true;
    } else if (step == GameSetupJob::Step::kFailed) {
        std::string message, detail;
        {
            std::lock_guard<std::mutex> lock(job.mutex);
            message = job.message;
            detail = job.detail;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.45f, 0.40f, 1.0f));
        paragraph(Tr(app, message));
        ImGui::PopStyleColor();
        if (!detail.empty()) paragraph(detail, true);
    }
}

// Picks up a finished setup job (on the frame's thread).
void PollGameSetup(Launcher& app) {
    GameSetupJob& job = *app.job;
    if (job.step != GameSetupJob::Step::kDone) return;
    if (job.worker.joinable()) job.worker.join();
    job.step = GameSetupJob::Step::kIdle;
    CheckGame(app);
    if (app.gameStatus == setup::GameStatus::kOk) {
        SetStatus(app, Tr(app, "Game files ready.") + " " + app.gameFolder.u8string());
    }
}

void StopGameSetup(Launcher& app) {
    for (GameSetupJob* job : {app.job.get(), app.packJob.get()}) {
        job->cancel = true;
        if (job->worker.joinable()) job->worker.join();
    }
}

void DetectDisplays(Launcher& app) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    std::vector<ui::Choice> choices;
    app.displayRefresh.clear();
    for (int index = 0; displays && index < count; ++index) {
        std::string label = std::to_string(index + 1) + ": ";
        const char* name = SDL_GetDisplayName(displays[index]);
        label += name ? name : "Display";
        double refresh = 0.0;
        if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(displays[index])) {
            label += " (" + std::to_string(mode->w) + " x " + std::to_string(mode->h) + ")";
            refresh = double(mode->refresh_rate);
        }
        app.displayRefresh.push_back(refresh);
        choices.push_back({std::to_string(index), std::move(label)});
    }
    SDL_free(displays);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (choices.empty()) return;
    for (const ui::Setting& setting : app.schema.settings) {
        if (setting.editor == ui::Editor::kDisplay) app.model.detected_choices[setting.key] = choices;
    }
}

// HD texture packs (V374): offered only when one is installed, with its size
// and video memory next to this graphics card's.
// NVIDIA DLSS needs an NVIDIA RTX card: the high-performance adapter is an
// NVIDIA one (vendor 0x10DE) with RTX in its name (as the in-game overlay
// checks). True when DXGI can't tell.
bool HasNvidiaRtxCard() {
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory)))) {
        return true;
    }
    bool rtx = false;
    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                      __uuidof(IDXGIAdapter1),
                                                      reinterpret_cast<void**>(&adapter)))) {
        DXGI_ADAPTER_DESC1 desc{};
        rtx = SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10DE &&
              !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && wcsstr(desc.Description, L"RTX");
        adapter->Release();
    }
    factory->Release();
    return rtx;
}

void DetectTexturePacks(Launcher& app) {
    const ui::TexturePackFolder pack =
        ui::ScanTexturePackFolder(app.directory / L"texture_packs");
    ui::DetectTexturePack(app.model, pack, RuntimeDetectGpuIdentity().videoMemoryBytes,
                          "the texture_packs folder in the game folder");
    // In-game Arabic (V409): the Language setting says whether a pack is
    // installed. V440: Arabic is offered only with a pack (installed, carried
    // by the package or downloadable).
    ui::LanguagePackFolder arabic =
        ui::ScanLanguagePackFolder(app.dataDirectory / L"language_packs" / L"arabic");
    if (!(arabic.strings && arabic.fonts) && app.dataDirectory != app.directory) {
        // A pack installed beside the executables by an earlier version.
        const ui::LanguagePackFolder beside =
            ui::ScanLanguagePackFolder(app.directory / L"language_packs" / L"arabic");
        if (beside.strings && beside.fonts) arabic = beside;
    }
    app.arabicPackInstalled = arabic.strings && arabic.fonts;
    app.offer = PcSettingsOfferFor(app.dataDirectory, app.directory, kLanguagePackUrl[0] != '\0');
    if (app.offer.arabic) {
        ui::DetectLanguagePack(app.model, arabic,
                               "the language_packs\\arabic folder in the game folder");
    }
}

// ---- The optional Arabic language pack ----

// Installs a downloaded pack archive (worker thread) against the player's
// own game files.
void StartPackInstall(Launcher& app, std::filesystem::path archive, bool download) {
    const std::filesystem::path directory = app.dataDirectory;
    const std::filesystem::path game = app.gameFolder;
    StartJob(*app.packJob, [directory, game, archive, download](GameSetupJob& job) {
        const auto progress = [&job](uint64_t done, uint64_t total) {
            job.done = done;
            job.total = total;
            return !job.cancel.load();
        };
        std::filesystem::path file = archive;
        if (download) {
            file = directory / L"language_packs" / L"download.partial.zip";
            job.step = GameSetupJob::Step::kExtracting;
            std::string error;
            if (!darkness::language_install::DownloadFile(
                    Utf8ToWide(kLanguagePackUrl), file, kLanguagePackSha256, progress, error)) {
                std::error_code ignore;
                std::filesystem::remove(file, ignore);
                return FinishJob(job, false, "Download failed.", error);
            }
        }
        job.step = GameSetupJob::Step::kChecking;
        const auto result = darkness::language_install::InstallPack(file, game, directory, progress);
        if (download) {
            std::error_code ignore;
            std::filesystem::remove(file, ignore);
        }
        FinishJob(job, result.ok, result.message, result.detail);
    });
}

// Offered when the Arabic interface is chosen and the game's own text has
// no pack yet, or while an install runs or reports.
void DrawLanguagePackBar(Launcher& app) {
    GameSetupJob& job = *app.packJob;
    const GameSetupJob::Step step = job.step;
    const bool arabic = ui::ValueOf(app.model, "general.language") == "arabic";
    const bool busy =
        step == GameSetupJob::Step::kChecking || step == GameSetupJob::Step::kExtracting;
    if (!busy && step != GameSetupJob::Step::kFailed && (!arabic || app.arabicPackInstalled)) {
        return;
    }
    // Shelved (V443): without an offered pack (a configuration copied from
    // another folder can still say "arabic") nothing points to Arabic.
    if (!busy && !app.offer.arabic) return;
    const float width = ImGui::GetContentRegionAvail().x;
    auto line = [&](const std::string& text, bool error = false) {
        if (error) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.45f, 0.40f, 1.0f));
        if (app.arabic) {
            const float x = ImGui::GetCursorPosX();
            for (const std::string& shown : rex::ui::rtl::WrapVisual(text, width, TextWidth)) {
                ImGui::SetCursorPosX(x + std::max(0.0f, width - TextWidth(shown)));
                ImGui::TextUnformatted(shown.c_str());
            }
        } else {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
        }
        if (error) ImGui::PopStyleColor();
    };
    ImGui::Spacing();
    if (busy) {
        const uint64_t done = job.done, total = job.total;
        line(step == GameSetupJob::Step::kExtracting
                 ? ui::FormatText(Tr(app, "Downloading the language pack: {} of {} MB"),
                                  {std::to_string(done >> 20), std::to_string(total >> 20)})
                 : Tr(app, "Installing the language pack..."));
        ImGui::ProgressBar(total ? float(double(done) / double(total)) : 0.0f, ImVec2(width, 0.0f));
    } else {
        if (step == GameSetupJob::Step::kFailed) {
            std::string message, detail;
            {
                std::lock_guard<std::mutex> lock(job.mutex);
                message = job.message;
                detail = job.detail;
            }
            line(Tr(app, message) + (detail.empty() ? "" : "  (" + detail + ")"), true);
        }
        if (arabic && !app.arabicPackInstalled) {
            line(Tr(app, "The game's own text (menus, messages, subtitles) needs the Arabic "
                         "language pack."));
            if (app.gameStatus != setup::GameStatus::kOk) {
                line(Tr(app, "Set up the game files first."));
            } else {
                const float button = ImGui::GetFontSize() * 13.0f;
                // A package can carry the pack (language_packs/
                // arabic_language_pack.zip): then one click installs it.
                const std::filesystem::path included =
                    app.directory / L"language_packs" / L"arabic_language_pack.zip";
                std::error_code includedError;
                const bool bundled = std::filesystem::is_regular_file(included, includedError);
                const bool url = !bundled && kLanguagePackUrl[0] != '\0';
                const bool two = url || bundled;
                if (app.arabic) AlignRight(button * (two ? 2.0f : 1.0f) + ImGui::GetStyle().ItemSpacing.x);
                auto install = [&] {
                    if (ImGui::Button((Shown(app, Tr(app, "Install")) + "##packincluded").c_str(),
                                      ImVec2(button, 0.0f))) {
                        StartPackInstall(app, included, false);
                    }
                };
                auto download = [&] {
                    if (ImGui::Button((Shown(app, Tr(app, "Download and install")) + "##packdl").c_str(),
                                      ImVec2(button, 0.0f))) {
                        StartPackInstall(app, {}, true);
                    }
                };
                auto from_file = [&] {
                    if (ImGui::Button((Shown(app, Tr(app, "Install from file...")) + "##packfile").c_str(),
                                      ImVec2(button, 0.0f))) {
                        if (const auto picked = PickPath(app, Pick::kLanguagePack)) {
                            StartPackInstall(app, *picked, false);
                        }
                    }
                };
                // Right to left, the first button sits on the right.
                const auto first = [&] { if (bundled) install(); else download(); };
                if (two && app.arabic) {
                    from_file();
                    ImGui::SameLine();
                    first();
                } else if (two) {
                    first();
                    ImGui::SameLine();
                    from_file();
                } else {
                    from_file();
                }
            }
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
}

// Picks up a finished pack install (on the frame's thread).
void PollLanguagePack(Launcher& app) {
    GameSetupJob& job = *app.packJob;
    if (job.step != GameSetupJob::Step::kDone) return;
    if (job.worker.joinable()) job.worker.join();
    job.step = GameSetupJob::Step::kIdle;
    DetectTexturePacks(app);
    SetStatus(app, Tr(app, "Language pack installed."));
}

// Frame rates from the chosen display's refresh and what Automatic internal
// scale picks there, rebuilt when the display or output changes.
void UpdateDetections(Launcher& app) {
    const std::string monitorText = ui::ValueOf(app.model, "monitor");
    const std::string output = ui::ValueOf(app.model, "output_resolution");
    const std::string frameRate = ui::ValueOf(app.model, "display.frame_rate");
    const std::string frameLimit = ui::ValueOf(app.model, "display.frame_limit");
    const std::string key = monitorText + "|" + output + "|" + frameRate + "|" + frameLimit;
    if (key == app.detectedFor) return;
    app.detectedFor = key;
    const int monitor = std::max(0, std::atoi(monitorText.c_str()));
    const double refresh = size_t(monitor) < app.displayRefresh.size()
                               ? app.displayRefresh[size_t(monitor)]
                               : (app.displayRefresh.empty() ? 0.0 : app.displayRefresh[0]);
    ui::DetectFrameRateChoices(app.model, refresh);
    RuntimeAutoScale automatic = RuntimeDetectAutomaticScale(output, monitor);
    // V338: the game's measurement of this graphics card, when it has one
    // (default user data folder beside the game).
    bool measured = false;
    const RuntimeGpuIdentity gpu = RuntimeDetectGpuIdentity();
    const auto calibration =
        RuntimeLoadGpuCalibration(RuntimeGpuCalibrationPath(app.userData.root));
    if (calibration && RuntimeGpuCalibrationMatches(*calibration, gpu)) {
        RuntimeScaleChoiceInputs inputs;
        inputs.backdrop = calibration->backdrop;
        inputs.outputHeight = automatic.outputHeight;
        inputs.targetFps = RuntimeAutomaticScaleTargetFps(
            frameRate, uint32_t(std::max(0, std::atoi(frameLimit.c_str()))), refresh);
        inputs.videoMemoryBytes = gpu.videoMemoryBytes;
        automatic.scale = RuntimeChooseMeasuredScale(inputs).scale;
        measured = true;
    }
    std::string note =
        ui::FormatText(Tr(app, "Automatic here: {}"), {RuntimeAutoScaleLabel(automatic.scale)});
    if (automatic.outputWidth && automatic.outputHeight) {
        note += ui::FormatText(Tr(app, " for a {} x {} screen"),
                               {std::to_string(automatic.outputWidth),
                                std::to_string(automatic.outputHeight)});
    }
    note += Tr(app, measured ? " (measured on this graphics card)"
                             : " (estimated; the game measures the card on its first start)");
    app.model.detected_notes["resolution_scale"] = std::move(note);
}

void DrawHeader(Launcher& app) {
    // The product's own name as plain text (no game logo), then what it runs.
    const std::string subtitle = Shown(app, Tr(app, "The Darkness (Xbox 360)  |  Settings"));
    const char* title = DARKNESS_PRODUCT_NAME;
    const float title_size = ImGui::GetStyle().FontSizeBase * 1.9f;
    const ImVec2 top = ImGui::GetCursorPos();
    if (app.arabic) {
        // Mirrored: the title on the right, the subtitle to its left.
        if (app.titleFont) ImGui::PushFont(app.titleFont, title_size);
        const float title_width = TextWidth(title);
        if (app.titleFont) ImGui::PopFont();
        AlignRight(title_width + ImGui::GetStyle().ItemSpacing.x + TextWidth(subtitle));
        const float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + ImGui::GetFontSize() * 0.55f);
        ImGui::TextDisabled("%s", subtitle.c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosY(y);
        if (app.titleFont) ImGui::PushFont(app.titleFont, title_size);
        ImGui::TextUnformatted(title);
        if (app.titleFont) ImGui::PopFont();
    } else {
        if (app.titleFont) ImGui::PushFont(app.titleFont, title_size);
        ImGui::TextUnformatted(title);
        if (app.titleFont) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFontSize() * 0.55f);
        ImGui::TextDisabled("%s", subtitle.c_str());
    }
    // About at the far end of the title line.
    {
        const ImVec2 below = ImGui::GetCursorPos();
        const std::string about = Shown(app, Tr(app, "About"));
        const float width = TextWidth(about) + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPos(ImVec2(app.arabic ? top.x : ImGui::GetContentRegionMax().x - width,
                                   top.y + ImGui::GetFontSize() * 0.35f));
        if (ImGui::Button((about + "##about").c_str())) app.openAbout = true;
        ImGui::SetCursorPos(below);
    }

    ImGui::Spacing();
    // Any edit after a preset turns the header into Custom.
    const std::string preview = Shown(
        app, app.presetCombo >= 0 && !app.presetEdited
                 ? Tr(app, WideToUtf8(app.presets[app.presetCombo].displayName))
                 : Tr(app, "Custom"));
    const std::string label = Shown(app, Tr(app, "Preset"));
    const std::string saved = Shown(app, app.savedDescription);
    const float combo_width = ImGui::GetFontSize() * 18.0f;
    auto combo = [&]() {
        ImGui::SetNextItemWidth(combo_width);
        if (ImGui::BeginCombo("##preset", preview.c_str())) {
            if (app.arabic) {
                ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(1.0f, 0.5f));
            }
            for (int index = 0; index < int(app.presets.size()); ++index) {
                // Players see the three quality levels (and Steam Deck on a Deck);
                // output-path, validation and developer presets need Advanced.
                const PcPresetAudience audience = app.presets[index].audience;
                const bool offered = audience == PcPresetAudience::Player ||
                                     (audience == PcPresetAudience::SteamDeck && app.onSteamDeck);
                if (!offered && !app.model.show_advanced && index != app.presetCombo) continue;
                const std::string name =
                    Shown(app, Tr(app, WideToUtf8(app.presets[index].displayName))) + "##" +
                    std::to_string(index);
                if (ImGui::Selectable(name.c_str(), index == app.presetCombo)) {
                    app.presetCombo = index;
                    LoadPreset(app, index);
                }
            }
            if (app.arabic) ImGui::PopStyleVar();
            ImGui::EndCombo();
        }
    };
    ImGui::AlignTextToFramePadding();
    if (app.arabic) {
        // Mirrored: [saved] [preset combo] [Preset], right-aligned.
        const ImGuiStyle& style = ImGui::GetStyle();
        AlignRight(TextWidth(saved) + style.ItemSpacing.x + combo_width + style.ItemSpacing.x +
                   TextWidth(label));
        ImGui::TextDisabled("%s", saved.c_str());
        ImGui::SameLine();
        combo();
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());
    } else {
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine();
        combo();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", saved.c_str());
    }
    ImGui::Spacing();
}

void DrawFooter(Launcher& app) {
    const auto pending = ui::PendingKeys(app.model);
    const bool dirty = !pending.empty() || app.loadedPreset >= 0;
    ImGui::Spacing();
    std::string message;
    ImVec4 color(0.60f, 0.59f, 0.57f, 1.0f);
    if (!app.status.empty()) {
        message = app.status;
        if (app.statusError) color = ImVec4(0.92f, 0.45f, 0.40f, 1.0f);
    } else if (dirty) {
        message = ui::FormatText(
            Tr(app, pending.size() == 1 ? "{} unsaved change" : "{} unsaved changes"),
            {std::to_string(pending.size())});
    } else if (!app.configExists) {
        message = Tr(app, "No changes. Play starts with these settings.");
    } else {
        message = Tr(app, "All settings saved.");
    }
    message = Shown(app, message);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttonWidth = ImGui::GetFontSize() * 7.5f;
    const float total = buttonWidth * 4.0f + style.ItemSpacing.x * 3.0f;
    auto button = [&](const char* english, const char* id) {
        return ImGui::Button((Shown(app, Tr(app, english)) + id).c_str(), ImVec2(buttonWidth, 0.0f));
    };
    auto revert = [&]() {
        if (button("Revert", "##revert")) {
            Reload(app);
            SetStatus(app, Tr(app, "Reverted to the saved settings."));
        }
    };
    auto save = [&]() {
        if (button("Save", "##save")) Save(app);
    };
    const bool gameReady = app.gameStatus == setup::GameStatus::kOk;
    auto safe_mode = [&]() {
        ImGui::BeginDisabled(!gameReady);
        const bool pressed = button("Safe mode", "##safe");
        ImGui::EndDisabled();
        if (pressed) {
            if (Launch(app, true)) PostQuitMessage(0);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", Shown(app, Tr(app, "Starts once with the Original preset. "
                                                       "Your saved settings are kept."))
                                        .c_str());
        }
    };
    auto play = [&]() {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.13f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.21f, 0.17f, 1.0f));
        ImGui::BeginDisabled(!gameReady);
        const bool pressed = button(dirty ? "Save and play" : "Play", "##play");
        ImGui::EndDisabled();
        if (pressed) {
            if (!dirty || Save(app)) {
                if (Launch(app, false)) PostQuitMessage(0);
            }
        }
        ImGui::PopStyleColor(2);
    };
    if (app.arabic) {
        // Mirrored: [Play] [Safe mode] [Save] [Revert] on the left, the
        // message on the right.
        play();
        ImGui::SameLine();
        safe_mode();
        ImGui::SameLine();
        ImGui::BeginDisabled(!dirty);
        save();
        ImGui::SameLine();
        revert();
        ImGui::EndDisabled();
        ImGui::SameLine();
        AlignRight(TextWidth(message));
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(message.c_str());
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(message.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - total);
    ImGui::BeginDisabled(!dirty);
    revert();
    ImGui::SameLine();
    save();
    ImGui::EndDisabled();
    ImGui::SameLine();
    safe_mode();
    ImGui::SameLine();
    play();
}

void DrawModal(Launcher& app) {
    if (app.openModal) {
        ImGui::OpenPopup("##modal");
        app.openModal = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 28.0f, 0.0f));
    if (ImGui::BeginPopupModal("##modal", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (app.arabic) {
            const std::string title = Shown(app, app.modalTitle);
            AlignRight(TextWidth(title));
            ImGui::TextUnformatted(title.c_str());
            ImGui::Separator();
            const float available = ImGui::GetContentRegionAvail().x;
            const float x = ImGui::GetCursorPosX();
            for (const std::string& line :
                 rex::ui::rtl::WrapVisual(app.modalText, available, TextWidth)) {
                ImGui::SetCursorPosX(x + std::max(0.0f, available - TextWidth(line)));
                ImGui::TextUnformatted(line.c_str());
            }
        } else {
            ImGui::TextUnformatted(app.modalTitle.c_str());
            ImGui::Separator();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(app.modalText.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        if (ImGui::Button((Shown(app, Tr(app, "OK")) + "##ok").c_str(),
                          ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// A folder beside the launcher in Explorer (created when it does not exist yet).
void OpenFolder(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Version from the package manifest (package.ps1), empty for other builds.
std::string ReadPackageVersion(const std::filesystem::path& directory) {
    try {
        const toml::table manifest =
            toml::parse_file((directory / L"TheDarkness.package.toml").u8string());
        return manifest["package_version"].value_or(std::string{});
    } catch (...) {
        return {};
    }
}

// Copies the saves of another (older) folder into the saves folder in use;
// the saves and settings there are kept under a new name.
void ImportSaves(Launcher& app, const std::filesystem::path& source) {
    bool running = true;
    try {
        running = RuntimeSingleInstance::Exists(kRuntimeTitleLockName);
    } catch (...) {
    }
    if (running) {
        ShowModal(app, Tr(app, "Close the game first"),
                  Tr(app, "Saves can be imported while the game is not running."));
        return;
    }
    const RuntimeUserDataCopy copy = RuntimeImportUserData(app.userData, source);
    std::string text = copy.ok ? std::string{} : copy.error;
    for (const std::string& line : copy.lines) text += (text.empty() ? "" : "\n") + line;
    app.userData = RuntimeDetectUserDataLayout(app.directory);
    app.dataDirectory = app.userData.localData;
    app.activeConfig = app.userData.configPath;
    app.userDataNotice.clear();
    Reload(app);
    ShowModal(app, Tr(app, copy.ok ? "Saves imported" : "Saves not imported"), text);
}

// One-time notices about the saves folder: the copy this start made, or
// saves of an older version that were left in this folder.
void DrawUserDataBar(Launcher& app) {
    const bool leftBehind = app.userData.gameFolderSavesLeftBehind;
    if (app.userDataNotice.empty() && !leftBehind) return;
    const float button = ImGui::GetFontSize() * 13.0f;
    ImGui::Spacing();
    if (!app.userDataNotice.empty()) {
        if (app.userDataNoticeError) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.45f, 0.40f, 1.0f));
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(Shown(app, app.userDataNotice).c_str());
        ImGui::PopTextWrapPos();
        if (app.userDataNoticeError) ImGui::PopStyleColor();
        if (ImGui::Button((Shown(app, Tr(app, "Open saves folder")) + "##savesopen").c_str(),
                          ImVec2(button, 0.0f))) {
            OpenFolder(app.userData.root);
        }
        ImGui::SameLine();
        if (ImGui::Button((Shown(app, Tr(app, "OK")) + "##savesok").c_str(),
                          ImVec2(button * 0.5f, 0.0f))) {
            app.userDataNotice.clear();
        }
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(
            Shown(app, ui::FormatText(
                           Tr(app, "This folder has saves of an older version that were not "
                                   "copied, because {} already has saves."),
                           {app.userData.root.u8string()}))
                .c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button((Shown(app, Tr(app, "Use this folder's saves")) + "##savesuse").c_str(),
                          ImVec2(button, 0.0f))) {
            ImportSaves(app, app.directory);
        }
        ImGui::SameLine();
        if (ImGui::Button((Shown(app, Tr(app, "Keep the current saves")) + "##saveskeep").c_str(),
                          ImVec2(button, 0.0f))) {
            RuntimeKeepGameFolderUserData(app.userData);
            app.userData = RuntimeDetectUserDataLayout(app.directory);
            app.dataDirectory = app.userData.localData;
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
}

void DrawAbout(Launcher& app) {
    if (app.openAbout) {
        ImGui::OpenPopup("##about");
        app.openAbout = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, 0.0f));
    if (!ImGui::BeginPopupModal("##about", nullptr,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const float width = ImGui::GetContentRegionAvail().x;
    auto paragraph = [&](const std::string& text) {
        if (app.arabic) {
            const float x = ImGui::GetCursorPosX();
            for (const std::string& line : rex::ui::rtl::WrapVisual(text, width, TextWidth)) {
                ImGui::SetCursorPosX(x + std::max(0.0f, width - TextWidth(line)));
                ImGui::TextUnformatted(line.c_str());
            }
        } else {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
    };
    const std::string title = Shown(app, Tr(app, "About"));
    if (app.arabic) AlignRight(TextWidth(title));
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Spacing();
    paragraph(ui::FormatText(
        Tr(app, "{} runs The Darkness (Xbox 360) as a native Windows program, recompiled from "
                "your own copy of the game. It is a fan project, not affiliated with or endorsed "
                "by the game's publishers or rights holders."),
        {DARKNESS_PRODUCT_NAME}));
    if (!app.packageVersion.empty()) {
        paragraph(ui::FormatText(Tr(app, "Version {}"), {app.packageVersion}));
    }
    paragraph(Tr(app, "Built with ReXGlue (BSD-3-Clause, based on Xenia), XenonRecomp (MIT), "
                      "Dear ImGui, SDL3, toml++, FFmpeg and libmspack (LGPL-2.1) and other "
                      "open-source components."));
    // The optional upscaler runtimes that are in this folder.
    std::error_code error;
    const auto present = [&](const wchar_t* name) {
        return std::filesystem::is_regular_file(app.directory / name, error);
    };
    std::string upscalers;
    for (const auto& [file, name] :
         {std::pair{L"amd_fidelityfx_dx12.dll", "AMD FidelityFX SDK (FSR 3.1)"},
          std::pair{L"libxess.dll", "Intel XeSS"}, std::pair{L"nvngx_dlss.dll", "NVIDIA DLSS"}}) {
        if (!present(file)) continue;
        if (!upscalers.empty()) upscalers += Tr(app, ", ");
        upscalers += name;
    }
    if (!upscalers.empty()) {
        paragraph(ui::FormatText(Tr(app, "Upscalers in this folder: {}."), {upscalers}));
    }
    if (present(L"nvngx_dlss.dll")) {
        paragraph(Tr(app, "NVIDIA DLSS: this program uses NVIDIA DLSS. NVIDIA, GeForce RTX and "
                          "NVIDIA DLSS are trademarks of NVIDIA Corporation."));
    }
    paragraph(Tr(app, "The licence texts are in the licenses folder. Crash reports "
                      "(runtime_crash.log and .dmp files) are in the logs folder: attach them "
                      "to a bug report."));
    paragraph(app.userData.mode == RuntimeUserDataMode::kPerUser
                  ? ui::FormatText(Tr(app, "Saves and settings: {}"), {app.userData.root.u8string()})
                  : ui::FormatText(Tr(app, "Saves and settings: in this folder ({})."),
                                   {app.userData.mode == RuntimeUserDataMode::kPortable
                                        ? std::string("portable.txt")
                                        : app.userData.reason}));
    {
        const float wide = ImGui::GetFontSize() * 11.0f;
        if (app.arabic) AlignRight(wide * 2.0f + ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button((Shown(app, Tr(app, "Open saves folder")) + "##aboutsaves").c_str(),
                          ImVec2(wide, 0.0f))) {
            OpenFolder(app.userData.root);
        }
        ImGui::SameLine();
        if (ImGui::Button((Shown(app, Tr(app, "Import saves...")) + "##aboutimport").c_str(),
                          ImVec2(wide, 0.0f))) {
            if (const auto picked = PickPath(app, Pick::kSavesFolder)) {
                ImGui::CloseCurrentPopup();
                ImportSaves(app, *picked);
            }
        }
        ImGui::Spacing();
    }
    const float button = ImGui::GetFontSize() * 7.5f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    auto licences = [&] {
        if (ImGui::Button((Shown(app, Tr(app, "Licences")) + "##licences").c_str(),
                          ImVec2(button, 0.0f))) {
            OpenFolder(app.directory / L"licenses");
        }
    };
    auto logs = [&] {
        if (ImGui::Button((Shown(app, Tr(app, "Logs")) + "##logs").c_str(), ImVec2(button, 0.0f))) {
            OpenFolder(app.dataDirectory / L"logs");
        }
    };
    auto ok = [&] {
        if (ImGui::Button((Shown(app, Tr(app, "OK")) + "##aboutok").c_str(),
                          ImVec2(button, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
    };
    if (app.arabic) {
        AlignRight(button * 3.0f + spacing * 2.0f);
        ok();
        ImGui::SameLine();
        logs();
        ImGui::SameLine();
        licences();
    } else {
        licences();
        ImGui::SameLine();
        logs();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - button);
        ok();
    }
    ImGui::EndPopup();
}

void DrawFrame(Launcher& app) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    // The language setting switches the launcher at once (the game and its
    // overlay use it from their next start).
    const bool arabic = app.offer.arabic &&
                        PcSettingsInterfaceArabic(ui::ValueOf(app.model, "general.language"));
    if (arabic != app.arabic || !(app.offer == app.schemaOffer)) {
        app.arabic = arabic;
        app.schemaOffer = app.offer;
        app.schema = BuildPcSettingsUiSchema(arabic, app.offer);
        app.model.schema = &app.schema;
        app.detectedFor.clear();
        DetectTexturePacks(app);
        DescribeSaved(app);
    }
    UpdateDetections(app);
    PollGameSetup(app);
    PollLanguagePack(app);
    DrawHeader(app);
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    ImGui::BeginChild("##panel", ImVec2(0.0f, -footer), ImGuiChildFlags_None);
    ui::DetectUpscalers(app.model, app.upscalers);
    if (app.gameStatus != setup::GameStatus::kOk) {
        DrawGameSetup(app);
    } else {
        DrawUserDataBar(app);
        DrawLanguagePackBar(app);
        const auto changes = ui::DrawPanel(app.model, ui::Surface::kLauncher);
        if (!changes.empty()) {
            SetStatus(app, {});
            app.presetEdited = true;
        }
    }
    ImGui::EndChild();
    DrawFooter(app);
    DrawModal(app);
    DrawAbout(app);
    ImGui::End();
}

// ---- Direct3D 11 ----

void CreateTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_target);
        back->Release();
    }
}

void ReleaseTarget() {
    if (g_target) {
        g_target->Release();
        g_target = nullptr;
    }
}

bool CreateDevice(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                                   levels, 2, D3D11_SDK_VERSION, &desc,
                                                   &g_swapChain, &g_device, &level, &g_context);
    if (result == DXGI_ERROR_UNSUPPORTED) {
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels,
                                               2, D3D11_SDK_VERSION, &desc, &g_swapChain,
                                               &g_device, &level, &g_context);
    }
    if (FAILED(result)) return false;
    CreateTarget();
    return true;
}

// Developer review: the presented image as a 32-bit BMP (DARKNESS_LAUNCHER_SCREENSHOT).
bool SaveBackBuffer(const std::filesystem::path& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool saved = false;
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &staging)) && staging) {
        g_context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint32_t width = desc.Width, height = desc.Height;
            BITMAPFILEHEADER file{};
            BITMAPINFOHEADER info{};
            info.biSize = sizeof(info);
            info.biWidth = LONG(width);
            info.biHeight = -LONG(height);
            info.biPlanes = 1;
            info.biBitCount = 32;
            info.biCompression = BI_RGB;
            file.bfType = 0x4D42;
            file.bfOffBits = sizeof(file) + sizeof(info);
            file.bfSize = file.bfOffBits + width * height * 4;
            if (FILE* out = _wfopen(path.c_str(), L"wb")) {
                std::fwrite(&file, sizeof(file), 1, out);
                std::fwrite(&info, sizeof(info), 1, out);
                std::vector<uint8_t> row(size_t(width) * 4);
                for (uint32_t y = 0; y < height; ++y) {
                    const uint8_t* source =
                        static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
                    for (uint32_t x = 0; x < width; ++x) {  // RGBA -> BGRA
                        row[x * 4 + 0] = source[x * 4 + 2];
                        row[x * 4 + 1] = source[x * 4 + 1];
                        row[x * 4 + 2] = source[x * 4 + 0];
                        row[x * 4 + 3] = source[x * 4 + 3];
                    }
                    std::fwrite(row.data(), 1, row.size(), out);
                }
                std::fclose(out);
                saved = true;
            }
            g_context->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
    return saved;
}

void DestroyDevice() {
    ReleaseTarget();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swapChain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) return TRUE;
    switch (message) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_resizeWidth = LOWORD(lParam);
                g_resizeHeight = HIWORD(lParam);
            }
            return 0;
        case WM_DPICHANGED: {
            g_dpiScale = HIWORD(wParam) / 96.0f;
            g_dpiChanged = true;
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = LONG(760 * g_dpiScale);
            info->ptMinTrackSize.y = LONG(520 * g_dpiScale);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ApplyScale(Launcher& app) {
    ui::ApplyStyle(g_dpiScale);
    ImGui::GetStyle().FontScaleDpi = g_dpiScale;
    (void)app;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    ImGui_ImplWin32_EnableDpiAwareness();

    Launcher app;
    app.directory = ExecutableDirectory();
    // Saves and settings live in Saved Games (0.9.1). The first start after
    // an update from 0.9.0 copies them there from this folder (not while the
    // game runs: it may be saving). The local data folder (logs, packs, the
    // game-location file) is known before the offer scans it for packs.
    app.userData = RuntimeDetectUserDataLayout(app.directory);
    app.dataDirectory = app.userData.localData;
    {
        // It may not exist yet on an installed copy's first start.
        std::error_code ignore;
        std::filesystem::create_directories(app.dataDirectory, ignore);
    }
    app.offer = PcSettingsOfferFor(app.dataDirectory, app.directory, kLanguagePackUrl[0] != '\0');
    app.schemaOffer = app.offer;
    app.schema = BuildPcSettingsUiSchema(false, app.offer);
    app.model.schema = &app.schema;
    app.runtimeExecutable = app.directory / L"TheDarkness.exe";
    app.presetDirectory = app.directory / L"presets";
    bool gameRunning = true;
    try {
        gameRunning = RuntimeSingleInstance::Exists(kRuntimeTitleLockName);
    } catch (...) {
    }
    if (app.userData.migrationPending && !gameRunning) {
        RuntimeUserDataCopy copy;
        app.userData = RuntimeMigrateGameFolderUserData(app.userData, copy);
        app.dataDirectory = app.userData.localData;
        if (!copy.ok) {
            app.userDataNotice = ui::FormatText(
                Tr(app, "Your saves and settings could not be copied to {}: {}. The game keeps "
                        "using this folder and tries again at its next start."),
                {app.userData.perUserRoot.u8string(), copy.error});
            app.userDataNoticeError = true;
        } else if (copy.files) {
            app.userDataNotice = ui::FormatText(
                Tr(app, "Your saves and settings were copied to {} and every copy was checked. "
                        "The game uses them from there now; the originals in this folder stay "
                        "unchanged as a backup."),
                {app.userData.root.u8string()});
        }
    }
    app.activeConfig = app.userData.configPath;
    app.exampleConfig = app.directory / L"TheDarkness.pc.example.toml";
    try {
        app.presets = EnumeratePcPresetOptions(app.presetDirectory);
    } catch (...) {
    }
    // Name exactly what is missing (#2: a Steam Deck extraction left the presets
    // folder out); the launcher must stay beside the runtime, its example
    // configuration and the presets.
    std::wstring missing;
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(app.runtimeExecutable, fileError)) {
        missing += L"\n  - TheDarkness.exe";
    }
    if (!std::filesystem::is_regular_file(app.exampleConfig, fileError)) {
        missing += L"\n  - TheDarkness.pc.example.toml";
    }
    if (app.presets.empty()) {
        missing += std::filesystem::is_directory(app.presetDirectory, fileError)
                       ? L"\n  - the presets in the presets folder (empty or unreadable)"
                       : L"\n  - the presets folder";
    }
    if (!missing.empty()) {
        const std::wstring text =
            L"Some files of " DARKNESS_PRODUCT_NAME " are missing next to the settings launcher:" +
            missing +
            L"\n\nExtract the whole zip again into an empty folder and keep its folders "
            L"(presets, licenses).\n\nFolder: " + app.directory.wstring();
        MessageBoxW(nullptr, text.c_str(), kWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!windowClass.hIcon) windowClass.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1080, 760, nullptr, nullptr,
                                  instance, nullptr);
    if (!window) return 1;
    // Size for the window's own monitor scale (physical pixels), within the
    // work area, centred.
    g_dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const RECT& work = monitor.rcWork;
        const int width = std::min(int(1080 * g_dpiScale), int(work.right - work.left));
        const int height = std::min(int(780 * g_dpiScale), int(work.bottom - work.top));
        SetWindowPos(window, nullptr, work.left + (work.right - work.left - width) / 2,
                     work.top + (work.bottom - work.top - height) / 2, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!CreateDevice(window)) {
        MessageBoxW(nullptr, L"Direct3D 11 is unavailable, so the settings window cannot open.",
                    kWindowTitle, MB_ICONERROR | MB_OK);
        DestroyDevice();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    wchar_t windowsDirectory[MAX_PATH]{};
    GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    const std::filesystem::path fonts = std::filesystem::path(windowsDirectory) / L"Fonts";
    // Latin plus Arabic and its presentation forms (the Arabic interface draws
    // pre-shaped text, rex/ui/rtl_text.h).
    static const ImWchar kGlyphRanges[] = {0x0020, 0x024F, 0x0600, 0x06FF, 0x2000, 0x206F,
                                           0xFB50, 0xFDFF, 0xFE70, 0xFEFF, 0};
    ImFont* body = nullptr;
    if (std::filesystem::is_regular_file(fonts / L"segoeui.ttf")) {
        body = io.Fonts->AddFontFromFileTTF((fonts / L"segoeui.ttf").string().c_str(), 17.0f,
                                            nullptr, kGlyphRanges);
    }
    if (!body) io.Fonts->AddFontDefault();
    if (std::filesystem::is_regular_file(fonts / L"segoeuib.ttf")) {
        app.titleFont = io.Fonts->AddFontFromFileTTF((fonts / L"segoeuib.ttf").string().c_str(),
                                                     17.0f, nullptr, kGlyphRanges);
    }
    ImGui::GetStyle().FontSizeBase = 17.0f;
    ApplyScale(app);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    app.onSteamDeck = IsRunningOnSteamDeck() || std::getenv("DARKNESS_LAUNCHER_STEAM_DECK");
    DetectDisplays(app);
    DetectTexturePacks(app);
    Reload(app);
    CheckGame(app);
    app.packageVersion = ReadPackageVersion(app.directory);
    app.upscalers = ui::ScanUpscalerRuntimes(app.directory);
    if (app.upscalers.dlss) app.upscalers.dlss_card = HasNvidiaRtxCard();
    if (app.onSteamDeck && !app.configExists && app.presetCombo >= 0) {
        // On a Deck the first save installs the Steam Deck preset.
        LoadPreset(app, app.presetCombo);
    }
    bool running_test_edit = false;
    // Developer review: DARKNESS_LAUNCHER_DEBUG=<section>[:advanced] opens a
    // section (and the advanced view) and records the scale for three frames.
    if (const char* debug = std::getenv("DARKNESS_LAUNCHER_DEBUG")) {
        app.model.section = size_t(std::max(0, std::atoi(debug)));
        app.model.show_advanced = std::strchr(debug, ':') != nullptr;
        app.openAbout = std::strstr(debug, "about") != nullptr;
        if (FILE* log = _wfopen((app.directory / L"launcher_debug.txt").c_str(), L"a")) {
            std::fprintf(log, "defaults=%zu saved=%zu window_mode default=%s saved=%s\n",
                         app.model.defaults.size(), app.model.saved.size(),
                         ui::ValueOf(ui::PanelModel{&app.schema, {}, {}, app.model.defaults},
                                     "window_mode").c_str(),
                         ui::ValueOf(app.model, "window_mode").c_str());
            std::fclose(log);
        }
    }
    // Developer check of the Save path: DARKNESS_LAUNCHER_TEST_EDIT=
    // "key=value;key=value" edits, saves exactly like the Save button, records
    // the result in launcher_debug.txt and exits.
    // Developer check of the pack install: DARKNESS_LAUNCHER_INSTALL_PACK=
    // <archive> installs it like "Install from file" (synchronously) and
    // records the result in launcher_debug.txt.
    if (const char* archive = std::getenv("DARKNESS_LAUNCHER_INSTALL_PACK")) {
        const auto result = darkness::language_install::InstallPack(
            std::filesystem::u8path(archive), app.gameFolder, app.directory, nullptr);
        DetectTexturePacks(app);
        if (FILE* log = _wfopen((app.directory / L"launcher_debug.txt").c_str(), L"a")) {
            std::fprintf(log, "install_pack ok=%u language=%s message=%s detail=%s installed=%u\n",
                         result.ok ? 1u : 0u, result.language.c_str(), result.message.c_str(),
                         result.detail.c_str(), app.arabicPackInstalled ? 1u : 0u);
            std::fclose(log);
        }
    }
    if (const char* edits = std::getenv("DARKNESS_LAUNCHER_TEST_EDIT")) {
        std::string list = edits;
        size_t start = 0;
        while (start < list.size()) {
            size_t end = list.find(';', start);
            if (end == std::string::npos) end = list.size();
            const std::string pair = list.substr(start, end - start);
            const size_t equals = pair.find('=');
            if (equals != std::string::npos) {
                app.model.values[pair.substr(0, equals)] = pair.substr(equals + 1);
            }
            start = end + 1;
        }
        const bool saved = Save(app);
        if (FILE* log = _wfopen((app.directory / L"launcher_debug.txt").c_str(), L"a")) {
            std::fprintf(log, "test_edit saved=%u status=%s modal=%s %s\n", saved ? 1u : 0u,
                         app.status.c_str(), app.modalTitle.c_str(), app.modalText.c_str());
            std::fclose(log);
        }
        running_test_edit = true;
    }

    bool running = !running_test_edit;
    while (running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(window)) {
            Sleep(20);
            continue;
        }
        if (g_resizeWidth && g_resizeHeight) {
            ReleaseTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            CreateTarget();
        }
        if (g_dpiChanged) {
            ApplyScale(app);
            g_dpiChanged = false;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (static int logged = 0; logged < 3 && std::getenv("DARKNESS_LAUNCHER_DEBUG")) {
            ++logged;
            RECT client{};
            GetClientRect(window, &client);
            if (FILE* log = _wfopen((app.directory / L"launcher_debug.txt").c_str(), L"a")) {
                std::fprintf(log, "scale=%.3f dpi=%u client=%ldx%ld display=%.0fx%.0f font=%.1f\n",
                             g_dpiScale, GetDpiForWindow(window), client.right, client.bottom,
                             ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y,
                             ImGui::GetStyle().FontSizeBase * ImGui::GetStyle().FontScaleDpi);
                std::fclose(log);
            }
        }
        ImGui::NewFrame();
        DrawFrame(app);
        ImGui::Render();
        const float clear[4] = {0.05f, 0.05f, 0.055f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // Developer review: DARKNESS_LAUNCHER_SCREENSHOT=<file.bmp> saves the
        // window after a few frames (layout settled) and exits.
        if (const char* shot = std::getenv("DARKNESS_LAUNCHER_SCREENSHOT")) {
            static int frames = 0;
            if (++frames == 20) {
                SaveBackBuffer(std::filesystem::path(Utf8ToWide(shot)));
                running = false;
            }
        }
        g_swapChain->Present(1, 0);
    }

    StopGameSetup(app);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyDevice();
    DestroyWindow(window);
    UnregisterClassW(kWindowClass, instance);
    return 0;
}
