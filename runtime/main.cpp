#include "runtime_harness.h"
#include "runtime_audio.h"
#include "runtime_diagnostics.h"
#include "runtime_filesystem.h"
#include "runtime_fatal.h"
#include "runtime_frame_wait.h"
#include "runtime_function_trace.h"
#include "runtime_graphics.h"
#include "runtime_pc_config_snapshot.h"
#include "runtime_graphics_cache.h"
#include "runtime_input.h"
#include "runtime_job_poll_wake.h"
#include "runtime_mouse_look.h"
#include "runtime_stick_look.h"
#include "runtime_widescreen.h"
#include "runtime_language_pack.h"
#include "runtime_button_prompts.h"
#include "pc_settings_ui.h"
#include "product_name.h"
#include "runtime_settings_service.h"
#include "runtime_movement_packet.h"
#include "runtime_modules.h"
#include "runtime_mods.h"
#include "runtime_pc_settings.h"
#include "runtime_auto_scale.h"
#include "runtime_gpu_calibration.h"
#include "runtime_game_setup.h"
#include "runtime_package_validation.h"
#include "runtime_single_instance.h"
#include "runtime_source_memory_coordinator.h"
#include "runtime_owned_camera_mode.h"
#include "runtime_threads.h"
#include "runtime_user_data.h"
#include "runtime_xam.h"
#include "ppc_recomp_shared.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <shellapi.h>
#include <io.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <image.h>

namespace {
constexpr uint32_t kInitialStack = 0x70000000u;
constexpr size_t kStackSize = 0x00200000;
constexpr uint64_t kGuestAddressSpace = 0x100000000ull;
constexpr uint64_t kGuestBackingSize = 0x120000000ull;

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_miniDumpWriteDump{};
wchar_t g_crashDirectory[MAX_PATH]{};  // "<executable directory>\logs\"

LONG WINAPI CrashEvidence(EXCEPTION_POINTERS* exception) {
    if (!exception || !exception->ExceptionRecord || !exception->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto* record = exception->ExceptionRecord;
    std::array<char, 4096> message{};
    HMODULE faultModule{};
    wchar_t faultModulePath[MAX_PATH]{};
    uintptr_t faultModuleBase{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(record->ExceptionAddress), &faultModule)) {
        faultModuleBase = reinterpret_cast<uintptr_t>(faultModule);
        GetModuleFileNameW(faultModule, faultModulePath, MAX_PATH);
    }
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    const uintptr_t faultAddress = record->NumberParameters >= 2
        ? record->ExceptionInformation[1]
        : 0;
    const char* access = "n/a";
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 1) {
        access = record->ExceptionInformation[0] == 0 ? "read"
            : record->ExceptionInformation[0] == 1 ? "write"
            : record->ExceptionInformation[0] == 8 ? "execute" : "unknown";
    }
    _snprintf_s(
        message.data(), message.size(), _TRUNCATE,
        "HOST_EXCEPTION utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid=%lu tid=%lu "
        "code=0x%08lx ip=0x%p access=%s address=0x%p module_base=0x%p "
        "module_offset=0x%llx module=%ls\r\n",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
        utc.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(),
        record->ExceptionCode,
        reinterpret_cast<void*>(exception->ContextRecord->Rip),
        access, reinterpret_cast<void*>(faultAddress),
        reinterpret_cast<void*>(faultModuleBase),
        faultModuleBase
            ? static_cast<unsigned long long>(
                  reinterpret_cast<uintptr_t>(record->ExceptionAddress) - faultModuleBase)
            : 0ull,
        faultModulePath);
    RuntimeAppendPpcCrashEvidence(message.data(), message.size());
    DWORD written{};
    const DWORD messageBytes = static_cast<DWORD>(std::strlen(message.data()));
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message.data(), messageBytes, &written, nullptr);
    // Beside the executable when known (a shortcut or launcher may start the
    // game in another working directory); relative logs\ otherwise.
    wchar_t logPath[MAX_PATH]{};
    if (g_crashDirectory[0]) {
        CreateDirectoryW(g_crashDirectory, nullptr);
        _snwprintf_s(logPath, _TRUNCATE, L"%lsruntime_crash.log", g_crashDirectory);
    } else {
        CreateDirectoryW(L"logs", nullptr);
        wcscpy_s(logPath, L"logs\\runtime_crash.log");
    }
    const HANDLE crashLog = CreateFileW(logPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (crashLog != INVALID_HANDLE_VALUE) {
        WriteFile(crashLog, message.data(), messageBytes, &written, nullptr);
    }
    // A small minidump (threads, stacks and the memory they reference; not
    // the guest address space) makes a tester's crash actionable.
    if (g_miniDumpWriteDump && g_crashDirectory[0]) {
        wchar_t dumpPath[MAX_PATH]{};
        _snwprintf_s(dumpPath, _TRUNCATE,
                     L"%lsTheDarkness_crash_%04u%02u%02u_%02u%02u%02u_%lu.dmp", g_crashDirectory,
                     utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
                     GetCurrentProcessId());
        const HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        BOOL dumped = FALSE;
        if (dump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, FALSE};
            dumped = g_miniDumpWriteDump(
                GetCurrentProcess(), GetCurrentProcessId(), dump,
                static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                           MiniDumpWithThreadInfo |
                                           MiniDumpWithUnloadedModules),
                &info, nullptr, nullptr);
            CloseHandle(dump);
        }
        std::array<char, MAX_PATH * 2 + 64> dumpLine{};
        _snprintf_s(dumpLine.data(), dumpLine.size(), _TRUNCATE,
                    "HOST_EXCEPTION_MINIDUMP written=%d path=%ls\r\n", dumped ? 1 : 0,
                    dumpPath);
        const DWORD dumpLineBytes = static_cast<DWORD>(std::strlen(dumpLine.data()));
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), dumpLine.data(), dumpLineBytes, &written,
                  nullptr);
        if (crashLog != INVALID_HANDLE_VALUE) {
            WriteFile(crashLog, dumpLine.data(), dumpLineBytes, &written, nullptr);
        }
    }
    if (crashLog != INVALID_HANDLE_VALUE) CloseHandle(crashLog);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Resolved before any guest code runs so the unhandled filter only formats
// and writes: the crash directory beside the executable and MiniDumpWriteDump
// from the system dbghelp.dll (never a copy beside the game).
// logsFolder: the local data folder's logs (runtime_user_data.h); beside the
// executable when it is empty.
void PrepareCrashEvidence(const std::filesystem::path& logsFolder) {
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    std::wstring directory;
    if (!logsFolder.empty()) {
        // The local data folder may not exist yet (a fresh installed copy);
        // the crash handler itself only creates the last level.
        std::error_code ignore;
        std::filesystem::create_directories(logsFolder, ignore);
        directory = logsFolder.wstring();
        if (directory.back() != L'\\') directory += L'\\';
    } else if (length && length < MAX_PATH) {
        directory.assign(executable, length);
        directory.erase(directory.find_last_of(L"\\/") + 1);
        directory += L"logs\\";
    }
    if (!directory.empty() && directory.size() < MAX_PATH) {
        wcscpy_s(g_crashDirectory, directory.c_str());
    }
    if (const HMODULE dbghelp =
            LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
            GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    }
    RuntimeFatalConfigure(g_crashDirectory, reinterpret_cast<void*>(g_miniDumpWriteDump));
}

// Opt-in play-testing log (#14): an empty session_log.txt beside
// TheDarkness.exe sends this run's console output (REX_*/RUNTIME_* lines, the
// F9 snapshot markers) to logs\session_<date>_<time>.log. Without the file
// nothing changes (a game started by the launcher has no console).
// The marker is accepted beside the executable or in the local data folder
// (an installed copy's program folder may not be writable by the player).
void OpenSessionLogIfRequested(const std::filesystem::path& dataFolder) {
    if (!g_crashDirectory[0]) return;
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    std::wstring marker(executable, length);
    marker.erase(marker.find_last_of(L"\\/") + 1);
    marker += L"session_log.txt";
    bool requested = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!requested && !dataFolder.empty()) {
        requested = GetFileAttributesW((dataFolder / L"session_log.txt").c_str()) !=
                    INVALID_FILE_ATTRIBUTES;
    }
    if (!requested) return;
    CreateDirectoryW(g_crashDirectory, nullptr);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t path[MAX_PATH]{};
    _snwprintf_s(path, _TRUNCATE, L"%lssession_%04u%02u%02u_%02u%02u%02u.log", g_crashDirectory,
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const HANDLE created = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created == INVALID_HANDLE_VALUE) return;
    CloseHandle(created);
    // stderr appends to the file. stdout, when this process has one, shares
    // its descriptor (never a second open of the file: a failed reopen would
    // leave stdout closed, and the C runtime stops the process at its next
    // write); a game started by the launcher has no stdout and keeps none.
    FILE* errorStream = nullptr;
    if (_wfreopen_s(&errorStream, path, L"a", stderr) != 0 || !errorStream) return;
    const int errorDescriptor = _fileno(stderr);
    const int outputDescriptor = _fileno(stdout);
    if (errorDescriptor >= 0 && outputDescriptor >= 0) {
        std::fflush(stdout);
        if (_dup2(errorDescriptor, outputDescriptor) == 0) {
            SetStdHandle(STD_OUTPUT_HANDLE,
                         reinterpret_cast<HANDLE>(_get_osfhandle(outputDescriptor)));
        }
    }
    if (errorDescriptor >= 0) {
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(errorDescriptor)));
    }
    std::fprintf(stderr, "RUNTIME_SESSION_LOG path=%ls\n", path);
    std::fflush(stderr);
}

// The settings launcher reads and writes settings by running this executable
// with one of these actions and reading its standard output. The session log
// must leave that output alone: with session_log.txt present (0.9.2-0.9.3) the
// launcher read nothing and showed every setting empty, key bindings "(none)".
bool IsLauncherSettingsAction(int argc, char** argv) {
    static constexpr std::string_view kActions[] = {
        "--help",          "-h",           "--print-paths",   "--list-presets",
        "--print-capabilities", "--validate-config", "--inspect-config",
        "--validate-mods", "--verify-package", "--edit-config", "--install-preset"};
    for (int index = 1; index < argc; ++index) {
        if (!argv[index]) continue;
        const std::string_view argument(argv[index]);
        if (argument.rfind("--install-preset=", 0) == 0) return true;
        for (const std::string_view action : kActions) {
            if (argument == action) return true;
        }
    }
    return false;
}

using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
    MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T,
    ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

template <typename T>
T ResolveMemoryApi(const char* name) {
    const auto module = GetModuleHandleW(L"KernelBase.dll");
    auto* function = module ? GetProcAddress(module, name) : nullptr;
    if (!function) function = GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), name);
    if (!function) throw std::runtime_error("required Windows guest-memory API is unavailable");
    return reinterpret_cast<T>(function);
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("unable to open XEX");
    const auto size = stream.tellg();
    if (size <= 0) throw std::runtime_error("XEX is empty");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream) throw std::runtime_error("unable to read XEX");
    return bytes;
}

struct GuestMemory {
    uint8_t* base{};
    HANDLE backing{};
    std::vector<void*> views{};

    ~GuestMemory() {
        for (auto it = views.rbegin(); it != views.rend(); ++it) UnmapViewOfFile(*it);
        if (base) VirtualFree(base, 0, MEM_RELEASE);
        if (backing) CloseHandle(backing);
    }

    void Initialize() {
        backing = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            static_cast<DWORD>(kGuestBackingSize >> 32), static_cast<DWORD>(kGuestBackingSize), nullptr);
        if (!backing) throw std::runtime_error("unable to create guest memory backing");
        const auto virtualAlloc2 = ResolveMemoryApi<VirtualAlloc2Fn>("VirtualAlloc2");
        base = static_cast<uint8_t*>(virtualAlloc2(GetCurrentProcess(), nullptr, kGuestAddressSpace,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!base) throw std::runtime_error("unable to reserve guest virtual address space");

        Map(0x00000000, 0x40000000, 0x00000000);
        Map(0x40000000, 0x3F000000, 0x40000000);
        Map(0x7F000000, 0x01000000, 0x100000000ull);
        Map(0x80000000, 0x10000000, 0x80000000);
        Map(0x90000000, 0x10000000, 0x80000000);
        Map(0xA0000000, 0x20000000, 0x100000000ull);
        Map(0xC0000000, 0x20000000, 0x100000000ull);
        Map(0xE0000000, 0x1FD00000, 0x100001000ull);
    }

private:
    void Map(uint64_t guestAddress, uint64_t size, uint64_t backingOffset) {
        if (!VirtualFree(base + guestAddress, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            throw std::runtime_error("unable to split guest memory placeholder");
        }
        const auto mapViewOfFile3 = ResolveMemoryApi<MapViewOfFile3Fn>("MapViewOfFile3");
        auto* view = mapViewOfFile3(backing, GetCurrentProcess(), base + guestAddress, backingOffset, size,
            MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (!view) {
            throw std::runtime_error("unable to map guest memory view guest=0x" +
                std::to_string(guestAddress) + " error=" + std::to_string(GetLastError()));
        }
        views.push_back(view);
    }
};

size_t BackingImageBytes(const Image& image) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(image.data.get(), &region, sizeof(region)) || region.State != MEM_COMMIT)
        throw std::runtime_error("unable to inspect XEX backing allocation");
    const auto start = reinterpret_cast<uintptr_t>(image.data.get());
    const auto end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (end < start) throw std::runtime_error("invalid XEX backing allocation bounds");
    return static_cast<size_t>(end - start);
}

void RegisterFunctions(uint8_t* base) {
    const size_t tableBase = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    size_t count = 0;
    for (auto* mapping = PPCFuncMappings; mapping->guest != 0; ++mapping) {
        auto** slot = reinterpret_cast<PPCFunc**>(base + tableBase + (uint64_t(uint32_t(mapping->guest) - PPC_CODE_BASE) * 2));
        *slot = mapping->host;
        ++count;
    }
    std::cout << "FUNCTION_DISPATCH count=" << count << '\n';
}
}

[[noreturn]] void RuntimeTrap(const char* importName, PPCContext& context, uint8_t* base) {
    std::ostringstream trap;
    trap << "UNRESOLVED_IMPORT name=" << importName << " lr=0x" << std::hex << context.lr
         << " r3=0x" << context.r3.u64 << " r4=0x" << context.r4.u64
         << " r5=0x" << context.r5.u64 << " r6=0x" << context.r6.u64
         << " r7=0x" << context.r7.u64 << " r8=0x" << context.r8.u64
         << " r9=0x" << context.r9.u64 << std::dec;
    std::cerr << trap.str() << '\n';
    RuntimeFatalRecordDetail(trap.str());
    RuntimeFatalWriteDump("unresolved-import");
    if (std::strcmp(importName, "__imp__RtlEnterCriticalSection") == 0) {
        const uint32_t address = context.r3.u32;
        std::cerr << "CRITSEC_STATE address=0x" << std::hex << address
                  << " header=0x" << PPC_LOAD_U32(address)
                  << " lock_count=0x" << PPC_LOAD_U32(address + 16)
                  << " recursion=0x" << PPC_LOAD_U32(address + 20)
                  << " owner=0x" << PPC_LOAD_U32(address + 24)
                  << std::dec << '\n';
    }
    if (std::strcmp(importName, "__imp__NtOpenFile") == 0) {
        const uint32_t objectAttributes = context.r5.u32;
        const uint32_t root = objectAttributes ? PPC_LOAD_U32(objectAttributes) : 0;
        const uint32_t string = objectAttributes ? PPC_LOAD_U32(objectAttributes + 4) : 0;
        const uint32_t attributes = objectAttributes ? PPC_LOAD_U32(objectAttributes + 8) : 0;
        const uint16_t length = string ? PPC_LOAD_U16(string) : 0;
        const uint16_t maximumLength = string ? PPC_LOAD_U16(string + 2) : 0;
        const uint32_t buffer = string ? PPC_LOAD_U32(string + 4) : 0;
        std::string path;
        if (buffer && length <= 0x1000) path.assign(reinterpret_cast<const char*>(base + buffer), length);
        std::cerr << "NT_OPEN_FILE_EVIDENCE object_attributes=0x" << std::hex << objectAttributes
                  << " root=0x" << root << " string=0x" << string
                  << " attrs=0x" << attributes << " length=0x" << length
                  << " maximum_length=0x" << maximumLength << " buffer=0x" << buffer
                  << " path=" << path << " share=0x" << context.r7.u32
                  << " options=0x" << context.r8.u32 << std::dec << '\n';
    }
    if (std::strcmp(importName, "__imp__NtQueryDirectoryFile") == 0) {
        const uint32_t name = context.r10.u32;
        const uint16_t length = name ? PPC_LOAD_U16(name) : 0;
        const uint16_t maximumLength = name ? PPC_LOAD_U16(name + 2) : 0;
        const uint32_t buffer = name ? PPC_LOAD_U32(name + 4) : 0;
        std::string pattern;
        if (buffer && length <= 0x1000) pattern.assign(reinterpret_cast<const char*>(base + buffer), length);
        std::cerr << "NT_QUERY_DIRECTORY_EVIDENCE handle=0x" << std::hex << context.r3.u32
                  << " iosb=0x" << context.r7.u32 << " output=0x" << context.r8.u32
                  << " output_length=0x" << context.r9.u32 << " name=0x" << name
                  << " name_length=0x" << length << " name_maximum=0x" << maximumLength
                  << " name_buffer=0x" << buffer << " pattern=" << pattern
                  << " r11=0x" << context.r11.u32 << std::dec << '\n';
    }
    throw std::runtime_error(std::string("unresolved guest import ") + importName);
}

// The command line as UTF-8: paths beyond the system code page (a Saved
// Games folder under a user name in another script) survive.
std::vector<std::string> Utf8Arguments(int argc, char** argv) {
    std::vector<std::string> arguments;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide && count == argc) {
        for (int index = 0; index < count; ++index) {
            arguments.push_back(std::filesystem::path(wide[index]).u8string());
        }
    } else {
        arguments.assign(argv, argv + argc);
    }
    if (wide) LocalFree(wide);
    return arguments;
}

// Hybrid-graphics laptops: ask the NVIDIA Optimus and AMD PowerXpress
// drivers for the discrete GPU (read from the executable's exports).
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

int main(int argc, char** argv) {
    // Keep first-execution evidence intact if guest code faults before normal
    // process shutdown can flush a redirected stdout stream.
    // High-volume development traces must not flush the process-wide streams
    // after every insertion. Narrow correctness probes explicitly flush their
    // own evidence files, while crash evidence bypasses iostreams via WriteFile.
    // Record only a process-terminating, unhandled host exception. A vectored
    // first-chance hook incorrectly labels exceptions that a module later
    // handles (including injected overlays and compatibility layers) as title
    // crashes, and performs file I/O on their normal control-flow path.
    // Crash evidence and the session log go to the local data folder's logs
    // (runtime_user_data.h), decided from the folders alone.
    const RuntimeUserDataLayout startupLayout =
        RuntimeDetectUserDataLayout(RuntimeExecutableDirectory());
    PrepareCrashEvidence(startupLayout.localData / L"logs");
    if (!IsLauncherSettingsAction(argc, argv)) OpenSessionLogIfRequested(startupLayout.localData);
    SetUnhandledExceptionFilter(CrashEvidence);
    {
        // Developer check of the crash path (report line, log and minidump):
        // DARKNESS_TEST_HOST_CRASH=1 faults on purpose before any guest work.
        char testCrash[2]{};
        if (GetEnvironmentVariableA("DARKNESS_TEST_HOST_CRASH", testCrash, sizeof(testCrash)) ==
                1 &&
            testCrash[0] == '1') {
            *static_cast<volatile int*>(nullptr) = 1;
        }
    }
    GuestMemory guestMemory{};
    bool runtimeServicesStarted = false;
    // Only a game start reports a stop with a crash report and a dialog. The
    // settings actions the launcher runs without a window (inspect, install,
    // validate) return their error as text: a dialog there would wait unseen
    // behind the launcher, which waits for the action.
    bool gameStart = false;
    try {
        // Diagnostic-only and immutable for this process. Configure before any
        // module publication, allocation reset or guest worker can use a guard.
        char sourceTrackingValue[2]{};
        const bool sourceTracking = GetEnvironmentVariableA(
            "DARKNESS_SOURCE_MEMORY_TRACKING", sourceTrackingValue,
            sizeof(sourceTrackingValue)) == 1 && sourceTrackingValue[0] == '1';
        if (!RuntimeGuestSourceCoordinator().ConfigureAtStartup(sourceTracking))
            throw std::runtime_error("source memory tracking configured after first use");
        char cameraInputValue[2]{};
        const bool continuousCameraInput = GetEnvironmentVariableA(
            "DARKNESS_OWNED_CAMERA_CONTINUOUS", cameraInputValue,
            sizeof(cameraInputValue)) == 1 && cameraInputValue[0] == '1';
        if (!RuntimeOwnedCameraInputMode().ConfigureAtStartup(continuousCameraInput, sourceTracking))
            throw std::runtime_error("continuous camera input requires startup source memory tracking");
        std::cout << "OWNED_CAMERA_INPUT continuous=" << (continuousCameraInput ? 1 : 0)
                  << " scope=qualified_current_input_not_temporal_validity\n";
        std::cout << "SOURCE_MEMORY_TRACKING enabled=" << (sourceTracking ? 1 : 0)
                  << " scope=diagnostic_payload_serialization_not_gpu_camera_identity\n";
        const std::vector<std::string> utf8Arguments = Utf8Arguments(argc, argv);
        std::vector<const char*> launchArguments;
        for (const std::string& argument : utf8Arguments) launchArguments.push_back(argument.c_str());
        const std::filesystem::path executableDirectory =
            RuntimeExecutableDirectory();
        // Saves and settings: Saved Games\Estacado, the game folder with
        // portable.txt (runtime_user_data.h).
        RuntimeUserDataLayout userData = RuntimeDetectUserDataLayout(executableDirectory);
        RuntimeLaunchOptions launchOptions = ParseRuntimeLaunchOptions(
            int(launchArguments.size()), launchArguments.data(), executableDirectory, userData);
        gameStart = launchOptions.action == RuntimeLaunchAction::Run;
        if (gameStart && !launchOptions.userDataRootExplicit && userData.migrationPending) {
            // First start after an update from 0.9.0: copy the game folder's
            // saves and settings (the game folder stays in use if that fails).
            RuntimeUserDataCopy copy;
            userData = RuntimeMigrateGameFolderUserData(userData, copy);
            for (const std::string& line : copy.lines) std::cout << "USER_DATA_COPY " << line << '\n';
            if (!copy.ok) {
                std::cout << "USER_DATA_COPY failed=1 using=game_folder error=" << copy.error << '\n';
            }
            launchOptions = ParseRuntimeLaunchOptions(int(launchArguments.size()),
                                                      launchArguments.data(),
                                                      executableDirectory, userData);
        }
        if (gameStart && !launchOptions.pcConfigExplicit) {
            // 0.9.1 keyboard defaults (use on E, jump on Space): a 0.9.0
            // configuration whose two keys were never changed gets them
            // (runtime_pc_settings.h). Serialized with the launcher's saves.
            RuntimeSingleInstance configLock(RuntimeConfigLockName(launchOptions.pcConfigPath));
            if (configLock.acquired()) {
                std::string error;
                const RuntimeKeyBindingsUpgrade upgrade =
                    RuntimeUpgradePcConfigFileKeyBindings(launchOptions.pcConfigPath, &error);
                if (upgrade.changed || !error.empty()) {
                    std::cout << "PC_CONFIG_KEY_BINDINGS revision=" << kRuntimeKeyBindingsRevision
                              << " outcome=" << upgrade.outcome
                              << " swapped=" << (upgrade.swapped ? 1 : 0)
                              << (error.empty() ? std::string() : " error=" + error) << '\n';
                }
            }
        }
        const wchar_t* coverage = _wgetenv(L"DARKNESS_FUNCTION_COVERAGE");
        if (!coverage || !*coverage) coverage = _wgetenv(L"REX_FUNCTION_COVERAGE");
        if (gameStart && coverage && *coverage) {
            ConfigureRuntimeFunctionCoverage(coverage);
        }
        if (launchOptions.action == RuntimeLaunchAction::Help) {
            std::cout << RuntimeLaunchHelpText();
            return 0;
        }
        if (launchOptions.action == RuntimeLaunchAction::PrintPaths) {
            std::cout << "XEX_PATH=" << launchOptions.xexPath.u8string() << '\n'
                      << "XEX_EXISTS="
                      << (std::filesystem::is_regular_file(launchOptions.xexPath) ? 1 : 0)
                      << '\n'
                      << "PC_CONFIG_PATH=" << launchOptions.pcConfigPath.u8string() << '\n'
                      << "PC_CONFIG_EXISTS="
                      << (std::filesystem::is_regular_file(launchOptions.pcConfigPath) ? 1 : 0)
                      << '\n'
                      << "MOD_CONFIG_PATH=" << launchOptions.modsConfigPath.u8string() << '\n'
                      << "MOD_CONFIG_EXISTS="
                      << (std::filesystem::is_regular_file(launchOptions.modsConfigPath) ? 1 : 0)
                      << '\n'
                      << "USER_DATA_MODE="
                      << (launchOptions.userDataRootExplicit ? "explicit"
                                                             : RuntimeUserDataModeName(userData.mode))
                      << '\n'
                      << "USER_DATA_ROOT=" << launchOptions.userDataRoot.u8string() << '\n'
                      << "LOCAL_DATA_ROOT=" << userData.localData.u8string() << '\n'
                      << "CONTENT_ROOT="
                      << (launchOptions.userDataRoot / L"content").u8string() << '\n'
                      << "SCREENSHOT_ROOT="
                      << (launchOptions.userDataRoot / L"screenshots").u8string() << '\n';
            return 0;
        }
        if (launchOptions.action == RuntimeLaunchAction::ListPresets) {
            const auto presets = RuntimePresetPaths(executableDirectory);
            std::cout << "PRESET_ROOT="
                      << (executableDirectory / L"presets").string() << '\n';
            for (const auto& preset : presets) {
                std::cout << "PRESET=" << preset.string() << '\n';
            }
            std::cout << "PRESET_COUNT=" << presets.size() << '\n';
            return presets.empty() ? 1 : 0;
        }
        if (launchOptions.action == RuntimeLaunchAction::PrintCapabilities) {
            for (const auto& line : RuntimePcCapabilityLines()) {
                std::cout << line << '\n';
            }
            for (const auto& line : RuntimeHostDisplayLines()) {
                std::cout << line << '\n';
            }
            return 0;
        }
        const RuntimePackageValidation packageValidation =
            ValidateRuntimePackage(
                executableDirectory,
                launchOptions.action == RuntimeLaunchAction::VerifyPackage);
        if (launchOptions.action == RuntimeLaunchAction::VerifyPackage) {
            for (const auto& line :
                 RuntimePackageValidationLines(packageValidation)) {
                std::cout << line << '\n';
            }
            return packageValidation.valid ? 0 : 1;
        }
        if (!packageValidation.valid) {
            const std::string firstError = packageValidation.errors.empty()
                ? "unknown validation error"
                : packageValidation.errors.front();
            throw std::runtime_error("invalid runtime package: " + firstError +
                                     " (extract the whole zip again into an empty folder and keep "
                                     "its folders)");
        }
        if (launchOptions.action == RuntimeLaunchAction::ValidateMods) {
            try {
                const RuntimeModConfiguration mods =
                    LoadRuntimeModConfiguration(launchOptions.modsConfigPath,
                                                executableDirectory);
                for (const auto& line : RuntimeModConfigurationLines(mods)) {
                    std::cout << line << '\n';
                }
                return 0;
            } catch (const std::exception& error) {
                for (const auto& line : RuntimeModValidationErrorLines(
                         launchOptions.modsConfigPath, error.what())) {
                    std::cout << line << '\n';
                }
                return 1;
            }
        }
        // Enhanced first: a player without a saved configuration (and without
        // --pc-config/--preset) runs the packaged defaults, the example
        // configuration beside the executable. The first in-game settings
        // save creates TheDarkness.pc.toml from them.
        const auto packagedDefaultsPath =
            executableDirectory / L"TheDarkness.pc.example.toml";
        bool startupFromPackagedDefaults = false;
        const auto startupPcConfig = [&] {
            auto snapshot = RuntimePcConfigSnapshot::Capture(launchOptions.pcConfigPath);
            if (!snapshot.present() && !launchOptions.pcConfigExplicit &&
                launchOptions.action == RuntimeLaunchAction::Run) {
                auto defaults = RuntimePcConfigSnapshot::Capture(packagedDefaultsPath);
                if (defaults.present()) {
                    startupFromPackagedDefaults = true;
                    return defaults;
                }
            }
            return snapshot;
        }();
        // Startup consumers read the snapshot of the file actually used.
        const std::filesystem::path startupPcConfigPath =
            startupFromPackagedDefaults ? packagedDefaultsPath : launchOptions.pcConfigPath;
        const RuntimePcConfigValidation pcConfigValidation =
            ValidateRuntimePcConfig(
                startupPcConfigPath,
                launchOptions.pcConfigExplicit ||
                    launchOptions.action == RuntimeLaunchAction::ValidateConfig ||
                    launchOptions.action == RuntimeLaunchAction::InspectConfig,
                &startupPcConfig);
        if (launchOptions.action == RuntimeLaunchAction::ValidateConfig) {
            for (const auto& line : RuntimePcConfigValidationLines(
                     launchOptions.pcConfigPath, pcConfigValidation)) {
                std::cout << line << '\n';
            }
            return pcConfigValidation.valid ? 0 : 1;
        }
        if (launchOptions.action == RuntimeLaunchAction::InspectConfig) {
            for (const auto& line : RuntimePcConfigInspectionLines(
                     launchOptions.pcConfigPath, pcConfigValidation)) {
                std::cout << line << '\n';
            }
            return pcConfigValidation.valid ? 0 : 1;
        }
        if (!pcConfigValidation.valid) {
            const std::string firstError = pcConfigValidation.errors.empty()
                ? "unknown validation error"
                : pcConfigValidation.errors.front();
            throw std::runtime_error("invalid PC configuration '" +
                                     startupPcConfigPath.u8string() +
                                     "': " + firstError);
        }
        for (const std::string& warning : pcConfigValidation.warnings) {
            std::cout << "PC_CONFIG_WARNING=" << warning << '\n';
        }
        // Serialize settings writers and startup setup. Release this only once
        // CPU, cache and deferred GPU consumers all own their startup bytes.
        RuntimeSingleInstance configInstance(RuntimeConfigLockName(
            launchOptions.action == RuntimeLaunchAction::InstallPreset
                ? launchOptions.pcConfigInstallPath : launchOptions.pcConfigPath));
        if (!configInstance.acquired()) {
            throw std::runtime_error(
                "configuration is being read or saved; retry after game startup or the current settings action finishes");
        }
        if (launchOptions.action == RuntimeLaunchAction::InstallPreset) {
            try {
                const auto result = InstallRuntimePcPresetWithOverrides(
                    launchOptions.pcConfigPath,
                    launchOptions.pcConfigInstallPath,
                    launchOptions.pcConfigOverrides,
                    launchOptions.overwritePcConfig);
                for (const auto& line : RuntimePcConfigInstallLines(result)) {
                    std::cout << line << '\n';
                }
                return 0;
            } catch (const std::exception& error) {
                for (const auto& line : RuntimePcConfigInstallErrorLines(
                         launchOptions.pcConfigPath,
                         launchOptions.pcConfigInstallPath, error.what())) {
                    std::cout << line << '\n';
                }
                return 1;
            }
        }
        // Configuration-only actions above cannot launch guest work. There
        // must still be only one executing title across all package paths.
        RuntimeSingleInstance titleInstance(kRuntimeTitleLockName);
        if (!titleInstance.acquired()) {
            throw std::runtime_error(
                "another " DARKNESS_PRODUCT_NAME " title instance is already running");
        }
        std::cout << "RUNTIME_INSTANCE_LOCK acquired=1 title=0x545407EE\n";
        ConfigureGuestPortableContentDeviceRoot(
            launchOptions.userDataRoot / L"content");
        const auto startupExampleConfig = RuntimePcConfigSnapshot::Capture(
            startupPcConfig.present() ? std::filesystem::path{} :
                executableDirectory / L"TheDarkness.pc.example.toml");
        // Internal scaling needs the title's data-pass annotations: the GPU
        // receives the validated ones when the configuration raises the scale
        // without them (explicit values win; cache identity stays on the
        // startup bytes, which determine this derivation).
        // Automatic internal scale (resolution_scale = 0) becomes this run's
        // concrete scale from the output size and the graphics card.
        std::string graphicsPcConfigContents =
            startupPcConfig.present() ? startupPcConfig.contents() : std::string{};
        // Shelved temporal AA (V440): a saved choice stays off unless
        // DARKNESS_EXPERIMENTAL=temporal_aa.
        {
            std::string shelvedTemporalAa;
            graphicsPcConfigContents = RuntimePcConfigWithShelvedFeatures(
                graphicsPcConfigContents, startupPcConfig.origin().u8string(),
                PcExperimentalFeature("temporal_aa"), &shelvedTemporalAa);
            if (!shelvedTemporalAa.empty()) {
                std::cout << "PC_CONFIG_SHELVED temporal_aa=" << shelvedTemporalAa
                          << " applied=off\n";
            }
        }
        bool automaticScaleResolved = false;
        RuntimeAutoScale automaticScale{};
        if (startupPcConfig.present()) {
            const RuntimePcScaleTarget scaleTarget = RuntimePcScaleTargetFromContents(
                graphicsPcConfigContents, startupPcConfig.origin().u8string());
            if (scaleTarget.resolutionScale == 0) {
                automaticScale = RuntimeDetectAutomaticScale(scaleTarget.outputResolution,
                                                             scaleTarget.monitor);
                // V338: measured on this card (the title's menu backdrop)
                // when a measurement exists; otherwise measure this run.
                const RuntimeGpuIdentity gpu = RuntimeDetectGpuIdentity();
                const double targetFps = RuntimeAutomaticScaleTargetFps(
                    scaleTarget.frameRate, scaleTarget.frameLimit,
                    RuntimeMonitorRefreshHz(scaleTarget.monitor));
                const std::filesystem::path calibrationPath =
                    RuntimeGpuCalibrationPath(launchOptions.userDataRoot);
                const auto calibration = RuntimeLoadGpuCalibration(calibrationPath);
                if (calibration && RuntimeGpuCalibrationMatches(*calibration, gpu)) {
                    RuntimeScaleChoiceInputs inputs;
                    inputs.backdrop = calibration->backdrop;
                    inputs.outputHeight = automaticScale.outputHeight;
                    inputs.targetFps = targetFps;
                    inputs.videoMemoryBytes = gpu.videoMemoryBytes;
                    const RuntimeScaleChoice choice = RuntimeChooseMeasuredScale(inputs);
                    automaticScale.scale = choice.scale;
                    automaticScale.reason = "measured";
                    std::cout << "PC_CONFIG_AUTOMATIC_SCALE_MEASURED scale=" << choice.scale
                              << " measured_scale=" << calibration->backdrop.scale
                              << " backdrop_ms=" << calibration->backdrop.medianMs
                              << " cost_factor=" << choice.costFactor
                              << " target_fps=" << choice.targetFps
                              << " street_ms_1x=" << choice.predictedStreetMs[1]
                              << " street_ms_2x=" << choice.predictedStreetMs[2]
                              << " street_ms_3x=" << choice.predictedStreetMs[3]
                              << " holds_target=" << (choice.holdsTarget ? 1 : 0)
                              << " holds_60=" << (choice.holds60 ? 1 : 0)
                              << " reason=" << choice.reason << '\n';
                } else {
                    RuntimeGpuCalibrationArm(calibrationPath, gpu, automaticScale.scale,
                                             automaticScale.outputHeight, targetFps);
                    std::cout << "PC_CONFIG_AUTOMATIC_SCALE_CALIBRATION armed="
                              << (gpu.vendorId ? 1 : 0) << " record="
                              << (calibration ? "other_card_or_model" : "none")
                              << " target_fps=" << targetFps << " gpu=\"" << gpu.description
                              << "\"\n";
                }
                graphicsPcConfigContents = RuntimePcConfigWithAutomaticScale(
                    graphicsPcConfigContents, startupPcConfig.origin().u8string(),
                    automaticScale.scale, &automaticScaleResolved);
            }
        }
        if (automaticScaleResolved) {
            std::cout << "PC_CONFIG_AUTOMATIC_SCALE scale=" << automaticScale.scale
                      << " output=" << automaticScale.outputWidth << 'x'
                      << automaticScale.outputHeight << " video_memory_mb="
                      << (automaticScale.dedicatedVideoMemory >> 20)
                      << " reason=" << automaticScale.reason << '\n';
        }
        bool titleScaleRequirementsAdded = false;
        // V406 widescreen (experimental): a wider or taller guest mode, and the
        // matching host video mode for the presenter.
        RuntimeGuestDisplaySize guestDisplay = RuntimeGuestDisplaySizeFromEnvironment();
        if (!guestDisplay.Widescreen() && startupPcConfig.present() &&
            RuntimeWidescreenFromPcConfig(startupPcConfigPath, &startupPcConfig)) {
            const RuntimePcScaleTarget outputTarget = RuntimePcScaleTargetFromContents(
                graphicsPcConfigContents, startupPcConfig.origin().u8string());
            const RuntimeAutoScale output =
                RuntimeDetectAutomaticScale(outputTarget.outputResolution, outputTarget.monitor);
            guestDisplay =
                RuntimeGuestDisplaySizeForOutput(output.outputWidth, output.outputHeight);
            std::cout << "PC_CONFIG_WIDESCREEN output=" << output.outputWidth << 'x'
                      << output.outputHeight << " guest=" << guestDisplay.width << 'x'
                      << guestDisplay.height << '\n';
        }
        ConfigureRuntimeWidescreen(guestDisplay);
        // Language pack (general.language): configured before the graphics
        // settings, which receive its texture folder.
        const std::string languagePackName =
            RuntimeLanguagePackFromPcConfig(startupPcConfigPath, &startupPcConfig);
        RuntimeLanguagePackStatus languagePack;
        if (!languagePackName.empty()) {
            // REX_LANGUAGE_PACK_ROOT: another pack folder (developer checks).
            std::filesystem::path languagePackRoot =
                userData.localData / L"language_packs" / std::filesystem::path(languagePackName);
            {
                // A pack installed beside the executables by an earlier
                // version (before the local data folder) still loads.
                std::error_code packError;
                const std::filesystem::path besideExecutables =
                    executableDirectory / L"language_packs" /
                    std::filesystem::path(languagePackName);
                if (!std::filesystem::is_directory(languagePackRoot, packError) &&
                    std::filesystem::is_directory(besideExecutables, packError)) {
                    languagePackRoot = besideExecutables;
                }
            }
            if (const char* root = std::getenv("REX_LANGUAGE_PACK_ROOT"); root && *root) {
                languagePackRoot = std::filesystem::path(root);
            }
            languagePack = ConfigureRuntimeLanguagePack(languagePackRoot);
        }
        ConfigureRuntimeGraphicsPcConfig(
            startupPcConfig.present()
                ? startupPcConfig.WithContents(RuntimePcConfigWithProgramCacheSource(
                      RuntimePcConfigWithPromptIconSource(
                          RuntimePcConfigWithLanguageTextures(
                              RuntimePcConfigWithGuestDisplaySize(
                                  RuntimePcConfigWithTitleScaleRequirements(
                                      graphicsPcConfigContents,
                                      startupPcConfig.origin().u8string(),
                                      &titleScaleRequirementsAdded),
                                  guestDisplay),
                              languagePack.textures),
                          // Keyboard button prompts: the title's controller
                          // icons are recognised by their texels in its GUI
                          // textures.
                          launchOptions.xexPath.parent_path() / L"Content" / L"Textures" /
                              L"GUI.xtc"),
                      // First-visit stutter (#5): the shader prewarm rebuilds
                      // the packaged shader list from the title's own
                      // compiled shader cache.
                      launchOptions.xexPath.parent_path() / L"System" / L"Xenon" /
                          L"ProgramCache.xpc"))
                : startupPcConfig,
            executableDirectory);
        if (titleScaleRequirementsAdded) {
            std::cout << "PC_CONFIG_TITLE_SCALE_REQUIREMENTS added=1 threshold="
                      << kTitleScaleThreshold << " native_grid_rules="
                      << 1 + std::count(kTitleNativeGridRules.begin(), kTitleNativeGridRules.end(), ';')
                      << " native_resolve_region_tracking=1\n";
        }
        ConfigureRuntimeGraphicsScreenshotRoot(
            launchOptions.userDataRoot / L"screenshots");
        ConfigureRuntimeInputVibrationScale(
            RuntimeInputVibrationScaleFromPcConfig(startupPcConfigPath, &startupPcConfig));
        ConfigureRuntimeInputButtonMap(
            RuntimeInputButtonMapFromPcConfig(startupPcConfigPath, &startupPcConfig));
        const RuntimeControllerProfile controllerProfile =
            RuntimeControllerProfileFromPcConfig(startupPcConfigPath, &startupPcConfig);
        if (!GetGuestXamState().ConfigureControllerProfile(
                controllerProfile.sensitivity, controllerProfile.invertY)) {
            throw std::runtime_error("validated controller profile was rejected");
        }
        ConfigureRuntimeAudioMasterVolume(
            RuntimeAudioMasterVolumeFromPcConfig(startupPcConfigPath, &startupPcConfig));
        const uint32_t xboxLanguage =
            RuntimeXboxLanguageFromPcConfig(startupPcConfigPath, &startupPcConfig);
        if (!GetGuestXamState().ConfigureLanguage(xboxLanguage)) {
            throw std::runtime_error("validated Xbox language was rejected");
        }
        // CPU values and the deferred graphics copy are owned now. The later
        // cache builder also receives these immutable snapshots. A serialized
        // Save may replace the next-start file without changing this instance.
        configInstance.Release();
        std::cout << "PC_CONFIG_OWNERSHIP source=owned_startup_snapshot "
                     "next_start_save=enabled current_run=unchanged\n";
        ConfigureRuntimeFrameCadenceDiagnostics(
            launchOptions.frameCadenceDiagnostics);
        // Command-line timing flags (isolated tests) take precedence over the
        // player-facing display.frame_rate setting.
        const bool configHostPacing = RuntimeFrameRateUsesHostPacingFromPcConfig(
            startupPcConfigPath, &startupPcConfig);
        const bool timingFromCommandLine = launchOptions.oneVblankExperiment ||
                                           launchOptions.immediateDeadlineExperiment;
        const bool immediateDeadline = launchOptions.immediateDeadlineExperiment ||
            (!timingFromCommandLine && configHostPacing);
        ConfigureRuntimeOneVblankExperiment(launchOptions.oneVblankExperiment,
                                           immediateDeadline);
        std::cout << "FRAME_RATE_TIMING source="
                  << (timingFromCommandLine ? "command_line" : "config")
                  << " host_pacing=" << (immediateDeadline ? 1 : 0)
                  << " one_vblank=" << (launchOptions.oneVblankExperiment ? 1 : 0) << '\n';
        ConfigureRuntimeMovementPacketCompaction(launchOptions.movementPacketCompaction);
        ConfigureRuntimeHitchDiagnostics(launchOptions.hitchDiagnostics);
        ConfigureRuntimeAudioDiagnostics(launchOptions.audioDiagnostics);
        std::cout << "HOST_START\n";
        std::cout << "MOVEMENT_PACKET_COMPACTION enabled="
                  << launchOptions.movementPacketCompaction
                  << " status=DEFAULT_V367_ANALOG_VALIDATED\n";
        std::cout << "EXPERIMENT_IMMEDIATE_DEADLINE enabled="
                  << launchOptions.immediateDeadlineExperiment
                  << " status=UNVALIDATED_NO_ACHIEVED_RATE_CLAIM\n";
        std::cout << "EXPERIMENT_ONE_VBLANK enabled="
                  << (launchOptions.oneVblankExperiment ? 1 : 0)
                  << " status=UNVALIDATED_RENDER_DEADLINE_ONLY\n";
        std::cout << RuntimeUserDataLine(userData)
                  << (launchOptions.userDataRootExplicit ? " overridden=1" : "") << '\n';
        std::cout << "PC_CONFIG_PATH=" << launchOptions.pcConfigPath.u8string()
                  << " exists=" << (startupPcConfig.present() && !startupFromPackagedDefaults ? 1 : 0)
                  << " source=owned_startup_snapshot bytes=" << startupPcConfig.contents().size()
                  << " defaults=" << (startupFromPackagedDefaults ? "packaged_example" : "none")
                  << '\n';
        std::cout << "FRAME_CADENCE_DIAGNOSTICS enabled="
                  << (launchOptions.frameCadenceDiagnostics ? 1 : 0) << '\n';
        std::cout << "HITCH_DIAGNOSTICS enabled="
                  << (launchOptions.hitchDiagnostics ? 1 : 0) << '\n';
        std::cout << "AUDIO_DIAGNOSTICS enabled="
                  << (launchOptions.audioDiagnostics ? 1 : 0) << '\n';
        std::cout << "INPUT_VIBRATION_SCALE_Q16="
                  << RuntimeInputVibrationScaleQ16() << '\n';
        std::cout << "INPUT_CONTROLLER_BUTTON_MAP=0x" << std::hex
                  << RuntimeInputButtonMapCode() << std::dec << '\n';
        std::cout << "INPUT_CONTROLLER_PROFILE sensitivity="
                  << controllerProfile.sensitivity << " invert_y="
                  << (controllerProfile.invertY ? 1 : 0) << '\n';
        std::cout << "AUDIO_MASTER_VOLUME_Q16="
                  << RuntimeAudioMasterVolumeQ16() << '\n';
        std::cout << "XBOX_LANGUAGE=" << xboxLanguage << '\n';
        std::cout << "USER_DATA_ROOT=" << launchOptions.userDataRoot.u8string()
                  << " content_root=" << GuestPortableContentDeviceRoot().u8string()
                  << " screenshot_root="
                  << (launchOptions.userDataRoot / L"screenshots").u8string()
                  << '\n';
        const std::filesystem::path gameRoot = launchOptions.xexPath.parent_path();
        GetGuestFileSystem().SetGameRoot(gameRoot);
        std::cout << "GAME_CONTENT_ROOT=" << gameRoot.u8string() << '\n';
        const RuntimeModConfiguration mods = LoadRuntimeModConfiguration(
            launchOptions.modsConfigPath, executableDirectory);
        GetGuestFileSystem().SetContentOverlays(mods.layers);
        if (!languagePackName.empty()) {
            if (languagePack.contentOverlay) {
                std::vector<GuestContentOverlay> layers = mods.layers;
                layers.insert(layers.begin(), *languagePack.contentOverlay);
                GetGuestFileSystem().SetContentOverlays(std::move(layers));
            }
            std::cout << "LANGUAGE_PACK name=" << languagePackName
                      << " present=" << (languagePack.present ? 1 : 0)
                      << " active=" << (languagePack.active ? 1 : 0)
                      << " strings=" << languagePack.strings
                      << " rejected=" << languagePack.rejectedLines
                      << " content=" << (languagePack.contentOverlay ? 1 : 0)
                      << " textures=" << (languagePack.textures.empty() ? 0 : 1)
                      << " cube_atlas=" << (languagePack.cubeAtlas ? 1 : 0) << '\n';
        }
        std::cout << "MOD_CONFIG_PATH=" << mods.manifestPath.u8string()
                  << " present=" << (mods.manifestPresent ? 1 : 0)
                  << " enabled=" << (mods.enabled ? 1 : 0)
                  << " layers=" << mods.layers.size()
                  << " conflicts=" << mods.conflicts.size() << '\n';
        for (const auto& layer : mods.layers) {
            std::cout << "MOD_LAYER id=" << layer.id
                      << " priority=" << layer.priority
                      << " root=" << layer.root.u8string() << '\n';
        }
        for (const auto& conflict : mods.conflicts) {
            std::cout << "MOD_CONFLICT path=" << conflict.relativePath.generic_string()
                      << " winner=" << conflict.winnerId
                      << " shadowed=" << conflict.shadowedId << '\n';
        }
        const auto xex = ReadFile(launchOptions.xexPath);
        {
            // 0.9.1: the supported executable or a copy with the same code
            // (runtime_game_setup.h); anything else would run mismatched code.
            namespace setup = darkness::game_setup;
            const setup::XexIdentity identity = setup::IdentifyXex(xex.data(), xex.size());
            std::cout << "XEX_IDENTITY match="
                      << (identity.match == setup::XexMatch::kSupported  ? "supported"
                          : identity.match == setup::XexMatch::kSameCode ? "same_code"
                          : identity.match == setup::XexMatch::kDifferentCode
                              ? "different_code"
                              : "not_readable")
                      << " sha256=" << identity.fileSha256 << '\n';
            if (identity.match != setup::XexMatch::kSupported &&
                identity.match != setup::XexMatch::kSameCode) {
                std::error_code reportError;
                const std::filesystem::path logs = userData.localData / L"logs";
                std::filesystem::create_directories(logs, reportError);
                std::ofstream(logs / L"game_version_report.txt", std::ios::binary | std::ios::trunc)
                    << setup::XexIdentityReport(identity, launchOptions.xexPath.u8string());
                throw std::runtime_error(
                    setup::XexMatchText(identity) + " A detailed report is in " +
                    (logs / L"game_version_report.txt").u8string() + ".");
            }
        }
        ConfigureExecutableSystemFlags(xex.data(), xex.size());
        std::cout << "XEX_READ bytes=" << xex.size() << '\n';
        const Image image = Image::ParseImage(xex.data(), xex.size());
        std::cout << "XEX_PARSED sections=" << image.sections.size() << " base=0x" << std::hex
                  << image.base << " size=0x" << image.size << " entry=0x" << image.entry_point
                  << std::dec << '\n';
        if (image.entry_point != 0x828AA3E8) throw std::runtime_error("unexpected XEX entry point");
        if (!ConfigureExecutableTitleMetadata(xex.data(), xex.size(), image)) {
            throw std::runtime_error("XEX title metadata is missing or invalid");
        }
        RuntimeGraphicsCacheCompatibilityInputs graphicsCompatibility{};
        graphicsCompatibility.backend = "d3d12";
        if (mods.manifestPresent) {
            graphicsCompatibility.modsManifest = mods.manifestPath;
        }
        graphicsCompatibility.modTrees.reserve(mods.layers.size());
        for (const auto& layer : mods.layers) {
            graphicsCompatibility.modTrees.push_back({layer.id, layer.root});
        }
        const RuntimeGraphicsCacheIdentity graphicsCache =
            BuildRuntimeGraphicsCacheIdentityFromSnapshots(
                RuntimeGraphicsCacheBase(executableDirectory),
                ExecutableTitleId(), xex.data(), xex.size(),
                startupPcConfig, startupExampleConfig,
                RuntimeGraphicsCacheEnvironmentOverrides(),
                graphicsCompatibility);
        ConfigureRuntimeGraphicsCache(graphicsCache.root,
                                      ExecutableTitleId());
        std::cout << "GRAPHICS_CACHE_IDENTITY root="
                  << graphicsCache.root.u8string() << " title=0x" << std::hex
                  << ExecutableTitleId() << std::dec
                  << " xex_sha256=" << graphicsCache.executableSha256
                  << " environment_overrides="
                  << graphicsCache.environmentOverrideCount
                  << " environment_sha256="
                  << (graphicsCache.environmentSha256.empty()
                          ? std::string("none")
                          : graphicsCache.environmentSha256)
                  << " backend=" << graphicsCache.backend
                  << " mods_files=" << graphicsCache.modFileCount
                  << " mods_sha256="
                  << (graphicsCache.modsSha256.empty()
                          ? std::string("none")
                          : graphicsCache.modsSha256)
                  << " compatibility_sha256="
                  << graphicsCache.compatibilitySha256
                  << " settings_sha256=" << graphicsCache.settingsSha256
                  << " settings_in_key=0 binaries_in_key=0"
                  << " settings_source="
                  << (graphicsCache.settingsSource.empty()
                          ? std::string("built-in-defaults")
                          : graphicsCache.settingsSource.u8string())
                  << '\n';
        std::vector<uint32_t> achievementIds;
        achievementIds.reserve(ExecutableAchievements().size());
        for (const auto& achievement : ExecutableAchievements())
            achievementIds.push_back(achievement.id);
        std::cout << "XEX_TITLE_METADATA title=0x" << std::hex
                  << ExecutableTitleId() << std::dec
                  << " achievements=" << achievementIds.size() << '\n';
        if (!GetGuestXamState().ConfigureAchievementCatalog(ExecutableTitleId(),
                                                             achievementIds)) {
            throw std::runtime_error("portable achievement store is missing or invalid");
        }
        std::cout << "XEX_TITLE_METADATA achievements="
                  << ExecutableAchievements().size() << '\n';
        const size_t initializedImageBytes = BackingImageBytes(image);
        std::cout << "XEX_INITIALIZED_BYTES=0x" << std::hex << initializedImageBytes << std::dec << '\n';

        guestMemory.Initialize();
        ResetGuestRuntimeStop();
        // From this point onward an exception may leave input, guest workers,
        // audio, or graphics partially initialized and coordinated teardown is
        // required. Launch/configuration failures before guest memory exists
        // must not emit misleading subsystem-shutdown records.
        runtimeServicesStarted = true;
        if (launchOptions.inputDiagnostics) {
            InitializeRuntimeInputDiagnostics();
            std::cout << "INPUT_DIAGNOSTICS enabled=1 mode=transition_only_async\n";
        } else {
            std::cout << "INPUT_DIAGNOSTICS enabled=0\n";
        }
        InitializeRuntimeJobPollEarlyWake();
        InitializeRuntimeFrameWait();
        InitializeRuntimeNativeMouseLook();
        InitializeRuntimeStickLook();
        {
            // In-game settings overlay: saves only into the user's own
            // configuration (not a packaged preset, not a missing file).
            RuntimeSettingsServiceConfig settingsService;
            settingsService.configPath = launchOptions.pcConfigPath;
            settingsService.examplePath = packagedDefaultsPath;
            std::error_code presetError;
            const auto presetDirectory =
                std::filesystem::weakly_canonical(executableDirectory / L"presets", presetError);
            const auto configDirectory = std::filesystem::weakly_canonical(
                launchOptions.pcConfigPath.parent_path(), presetError);
            settingsService.persistence = startupPcConfig.present() &&
                                          configDirectory != presetDirectory;
            settingsService.offer = PcSettingsOfferFor(userData.localData, executableDirectory);
            StartRuntimeSettingsService(settingsService);
        }
        auto* base = guestMemory.base;
        std::cout << "GUEST_MEMORY_MAPPED base=0x" << std::hex << reinterpret_cast<uintptr_t>(base)
                  << std::dec << " aliases=xex_4k,physical\n";
        if (!PublishExecutableExecutionInfo(base, kExecutableExecutionInfoGuestAddress)) {
            throw std::runtime_error("XEX execution info is missing or invalid");
        }
        std::cout << "XEX_EXECUTION_INFO guest=0x" << std::hex
                  << kExecutableExecutionInfoGuestAddress << " bytes=0x"
                  << kExecutableExecutionInfoBytes << std::dec << '\n';
        {
            size_t sectionIndex = 0;
            for (const auto& section : image.sections) {
                std::cout << "MAP_SECTION index=" << sectionIndex++ << " name=" << section.name
                          << " base=0x" << std::hex << section.base << " size=0x" << section.size
                          << " data=0x" << reinterpret_cast<uintptr_t>(section.data) << std::dec << '\n';
                const size_t rva = section.base - image.base;
                const size_t initializedSize = rva < initializedImageBytes
                    ? std::min<size_t>(section.size, initializedImageBytes - rva)
                    : 0;
                std::memcpy(base + section.base, section.data, initializedSize);
                if (initializedSize != section.size) {
                    std::cout << "SECTION_ZERO_FILLED index=" << (sectionIndex - 1)
                              << " bytes=" << (section.size - initializedSize) << '\n';
                }
                std::cout << "SECTION_MAPPED index=" << (sectionIndex - 1) << '\n';
            }
            std::cout << "IMAGE_MAPPED\n";
            ApplyRuntimeWidescreenImagePatches(base);
            ApplyRuntimeLanguagePackImagePatches(base);
            std::cout << "GUEST_STACK_AVAILABLE base=0x" << std::hex << (kInitialStack - kStackSize)
                      << " size=0x" << kStackSize << std::dec << '\n';
            RegisterFunctions(base);

            PPCContext context{};
            InitializeGuestFloatingPointHostState(context);
            context.r1.u64 = kInitialStack;
            context.r13.u64 = InitializeMainGuestThread(base);
            std::cout << "RUNTIME_INIT\nHOST_MODULE_BASE=0x" << std::hex
                      << reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)) << std::dec
                      << "\nXEX_ENTRY_ADDRESS=0x828AA3E8\nFIRST_GENERATED_FUNCTION=_xstart\n";
            RuntimeFatalStartStallWatch(&RuntimeGraphicsCloseRequested);
            _xstart(context, base);
            RuntimeFatalStopStallWatch();
            RequestGuestRuntimeStop();
            ShutdownRuntimeAudio();
            std::cerr << "RUNTIME_GUEST_THREAD_JOIN_BEGIN\n";
            JoinGuestThreads();
            ShutdownRuntimeInputDiagnostics();
            std::cerr << "RUNTIME_GUEST_THREAD_JOIN_COMPLETE\n";
            std::cerr << "RUNTIME_GRAPHICS_SHUTDOWN_BEGIN\n";
            StopRuntimeSettingsService();
            ShutdownRuntimeGraphics();
            std::cerr << "RUNTIME_GRAPHICS_SHUTDOWN_COMPLETE\n";
            WriteRuntimeFunctionCoverage();
            std::cout << "XEX_ENTRY_RETURN lr=0x" << std::hex << context.lr << std::dec << '\n';
            return 0;
        }
    } catch (const std::exception& error) {
        // Keep the backing mapping alive until workers have observed the
        // coordinated stop, preventing a teardown race with guest accesses.
        RuntimeFatalStopStallWatch();
        std::string blocker = error.what();
        // A stop the player asked for (window closed) carries no blocker; any
        // other stop is fatal and must leave a report (V380).
        bool fatal = true;
        if (dynamic_cast<const GuestRuntimeStop*>(&error)) {
            const std::string workerBlocker = GuestRuntimeBlockerMessage();
            if (!workerBlocker.empty()) blocker = workerBlocker;
            fatal = !workerBlocker.empty();
        }
        if (fatal && gameStart) {
            RuntimeFatalReport(blocker);
            RuntimeFatalShowDialog(blocker);
        }
        if (runtimeServicesStarted) {
            RequestGuestRuntimeStop();
            ShutdownRuntimeAudio();
            std::cerr << "RUNTIME_GUEST_THREAD_JOIN_BEGIN\n";
            JoinGuestThreads();
            ShutdownRuntimeInputDiagnostics();
            std::cerr << "RUNTIME_GUEST_THREAD_JOIN_COMPLETE\n";
            std::cerr << "RUNTIME_GRAPHICS_SHUTDOWN_BEGIN\n";
            StopRuntimeSettingsService();
            ShutdownRuntimeGraphics();
            std::cerr << "RUNTIME_GRAPHICS_SHUTDOWN_COMPLETE\n";
        }
        WriteRuntimeFunctionCoverage();
        std::cerr << "RUNTIME_BLOCKER " << blocker << '\n';
        return fatal ? 1 : 0;
    }
}
