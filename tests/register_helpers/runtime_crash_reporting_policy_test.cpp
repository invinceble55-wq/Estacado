#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main() {
    const std::filesystem::path source =
        std::filesystem::path(DARKNESS_SOURCE_ROOT) / "runtime" / "main.cpp";
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        std::cerr << "FAIL: unable to open " << source << '\n';
        return 1;
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());

    bool passed = true;
    passed &= Check(text.find("SetUnhandledExceptionFilter(CrashEvidence)") !=
                        std::string::npos,
                    "crash evidence is not installed as an unhandled filter");
    passed &= Check(text.find("AddVectoredExceptionHandler(1, CrashEvidence)") ==
                        std::string::npos,
                    "first-chance crash evidence hook was reintroduced");
    passed &= Check(text.find("logs\\\\runtime_crash.log") != std::string::npos,
                    "generic runtime crash report path is missing");
    passed &= Check(text.find("GetSystemTime(&utc)") != std::string::npos &&
                        text.find("GetCurrentProcessId()") != std::string::npos &&
                        text.find("GetCurrentThreadId()") != std::string::npos,
                    "crash report provenance fields are incomplete");
    passed &= Check(text.find("RuntimeAppendPpcCrashEvidence") != std::string::npos,
                    "active PPC context is no longer appended to crash reports");
    // Crash reports: in the local data folder's logs (beside the executable
    // for a portable copy), with a small minidump from the system dbghelp
    // resolved before guest code runs.
    const size_t prepare = text.find("PrepareCrashEvidence(startupLayout.localData");
    const size_t install = text.find("SetUnhandledExceptionFilter(CrashEvidence)");
    passed &= Check(prepare != std::string::npos && install != std::string::npos &&
                        prepare < install,
                    "crash evidence paths and MiniDumpWriteDump are not prepared before the filter");
    passed &= Check(text.find("LOAD_LIBRARY_SEARCH_SYSTEM32") != std::string::npos &&
                        text.find("\"MiniDumpWriteDump\"") != std::string::npos &&
                        text.find("HOST_EXCEPTION_MINIDUMP written=") != std::string::npos,
                    "crash reports lost the system-dbghelp minidump");
    passed &= Check(text.find("MiniDumpWithFullMemory") == std::string::npos,
                    "a full-memory dump would include the multi-gigabyte guest address space");
    passed &= Check(text.find("bool runtimeServicesStarted = false") !=
                            std::string::npos &&
                        text.find("runtimeServicesStarted = true") !=
                            std::string::npos &&
                        text.find("if (runtimeServicesStarted)") !=
                            std::string::npos,
                    "early launch errors no longer guard coordinated subsystem teardown");

    if (passed) std::cout << "Runtime crash reporting policy tests passed\n";
    return passed ? 0 : 1;
}
