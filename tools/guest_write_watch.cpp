// Developer tool: which recompiled function writes one guest field.
//
// usage: guest_write_watch --pid N --host-address 0x... [--len 4] [--seconds 20]
//                          [--map build/runtime/TheDarkness.map] [--out FILE]
//                          [--max-log 64] [--rw 1]
//
// Attaches to an isolated test game as a debugger, puts a hardware write
// breakpoint (DR0) on one host address (guest base + guest address) on every
// thread, and for each write logs the writing instruction and a heuristic
// stack of return addresses inside the game image, both named from the
// linker map (sub_XXXXXXXX = the guest function). The value is read back as
// a big-endian float and word. Afterwards the breakpoint is removed from
// every thread and the tool detaches; the game keeps running. Other
// exceptions are passed to the game unchanged. Never for the owner's game.
// --rw 1 breaks on reads too (DR7 R/W = 11: the instruction that read or
// wrote the field).

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Symbol {
  uint64_t address;  // at the preferred base
  std::string name;
};

std::vector<Symbol> LoadMap(const std::string& path, uint64_t& preferred_base) {
  std::vector<Symbol> symbols;
  std::ifstream file(path);
  std::string line;
  preferred_base = 0x140000000ull;
  bool publics = false;
  while (std::getline(file, line)) {
    if (line.find("Preferred load address is") != std::string::npos) {
      preferred_base = std::strtoull(line.substr(line.rfind(' ') + 1).c_str(), nullptr, 16);
    }
    if (line.find("Publics by Value") != std::string::npos) {
      publics = true;
      continue;
    }
    if (!publics) continue;
    // " 0001:024a7600       __imp__sub_823FF1D0        00000001424a8600     obj"
    char section[16], name[512], address[32];
    if (std::sscanf(line.c_str(), " %15s %511s %31s", section, name, address) == 3 &&
        std::strchr(section, ':') && std::strlen(address) == 16) {
      symbols.push_back({std::strtoull(address, nullptr, 16), name});
    }
  }
  std::sort(symbols.begin(), symbols.end(),
            [](const Symbol& a, const Symbol& b) { return a.address < b.address; });
  return symbols;
}

std::string Name(const std::vector<Symbol>& symbols, uint64_t address) {
  auto it = std::upper_bound(symbols.begin(), symbols.end(), address,
                             [](uint64_t value, const Symbol& s) { return value < s.address; });
  if (it == symbols.begin()) return "?";
  --it;
  char text[640];
  std::snprintf(text, sizeof(text), "%s+0x%llx", it->name.c_str(),
                static_cast<unsigned long long>(address - it->address));
  return text;
}

}  // namespace

int main(int argc, char** argv) {
  DWORD pid = 0;
  uint64_t host_address = 0;
  uint32_t length = 4;
  double seconds = 20.0;
  std::string map_path = "build/runtime/TheDarkness.map";
  std::string out_path;
  int max_log = 64;
  bool read_write = false;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    const char* value = argv[i + 1];
    if (key == "--pid") pid = DWORD(std::strtoul(value, nullptr, 10));
    else if (key == "--host-address") host_address = std::strtoull(value, nullptr, 16);
    else if (key == "--len") length = uint32_t(std::strtoul(value, nullptr, 10));
    else if (key == "--seconds") seconds = std::strtod(value, nullptr);
    else if (key == "--map") map_path = value;
    else if (key == "--out") out_path = value;
    else if (key == "--max-log") max_log = std::atoi(value);
    else if (key == "--rw") read_write = std::atoi(value) != 0;
  }
  if (!pid || !host_address || (length != 1 && length != 2 && length != 4 && length != 8) ||
      host_address % length) {
    std::fprintf(stderr, "usage: guest_write_watch --pid N --host-address 0x... [--len 4] "
                         "[--seconds 20] [--map FILE] [--out FILE] [--max-log 64]\n");
    return 2;
  }
  FILE* out = out_path.empty() ? stdout : std::fopen(out_path.c_str(), "w");
  if (!out) return 2;
  uint64_t preferred_base = 0;
  const std::vector<Symbol> symbols = LoadMap(map_path, preferred_base);
  std::fprintf(out, "map %s symbols=%zu preferred_base=0x%llx\n", map_path.c_str(),
               symbols.size(), static_cast<unsigned long long>(preferred_base));

  if (!DebugActiveProcess(pid)) {
    std::fprintf(stderr, "DebugActiveProcess failed %lu\n", GetLastError());
    return 1;
  }
  DebugSetProcessKillOnExit(FALSE);

  const uint64_t len_bits = length == 1 ? 0 : length == 2 ? 1 : length == 8 ? 2 : 3;
  // L0, write (01) or read/write (11), length.
  const uint64_t dr7_bits = 1ull | ((read_write ? 3ull : 1ull) << 16) | (len_bits << 18);
  std::unordered_map<DWORD, HANDLE> threads;
  std::unordered_map<DWORD, bool> armed;
  HANDLE process = nullptr;
  uint64_t image_base = 0, image_size = 0;
  struct Site {
    uint64_t count = 0;
    std::string stack;
  };
  std::map<uint64_t, Site> sites;
  int logged = 0;
  uint64_t hits = 0;

  const auto set_registers = [&](DWORD id, HANDLE thread, bool on) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(thread, &context)) return false;
    context.Dr0 = on ? host_address : 0;
    context.Dr7 = on ? ((context.Dr7 & ~0xF0003ull) | dr7_bits) : (context.Dr7 & ~0xF0003ull);
    context.Dr6 = 0;
    const bool ok = SetThreadContext(thread, &context) != 0;
    if (ok) armed[id] = on;
    return ok;
  };

  const auto start = std::chrono::steady_clock::now();
  bool running = true;
  while (running) {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (elapsed >= seconds) break;
    DEBUG_EVENT event{};
    if (!WaitForDebugEvent(&event, 100)) continue;
    DWORD status = DBG_CONTINUE;
    switch (event.dwDebugEventCode) {
      case CREATE_PROCESS_DEBUG_EVENT: {
        process = event.u.CreateProcessInfo.hProcess;
        image_base = uint64_t(event.u.CreateProcessInfo.lpBaseOfImage);
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        SIZE_T got = 0;
        if (ReadProcessMemory(process, LPCVOID(image_base), &dos, sizeof(dos), &got) &&
            ReadProcessMemory(process, LPCVOID(image_base + dos.e_lfanew), &nt, sizeof(nt), &got)) {
          image_size = nt.OptionalHeader.SizeOfImage;
        }
        threads[event.dwThreadId] = event.u.CreateProcessInfo.hThread;
        if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
        std::fprintf(out, "attached image_base=0x%llx size=0x%llx\n",
                     static_cast<unsigned long long>(image_base),
                     static_cast<unsigned long long>(image_size));
        break;
      }
      case CREATE_THREAD_DEBUG_EVENT:
        threads[event.dwThreadId] = event.u.CreateThread.hThread;
        break;
      case EXIT_THREAD_DEBUG_EVENT:
        threads.erase(event.dwThreadId);
        armed.erase(event.dwThreadId);
        break;
      case LOAD_DLL_DEBUG_EVENT:
        if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        break;
      case EXIT_PROCESS_DEBUG_EVENT:
        running = false;
        break;
      case EXCEPTION_DEBUG_EVENT: {
        const DWORD code = event.u.Exception.ExceptionRecord.ExceptionCode;
        if (code == EXCEPTION_BREAKPOINT) {
          status = DBG_CONTINUE;  // the attach break
        } else if (code == EXCEPTION_SINGLE_STEP && threads.count(event.dwThreadId)) {
          HANDLE thread = threads[event.dwThreadId];
          CONTEXT context{};
          context.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
          if (GetThreadContext(thread, &context) && (context.Dr6 & 1)) {
            ++hits;
            const uint64_t rip = context.Rip;
            Site& site = sites[rip];
            ++site.count;
            if (site.stack.empty() || logged < max_log) {
              std::string stack;
              uint64_t frames[256] = {};
              SIZE_T got = 0;
              ReadProcessMemory(process, LPCVOID(context.Rsp), frames, sizeof(frames), &got);
              int named = 0;
              for (size_t i = 0; i < got / 8 && named < 10; ++i) {
                if (frames[i] > image_base && frames[i] < image_base + image_size) {
                  stack += "\n    " + Name(symbols, frames[i] - image_base + preferred_base);
                  ++named;
                }
              }
              // The guest context: a 64-byte aligned host register pointing
              // at a PPCContext whose lr is guest code (r3 +0, r1 +16, r4..r10
              // +32.., r28..r31 +224.., lr +256).
              const DWORD64 registers[] = {context.Rax, context.Rbx, context.Rcx, context.Rdx,
                                           context.Rsi, context.Rdi, context.Rbp, context.R8,
                                           context.R9,  context.R10, context.R11, context.R12,
                                           context.R13, context.R14, context.R15};
              for (DWORD64 candidate : registers) {
                if (!candidate || (candidate & 0x3F)) continue;
                uint64_t guest[34] = {};
                SIZE_T read = 0;
                if (!ReadProcessMemory(process, LPCVOID(candidate), guest, sizeof(guest), &read) ||
                    read != sizeof(guest)) {
                  continue;
                }
                const uint64_t lr = guest[32];
                if (lr < 0x82000000ull || lr >= 0x83000000ull || !uint32_t(guest[2])) continue;
                char text[512];
                std::snprintf(text, sizeof(text),
                              "\n    guest lr=0x%08llX r1=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X "
                              "r6=0x%08X r7=0x%08X r8=0x%08X r9=0x%08X r10=0x%08X r11=0x%08X "
                              "r28=0x%08X r29=0x%08X r30=0x%08X r31=0x%08X",
                              static_cast<unsigned long long>(lr), uint32_t(guest[2]),
                              uint32_t(guest[0]), uint32_t(guest[4]), uint32_t(guest[5]),
                              uint32_t(guest[6]), uint32_t(guest[7]), uint32_t(guest[8]),
                              uint32_t(guest[9]), uint32_t(guest[10]), uint32_t(guest[11]),
                              uint32_t(guest[28]), uint32_t(guest[29]), uint32_t(guest[30]),
                              uint32_t(guest[31]));
                stack = text + stack;
                break;
              }
              if (site.stack.empty()) site.stack = stack;
              if (logged < max_log) {
                uint8_t bytes[8] = {};
                ReadProcessMemory(process, LPCVOID(host_address & ~3ull), bytes, 4, &got);
                const uint32_t word = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
                                      (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
                float value;
                std::memcpy(&value, &word, 4);
                std::fprintf(out, "hit %llu t=%.3f tid=%lu value=%g word=0x%08X at %s%s\n",
                             static_cast<unsigned long long>(hits), elapsed, event.dwThreadId,
                             value, word,
                             Name(symbols, rip - image_base + preferred_base).c_str(),
                             stack.c_str());
                std::fflush(out);
                ++logged;
              }
            }
            context.Dr6 = 0;
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            SetThreadContext(thread, &context);
            status = DBG_CONTINUE;
          } else {
            status = DBG_EXCEPTION_NOT_HANDLED;
          }
        } else {
          status = DBG_EXCEPTION_NOT_HANDLED;  // the game's own exceptions
        }
        break;
      }
      default:
        break;
    }
    // Every thread is stopped while an event is pending: arm new ones.
    for (const auto& thread : threads) {
      if (!armed[thread.first]) set_registers(thread.first, thread.second, true);
    }
    ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
  }

  // Disarm every thread (suspended), then detach.
  for (const auto& thread : threads) {
    if (SuspendThread(thread.second) != DWORD(-1)) {
      set_registers(thread.first, thread.second, false);
      ResumeThread(thread.second);
    }
  }
  // Drain a pending single step raised just before disarming.
  DEBUG_EVENT event{};
  while (WaitForDebugEvent(&event, 50)) {
    DWORD status = DBG_CONTINUE;
    if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
        event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_SINGLE_STEP &&
        event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT) {
      status = DBG_EXCEPTION_NOT_HANDLED;
    }
    if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile) {
      CloseHandle(event.u.LoadDll.hFile);
    }
    ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
  }
  const bool detached = DebugActiveProcessStop(pid) != 0;
  std::fprintf(out, "hits=%llu sites=%zu detached=%d\n", static_cast<unsigned long long>(hits),
               sites.size(), detached ? 1 : 0);
  for (const auto& site : sites) {
    std::fprintf(out, "site %s count=%llu%s\n",
                 Name(symbols, site.first - image_base + preferred_base).c_str(),
                 static_cast<unsigned long long>(site.second.count), site.second.stack.c_str());
  }
  if (out != stdout) std::fclose(out);
  return 0;
}
