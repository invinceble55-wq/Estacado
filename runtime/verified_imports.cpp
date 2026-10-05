#include "runtime_guest_bulk_write.h"
#include "runtime_guest_write_completion.h"
#include "ppc_recomp_shared.h"
#include "runtime_audio.h"
#include "runtime_filesystem.h"
#include "runtime_fatal.h"
#include "runtime_function_trace.h"
#include "runtime_graphics.h"
#include "runtime_guest_format.h"
#include "runtime_input.h"
#include "runtime_job_poll_wake.h"
#include "runtime_memory.h"
#include "runtime_modules.h"
#include "runtime_movement_packet.h"
#include "runtime_objects.h"
#include "runtime_sync.h"
#include "runtime_tls.h"
#include "runtime_threads.h"
#include "runtime_title_notifications.h"
#include "runtime_video_mode.h"
#include "runtime_xam.h"

#include <TinySHA1.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

// Windows SDK aliases collide with GuestObjects method names below.
#undef CreateEvent
#undef CreateSemaphore

#include <algorithm>
#include <cstring>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kStatusSuccess = 0x00000000;
constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kStatusBufferTooSmall = 0xC0000023;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
constexpr uint32_t kStatusNotImplemented = 0xC0000002;
constexpr uint32_t kStatusObjectTypeMismatch = 0xC0000024;
constexpr uint32_t kStatusAccessDenied = 0xC0000022;
constexpr uint32_t kStatusObjectNameNotFound = 0xC0000034;
constexpr uint32_t kStatusObjectNameCollision = 0xC0000035;
constexpr uint32_t kStatusInfoLengthMismatch = 0xC0000004;
constexpr uint32_t kStatusInvalidInfoClass = 0xC0000003;
constexpr uint32_t kStatusNoSuchFile = 0xC000000F;
constexpr uint32_t kStatusNotFound = 0xC0000225;
constexpr uint32_t kStatusNoMoreFiles = 0x80000006;
constexpr uint32_t kStatusUserApc = 0x000000C0;
constexpr uint32_t kStatusTimeout = 0x00000102;
constexpr uint32_t kStatusPending = 0x00000103;
constexpr uint32_t kXErrorNotFound = 0x00000490;
constexpr uint32_t kXErrorIoPending = 0x000003E5;
constexpr uint32_t kXErrorInvalidParameter = 0x00000057;
constexpr uint32_t kXErrorInsufficientBuffer = 0x0000007A;
constexpr uint32_t kXErrorNoSuchUser = 0x00000525;
constexpr uint32_t kXErrorFileNotFound = 0x00000002;
constexpr uint32_t kXErrorPathNotFound = 0x00000003;
constexpr uint32_t kXErrorAccessDenied = 0x00000005;
constexpr uint32_t kXErrorAlreadyExists = 0x000000B7;
constexpr uint32_t kXErrorDiskFull = 0x00000070;
constexpr uint32_t kXErrorDeviceNotConnected = 0x0000048F;
constexpr uint32_t kXErrorFunctionFailed = 0x0000065B;
constexpr uint32_t kXErrorEmpty = 0x000010D2;
bool RuntimeIsOfflineXgiMessage(uint32_t message) noexcept;  // V380, below
constexpr uint32_t kXErrorFail = 0x80004005;
// The local XEX imports both xboxkrnl.exe and xam.xex at exactly 0.0.5759.32.
// XamGetSystemVersion uses the packed dashboard version layout
// major[31:28], minor[27:24], build[23:8], qfe[7:0]. Xbox 360 software
// identifies this kernel generation as major 2, giving 2.0.5759.32.
constexpr uint32_t kXamSystemVersion = 0x20167F20;
constexpr uint32_t kMmQueryStatisticsSize = 104;
constexpr uint32_t kPageReadOnly = 0x00000002;
constexpr uint32_t kPageReadWrite = 0x00000004;
constexpr uint32_t kPageProtectionMask = 0x000007FF;
constexpr uint32_t kMemLargePages = 0x20000000;
constexpr uint32_t kMem16MbPages = 0x80000000;
constexpr uint32_t kPhysicalGuestBase = 0xA0000000;
constexpr uint32_t kFileSynchronousIoAlert = 0x00000010;
constexpr uint32_t kFileSynchronousIoNonAlert = 0x00000020;
// These completed, poll-heavy subsystems previously emitted synchronous
// liveness heartbeats. Keep failure/transition evidence, but leave success-path
// heartbeats off for deterministic title pacing.
constexpr bool kCompletedHotPathDiagnosticsEnabled = false;
std::atomic<uint32_t> notifyUiPosition{};
std::atomic<uint64_t> ntReadTraceCount{};
std::atomic<uint64_t> ntWriteTraceCount{};
std::atomic<uint64_t> ntSetEventTraceCount{};
std::atomic<uint64_t> spinLockTraceCount{};
std::atomic<uint64_t> performanceFrequencyTraceCount{};
std::atomic<uint64_t> videoModeTraceCount{};
std::atomic<uint64_t> retrainEdramTraceCount{};
std::atomic<uint64_t> systemCommandBufferTraceCount{};
std::atomic<uint64_t> vdSwapTraceCount{};
std::atomic<uint64_t> resetEventTraceCount{};
std::atomic<uint64_t> criticalRegionTraceCount{};
RuntimeInputTransitionTracker xamInputCapabilitiesTransitions[5];
RuntimeInputTransitionTracker xamInputStateTransitions[5];
RuntimeInputTransitionTracker xamInputVibrationTransitions[5];
std::atomic<uint32_t> graphicsNotificationSequence{};
std::atomic<uint32_t> lastFrontBufferWidth{};
std::atomic<uint32_t> lastFrontBufferHeight{};
std::atomic<uint32_t> lastBackBufferWidth{};
std::atomic<uint32_t> lastBackBufferHeight{};

size_t InputDiagnosticSlot(uint32_t userIndex) {
    return userIndex < 4 ? userIndex : 4;
}

RuntimeInputDiagnosticEvent MakeInputDiagnosticEvent(
    RuntimeInputDiagnosticKind kind, PPCContext& ctx, uint32_t userIndex,
    uint32_t flags, uint32_t guestAddress, uint32_t result) {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    RuntimeInputDiagnosticEvent event{};
    event.kind = kind;
    event.hostMilliseconds = GetTickCount64();
    event.hostPerformanceCounter = counter.QuadPart;
    event.guestThread = CurrentGuestThreadId();
    event.guestLinkRegister = ctx.lr;
    event.userIndex = userIndex;
    event.flags = flags;
    event.guestAddress = guestAddress;
    event.result = result;
    return event;
}

bool ShouldTraceHotPath(std::atomic<uint64_t>& traceCount, uint64_t& ordinal,
                        uint64_t initial = 8, uint64_t mask = 0xFFFu) {
    ordinal = traceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    return ordinal <= initial || (ordinal & mask) == 0;
}

uint32_t RoundUp(uint32_t value, uint32_t multiple) {
    if (!value) return 0;
    const uint64_t rounded = (uint64_t(value) + multiple - 1) / multiple * multiple;
    return rounded > UINT32_MAX ? 0 : static_cast<uint32_t>(rounded);
}

bool HasXboxPathDevice(const std::string& path) {
    if (path.empty()) return false;
    // Xbox object-manager lookups do not implicitly bind a bare name such as
    // "Content" to the title volume. A DOS-device/symbolic-link prefix (D:,
    // cache:, xbmovie:, and so on), or an explicit NT object path, is required.
    if (path[0] == '\\' || path[0] == '/') return true;
    const size_t separator = path.find_first_of("\\/");
    const size_t colon = path.find(':');
    return colon != std::string::npos && colon != 0 &&
           (separator == std::string::npos || colon < separator);
}

void StoreBigEndian32(uint8_t* destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>(value >> 24);
    destination[1] = static_cast<uint8_t>(value >> 16);
    destination[2] = static_cast<uint8_t>(value >> 8);
    destination[3] = static_cast<uint8_t>(value);
}

std::u16string Utf8ToUtf16(const std::string& value) {
    if (value.empty()) return {};
    if (value.size() > INT_MAX) throw std::runtime_error("title metadata string is too large");
    const int characters = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (characters <= 0) throw std::runtime_error("title metadata contains invalid UTF-8");
    std::u16string result(static_cast<size_t>(characters), u'\0');
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()),
                            reinterpret_cast<wchar_t*>(result.data()),
                            characters) != characters) {
        throw std::runtime_error("title metadata UTF-16 conversion failed");
    }
    return result;
}

bool AppendUtf16BigEndian(const std::u16string& value, uint8_t*& hostCursor,
                          uint32_t& guestCursor, size_t& remaining,
                          uint32_t* guestStringAddress) {
    const uint64_t bytes64 = uint64_t(value.size() + 1) * 2;
    if (bytes64 > remaining || bytes64 > UINT32_MAX - guestCursor) return false;
    if (guestStringAddress) *guestStringAddress = guestCursor;
    for (const char16_t character : value) {
        hostCursor[0] = static_cast<uint8_t>(character >> 8);
        hostCursor[1] = static_cast<uint8_t>(character);
        hostCursor += 2;
    }
    hostCursor[0] = 0;
    hostCursor[1] = 0;
    hostCursor += 2;
    const uint32_t bytes = static_cast<uint32_t>(bytes64);
    guestCursor += bytes;
    remaining -= bytes;
    return true;
}

// XAM_OVERLAPPED is seven big-endian words: result, length, requesting
// thread handle, event handle, completion routine, completion context, and
// extended error. This follows the pinned ReXGlue immediate-completion path.
// Event notification and completion APC dispatch are real guest scheduling
// effects; neither may be replaced with a success-only memory write.
void CompleteXamOverlappedImmediateEx(uint8_t* base, uint32_t overlapped,
                                      uint32_t result, uint32_t extendedError,
                                      uint32_t length) {
    constexpr uint32_t kOverlappedBytes = 28;
    if (!overlapped || (overlapped & 3u) ||
        overlapped > UINT32_MAX - kOverlappedBytes) {
        throw std::runtime_error("XAM overlapped has an invalid guest address");
    }
    const uint32_t threadHandle = CurrentGuestThreadHandle();
    if (!threadHandle) {
        throw std::runtime_error("XAM overlapped completion has no requesting guest thread");
    }
    const uint32_t eventHandle = PPC_LOAD_U32(overlapped + 0x0C);
    const uint32_t completionRoutine = PPC_LOAD_U32(overlapped + 0x10);
    PPC_STORE_U32(overlapped + 0x00, result);
    PPC_STORE_U32(overlapped + 0x04, length);
    PPC_STORE_U32(overlapped + 0x08, threadHandle);
    PPC_STORE_U32(overlapped + 0x18, extendedError);
    if (eventHandle && !GetGuestObjects().SetEvent(eventHandle, nullptr)) {
        throw std::runtime_error("XAM overlapped references an invalid event handle");
    }
    if (completionRoutine) {
        QueueGuestApc(completionRoutine, result, length, overlapped);
    }
    std::cout << "XAM_OVERLAPPED_COMPLETE address=0x" << std::hex << overlapped
              << " result=0x" << result << " length=0x"
              << length << " extended=0x" << extendedError
              << " thread_handle=0x" << threadHandle
              << " event=0x" << eventHandle << " routine=0x" << completionRoutine
              << std::dec << '\n';
}

void CompleteXamOverlappedImmediate(uint8_t* base, uint32_t overlapped,
                                    uint32_t result) {
    CompleteXamOverlappedImmediateEx(base, overlapped, result, result,
                                     result ? UINT32_MAX : 0);
}

uint32_t XamHresultFromWin32(uint32_t result) {
    if (static_cast<int32_t>(result) <= 0) return result;
    return 0x80070000u | (result & 0xFFFFu);
}

constexpr uint32_t kPcrCurrentIrqlOffset = 0x18;
constexpr uint8_t kIrqlDispatch = 2;

uint32_t AcquireGuestSpinLock(PPCContext& ctx, uint8_t* base, uint32_t address,
                              bool changeIrql) {
    if (!address || (address & 3u)) {
        throw std::runtime_error("spin lock has an invalid guest address");
    }
    const uint32_t pcr = ctx.r13.u32;
    if (!pcr || pcr > UINT32_MAX - kPcrCurrentIrqlOffset) {
        throw std::runtime_error("spin lock caller has an invalid PCR");
    }

    const uint8_t oldIrql = PPC_LOAD_U8(pcr + kPcrCurrentIrqlOffset);
    if (changeIrql) PPC_STORE_U8(pcr + kPcrCurrentIrqlOffset, kIrqlDispatch);

    auto* lock = reinterpret_cast<volatile LONG*>(base + address);
    const LONG rawOwner = static_cast<LONG>(__builtin_bswap32(pcr));
    if (*lock == rawOwner) throw std::runtime_error("recursive guest spin-lock acquisition");
    for (;;) {
        bool acquired;
        {
            const RuntimeGuestSourceWriteScope source_write(address, 4);
            acquired = InterlockedCompareExchange(lock, rawOwner, 0) == 0;
            if (acquired) RuntimeNotifyGuestPhysicalWrite(address, 4);
        }
        if (acquired) break;
        if (GuestRuntimeStopRequested()) throw GuestRuntimeStop{};
        YieldProcessor();
        std::this_thread::yield();
    }
    return changeIrql ? oldIrql : 0;
}

void ReleaseGuestSpinLock(PPCContext& ctx, uint8_t* base, uint32_t address,
                          uint32_t oldIrql, bool changeIrql) {
    if (!address || (address & 3u)) {
        throw std::runtime_error("spin lock has an invalid guest address");
    }
    const uint32_t pcr = ctx.r13.u32;
    const LONG rawOwner = static_cast<LONG>(__builtin_bswap32(pcr));
    auto* lock = reinterpret_cast<volatile LONG*>(base + address);
    if (*lock != rawOwner) throw std::runtime_error("guest spin-lock owner mismatch");
    {
        const RuntimeGuestSourceWriteScope source_write(address, 4);
        InterlockedExchange(lock, 0);
        RuntimeNotifyGuestPhysicalWrite(address, 4);
    }
    if (changeIrql && oldIrql < kIrqlDispatch) {
        PPC_STORE_U8(pcr + kPcrCurrentIrqlOffset, static_cast<uint8_t>(oldIrql));
    }
}

void TraceGuestSpinLock(const char* operation, uint32_t address, uint32_t oldIrql) {
    if (!kCompletedHotPathDiagnosticsEnabled) return;
    const uint64_t ordinal = spinLockTraceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal <= 64 || (ordinal & 0xFFu) == 0) {
        std::cout << "SPIN_LOCK operation=" << operation << " thread="
                  << CurrentGuestThreadId() << " address=0x" << std::hex << address
                  << " old_irql=0x" << oldIrql << std::dec << " ordinal=" << ordinal << '\n';
    }
}

void DelayForGuestInterval(int64_t interval) {
    using namespace std::chrono;
    int64_t relativeTicks{};
    if (interval < 0) {
        relativeTicks = interval == INT64_MIN ? INT64_MAX : -interval;
    } else if (interval > 0) {
        constexpr int64_t kWindowsToUnixEpochTicks = 11644473600LL * 10'000'000LL;
        const int64_t now = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100
            + kWindowsToUnixEpochTicks;
        relativeTicks = interval > now ? interval - now : 0;
    }
    // The PowerPC ABI interval is in 100-nanosecond units. Convert only at
    // the final host wait so both relative and absolute contracts retain the
    // guest-visible semantics without a polling loop.
    const auto millisecondsToWait = relativeTicks / 10'000;
    if (millisecondsToWait > 0) std::this_thread::sleep_for(milliseconds(millisecondsToWait));
    else std::this_thread::yield();
}

std::chrono::steady_clock::time_point GuestWaitDeadline(int64_t timeout) {
    using namespace std::chrono;
    int64_t ticks{};
    if (timeout < 0) {
        ticks = timeout == INT64_MIN ? INT64_MAX : -timeout;
    } else if (timeout > 0) {
        constexpr int64_t kWindowsToUnixEpochTicks = 11644473600LL * 10'000'000LL;
        const int64_t now =
            duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100 +
            kWindowsToUnixEpochTicks;
        ticks = timeout > now ? timeout - now : 0;
    }
    const int64_t maximumTicks = duration_cast<nanoseconds>(hours(24)).count() / 100;
    return steady_clock::now() + nanoseconds(std::min(ticks, maximumTicks) * 100);
}

std::string ReadGuestAnsiString(uint8_t* base, uint32_t address) {
    if (!address || address > UINT32_MAX - 8) return {};
    const uint16_t length = PPC_LOAD_U16(address);
    const uint16_t maximumLength = PPC_LOAD_U16(address + 2);
    const uint32_t buffer = PPC_LOAD_U32(address + 4);
    if (length > maximumLength || (length && !buffer) || buffer > UINT32_MAX - length)
        throw std::runtime_error("invalid guest ANSI string");
    return length ? std::string(reinterpret_cast<const char*>(base + buffer), length)
                  : std::string{};
}

std::string ReadGuestCString(uint8_t* base, uint32_t address, uint32_t maximumBytes) {
    if (!address || !maximumBytes || address > UINT32_MAX - maximumBytes)
        throw std::runtime_error("invalid guest C string");
    std::string value;
    value.reserve(maximumBytes);
    for (uint32_t index = 0; index < maximumBytes; ++index) {
        const char character = static_cast<char>(PPC_LOAD_U8(address + index));
        if (!character) return value;
        value.push_back(character);
    }
    throw std::runtime_error("unterminated guest C string");
}

constexpr uint32_t kVideoModeSize = 0x30;
constexpr uint32_t kDisplayInfoSize = 0x58;

void WriteVerifiedVideoMode(uint8_t* base, uint32_t output, const char* importName) {
    if (!output || output > UINT32_MAX - kVideoModeSize) {
        throw std::runtime_error(std::string(importName) +
                                 " reached an invalid guest video-mode buffer");
    }
    RuntimeGeneratedMemset(base, base + output, 0, kVideoModeSize, __FILE__, __LINE__);
    PPC_STORE_U32(output + 0x00, darkness::guest_video_mode::DisplayWidth());
    PPC_STORE_U32(output + 0x04, darkness::guest_video_mode::DisplayHeight());
    PPC_STORE_U32(output + 0x08, 0);    // IsInterlaced
    PPC_STORE_U32(output + 0x0C, 1);    // IsWidescreen
    PPC_STORE_U32(output + 0x10, 1);    // IsHighDefinition
    PPC_STORE_U32(output + 0x14,
                  darkness::guest_video_mode::kRefreshRateFloatBits);
    PPC_STORE_U32(output + 0x18, 1);    // NTSC
    PPC_STORE_U32(output + 0x1C, 0x4A);
    PPC_STORE_U32(output + 0x20, 1);
    // +0x24, +0x28, and +0x2C are reserved and remain zero.
}

void WriteVerifiedDisplayInfo(uint8_t* base, uint32_t output, const char* importName) {
    if (!output || output > UINT32_MAX - kDisplayInfoSize) {
        throw std::runtime_error(std::string(importName) +
                                 " reached an invalid guest display-info buffer");
    }
    RuntimeGeneratedMemset(base, base + output, 0, kDisplayInfoSize, __FILE__, __LINE__);
    PPC_STORE_U16(output + 0x00, darkness::guest_video_mode::DisplayWidth());
    PPC_STORE_U16(output + 0x02, darkness::guest_video_mode::DisplayHeight());
    // The source rectangle starts at (0, 0) and ends at the active mode.
    PPC_STORE_U32(output + 0x10, darkness::guest_video_mode::DisplayWidth());
    PPC_STORE_U32(output + 0x14, darkness::guest_video_mode::DisplayHeight());
    PPC_STORE_U32(output + 0x18, darkness::guest_video_mode::DisplayWidth());
    PPC_STORE_U32(output + 0x1C, darkness::guest_video_mode::DisplayHeight());
    PPC_STORE_U32(output + 0x20, 1);    // VerticalFilterType
    PPC_STORE_U32(output + 0x30, 1);    // HorizontalFilterType
    PPC_STORE_U16(output + 0x40, darkness::guest_video_mode::DisplayWidth() / 4);  // OverscanLeft
    PPC_STORE_U16(output + 0x42, darkness::guest_video_mode::DisplayHeight() / 4);  // OverscanTop
    PPC_STORE_U16(output + 0x44, darkness::guest_video_mode::DisplayWidth() / 4);  // OverscanRight
    PPC_STORE_U16(output + 0x46, darkness::guest_video_mode::DisplayHeight() / 4);  // OverscanBottom
    PPC_STORE_U16(output + 0x48, darkness::guest_video_mode::DisplayWidth());
    PPC_STORE_U16(output + 0x4A, darkness::guest_video_mode::DisplayHeight());
    PPC_STORE_U32(output + 0x4C,
                  darkness::guest_video_mode::kRefreshRateFloatBits);
    PPC_STORE_U32(output + 0x50, 0);    // DisplayInterlaced
    PPC_STORE_U16(output + 0x56, darkness::guest_video_mode::DisplayWidth());
}
}

// Xbox 360 performance-counter frequency is fixed at 50 MHz. This matches the
// behavior in Xenia's xboxkrnl_threading implementation and returns the value
// through the guest ABI's r3 register.
PPC_FUNC(__imp__KeQueryPerformanceFrequency) {
    ctx.r3.u64 = 50'000'000u;
    uint64_t ordinal = 0;
    if (ShouldTraceHotPath(performanceFrequencyTraceCount, ordinal)) {
        std::cout << "IMPORT_CALL name=__imp__KeQueryPerformanceFrequency"
                     " result=50000000 ordinal="
                  << ordinal << '\n';
    }
}

// The executable and every currently supported ExCreateThread worker belong to
// the title's user process. This is X_PROCTYPE_USER in the pinned kernel model.
// KeSetCurrentProcessType remains trapped, so an unmodelled process transition
// cannot silently make this retained process state stale.
PPC_FUNC(__imp__KeGetCurrentProcessType) {
    constexpr uint32_t kXProcTypeUser = 1;
    ctx.r3.u64 = kXProcTypeUser;
    std::cout << "IMPORT_CALL name=__imp__KeGetCurrentProcessType thread="
              << CurrentGuestThreadId() << " result=" << kXProcTypeUser << '\n';
}

PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel) {
    RuntimeTraceImport("KeRaiseIrqlToDpcLevel", ctx);
    const uint32_t pcr = ctx.r13.u32;
    if (!pcr || pcr > UINT32_MAX - kPcrCurrentIrqlOffset)
        throw std::runtime_error("KeRaiseIrqlToDpcLevel has an invalid guest PCR");
    const uint8_t previous = PPC_LOAD_U8(pcr + kPcrCurrentIrqlOffset);
    if (previous > kIrqlDispatch)
        throw std::runtime_error("KeRaiseIrqlToDpcLevel called above DISPATCH_LEVEL");
    PPC_STORE_U8(pcr + kPcrCurrentIrqlOffset, kIrqlDispatch);
    ctx.r3.u64 = previous;
    static std::atomic<uint64_t> traceCount{};
    const uint64_t ordinal = traceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal <= 64 || (ordinal & 0xFFFu) == 0) {
        std::cout << "IMPORT_CALL name=__imp__KeRaiseIrqlToDpcLevel thread="
                  << CurrentGuestThreadId() << " previous=" << uint32_t(previous)
                  << " current=" << uint32_t(kIrqlDispatch)
                  << " ordinal=" << ordinal << '\n';
    }
}

PPC_FUNC(__imp__KfLowerIrql) {
    RuntimeTraceImport("KfLowerIrql", ctx);
    const uint32_t pcr = ctx.r13.u32;
    const uint8_t requested = ctx.r3.u8;
    if (!pcr || pcr > UINT32_MAX - kPcrCurrentIrqlOffset)
        throw std::runtime_error("KfLowerIrql has an invalid guest PCR");
    const uint8_t current = PPC_LOAD_U8(pcr + kPcrCurrentIrqlOffset);
    if (requested > current)
        throw std::runtime_error("KfLowerIrql attempted to raise the guest IRQL");
    PPC_STORE_U8(pcr + kPcrCurrentIrqlOffset, requested);
    static std::atomic<uint64_t> traceCount{};
    const uint64_t ordinal = traceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal <= 64 || (ordinal & 0xFFFu) == 0) {
        std::cout << "IMPORT_CALL name=__imp__KfLowerIrql thread="
                  << CurrentGuestThreadId() << " previous=" << uint32_t(current)
                  << " current=" << uint32_t(requested)
                  << " ordinal=" << ordinal << '\n';
    }
}

// Return the minimum XAM/kernel version encoded by this title's own import
// descriptors. This is title-specific binary evidence, not the zero-valued
// placeholder used by the pinned reference runtimes.
PPC_FUNC(__imp__XamGetSystemVersion) {
    RuntimeTraceImport("XamGetSystemVersion", ctx);
    ctx.r3.u64 = kXamSystemVersion;
    std::cout << "IMPORT_CALL name=__imp__XamGetSystemVersion version=2.0.5759.32"
              << " packed=0x" << std::hex << kXamSystemVersion << std::dec << '\n';
}

PPC_FUNC(__imp__XGetLanguage) {
    RuntimeTraceImport("XGetLanguage", ctx);
    ctx.r3.u64 = GetGuestXamState().Language();
}

// XamGetExecutionId returns a guest pointer to the immutable execution-info
// optional header from the title's own XEX. The static-recomp runtime does not
// load a console module object, so startup publishes the exact 0x18 source
// bytes into its private guest metadata page. This preserves the standard XAM
// pointer ABI and the original big-endian fields without inventing identity.
PPC_FUNC(__imp__XamGetExecutionId) {
    RuntimeTraceImport("XamGetExecutionId", ctx);
    const uint32_t output = ctx.r3.u32;
    const uint32_t executionInfo = ExecutableExecutionInfoAddress();
    uint32_t status = kStatusSuccess;
    if (!output || (output & 3u) || output > UINT32_MAX - 4u) {
        status = kStatusInvalidParameter;
    } else if (!executionInfo) {
        status = kStatusNotFound;
    } else {
        PPC_STORE_U32(output, executionInfo);
    }
    ctx.r3.u64 = status;
    std::cout << "IMPORT_CALL name=__imp__XamGetExecutionId output=0x" << std::hex
              << output << " execution_info=0x" << executionInfo << " title_id=0x"
              << (executionInfo ? PPC_LOAD_U32(executionInfo + 0x0C) : 0u)
              << " status=0x" << status << std::dec << '\n';
}

// XamUserGetSigninState returns the X_USER_SIGNIN_STATE value directly, not an
// error code. The native runtime currently owns one portable local profile in
// slot 0. It is signed in locally (1), never implicitly signed in to Xbox Live
// (2); the remaining console user slots are empty. This matches the pinned
// ReXGlue/Xenia user-profile contract without inventing a console identity.
PPC_FUNC(__imp__XamUserGetSigninState) {
    RuntimeTraceImport("XamUserGetSigninState", ctx);
    constexpr uint32_t kSignedInLocally = 1;
    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t signinState = userIndex == 0 ? kSignedInLocally : 0;
    ctx.r3.u64 = signinState;
    std::cout << "IMPORT_CALL name=__imp__XamUserGetSigninState user=" << userIndex
              << " state=" << signinState << '\n';
}

// The Darkness requests the six standard controller/gameplay preferences
// immediately after selecting its local user. Preserve the dashboard ABI:
// the first call reports the exact required byte count and the second returns
// a header plus fixed-size X_USER_PROFILE_SETTING records. These values match
// the pinned ReXGlue profile defaults; no online identity or title data is
// fabricated.
PPC_FUNC(__imp__XamUserReadProfileSettings) {
    RuntimeTraceImport("XamUserReadProfileSettings", ctx);
    constexpr uint32_t kHeaderBytes = 8;
    constexpr uint32_t kSettingBytes = 40;
    constexpr uint8_t kProfileTypeInt32 = 1;

    const uint32_t titleId = ctx.r3.u32;
    const uint32_t userIndex = ctx.r4.u32;
    const uint32_t xuidCount = ctx.r5.u32;
    const uint32_t xuids = ctx.r6.u32;
    const uint32_t settingCount = ctx.r7.u32;
    const uint32_t settingIds = ctx.r8.u32;
    const uint32_t bufferSizeAddress = ctx.r9.u32;
    const uint32_t buffer = ctx.r10.u32;
    const uint32_t overlapped =
        (ctx.r1.u32 <= UINT32_MAX - 88u) ? PPC_LOAD_U32(ctx.r1.u32 + 84u) : 0;

    uint32_t result = 0;
    uint32_t requiredBytes = 0;
    std::vector<std::pair<uint32_t, int32_t>> settings;
    if (xuidCount || xuids || settingCount < 1 || settingCount > 32 ||
        !settingIds || (settingIds & 3u) ||
        settingIds > UINT32_MAX - settingCount * 4u ||
        !bufferSizeAddress || (bufferSizeAddress & 3u) ||
        bufferSizeAddress > UINT32_MAX - 4u) {
        result = kXErrorInvalidParameter;
    } else {
        requiredBytes = kHeaderBytes + settingCount * kSettingBytes;
        const uint32_t suppliedBytes = PPC_LOAD_U32(bufferSizeAddress);
        if (suppliedBytes && (!buffer || (buffer & 3u) ||
                              buffer > UINT32_MAX - requiredBytes)) {
            result = kXErrorInvalidParameter;
        } else if (!buffer || suppliedBytes < requiredBytes) {
            if (!suppliedBytes) PPC_STORE_U32(bufferSizeAddress, requiredBytes);
            result = kXErrorInsufficientBuffer;
        } else if (userIndex != 0) {
            result = kXErrorNoSuchUser;
        } else {
            settings.reserve(settingCount);
            for (uint32_t index = 0; index < settingCount; ++index) {
                const uint32_t settingId = PPC_LOAD_U32(settingIds + index * 4u);
                int32_t value = 0;
                if (!GetGuestXamState().GetProfileInt32Setting(settingId, &value)) {
                    result = kXErrorInvalidParameter;
                    break;
                }
                settings.emplace_back(settingId, value);
            }
        }
    }

    if (!result) {
        RuntimeGeneratedMemset(base, base + buffer, 0, requiredBytes, __FILE__, __LINE__);
        PPC_STORE_U32(buffer + 0, settingCount);
        PPC_STORE_U32(buffer + 4, buffer + kHeaderBytes);
        for (uint32_t index = 0; index < settingCount; ++index) {
            const uint32_t output = buffer + kHeaderBytes + index * kSettingBytes;
            PPC_STORE_U32(output + 0, 1); // set global profile value
            PPC_STORE_U32(output + 8, userIndex);
            PPC_STORE_U32(output + 16, settings[index].first);
            PPC_STORE_U8(output + 24, kProfileTypeInt32);
            PPC_STORE_U32(output + 32, static_cast<uint32_t>(settings[index].second));
        }
    }

    if (overlapped) {
        CompleteXamOverlappedImmediate(base, overlapped, result);
        ctx.r3.u64 = kXErrorIoPending;
    } else {
        ctx.r3.u64 = result;
    }
    std::cout << "IMPORT_CALL name=__imp__XamUserReadProfileSettings title=0x"
              << std::hex << titleId << " user=" << std::dec << userIndex
              << " settings=" << settingCount << " required=0x" << std::hex
              << requiredBytes << " buffer=0x" << buffer << " completion=0x"
              << result << " status=0x" << ctx.r3.u32 << std::dec << '\n';
}

// The reached title call requests 0x838000 bytes for user 0, content type 1,
// with no flags, and supplies both a device-id output and XAM_OVERLAPPED. The
// native runtime has exactly one real writable device rooted under its ignored
// runtime_data directory. Select it only after creating that backing directory
// and verifying host free space; this is not a dummy Xbox device or a
// success-only stub. Completion follows the pinned XAM overlapped contract.
PPC_FUNC(__imp__XamShowDeviceSelectorUI) {
    RuntimeTraceImport("XamShowDeviceSelectorUI", ctx);
    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t contentType = ctx.r4.u32;
    const uint32_t contentFlags = ctx.r5.u32;
    const uint64_t totalRequested = ctx.r6.u64;
    const uint32_t deviceId = ctx.r7.u32;
    const uint32_t overlapped = ctx.r8.u32;

    uint32_t result = 0;
    if (userIndex != 0 || contentType != 1 || contentFlags != 0 ||
        !deviceId || (deviceId & 3u) || deviceId > UINT32_MAX - 4u) {
        result = kXErrorInvalidParameter;
    } else if (!EnsureGuestPortableContentDevice(totalRequested)) {
        result = kXErrorDiskFull;
    } else {
        PPC_STORE_U32(deviceId, kGuestPortableContentDeviceId);
    }

    if (overlapped) {
        CompleteXamOverlappedImmediate(base, overlapped, result);
        ctx.r3.u64 = kXErrorIoPending;
    } else {
        ctx.r3.u64 = result;
    }
    std::cout << "IMPORT_CALL name=__imp__XamShowDeviceSelectorUI user=" << userIndex
              << " content_type=0x" << std::hex << contentType
              << " content_flags=0x" << contentFlags
              << " requested=0x" << totalRequested << " device_output=0x" << deviceId
              << " selected=0x" << (result ? 0u : kGuestPortableContentDeviceId)
              << " overlapped=0x" << overlapped << " completion=0x" << result
              << " status=0x" << ctx.r3.u32 << std::dec << '\n';
}

// Probe 60 reaches the title's real achievement catalog request after leaving
// first-run video setup. The XEX owns an authoritative XDBF table containing
// the locked achievement metadata; expose that table through the standard
// XAM enumerator layout. Strings are returned as UTF-16BE in the caller's
// buffer and no console identity or unlocked state is fabricated.
PPC_FUNC(__imp__XamUserCreateAchievementEnumerator) {
    RuntimeTraceImport("XamUserCreateAchievementEnumerator", ctx);
    constexpr uint32_t kDetailsBytes = 36;
    constexpr uint32_t kStringBytesPerItem = 464;
    const uint32_t titleId = ctx.r3.u32;
    const uint32_t userIndex = ctx.r4.u32;
    const uint32_t xuid = ctx.r5.u32;
    const uint32_t flags = ctx.r6.u32;
    const uint32_t offset = ctx.r7.u32;
    const uint32_t itemsPerEnumerate = ctx.r8.u32;
    const uint32_t bufferSizeAddress = ctx.r9.u32;
    const uint32_t handleAddress = ctx.r10.u32;
    const uint32_t itemSize = kDetailsBytes + ((flags & 7u) ? kStringBytesPerItem : 0u);
    const uint64_t bufferBytes64 = uint64_t(itemSize) * itemsPerEnumerate;
    uint32_t result = 0;
    uint32_t handle{};
    uint32_t catalogItems{};
    const auto& sourceAchievements = ExecutableAchievements();

    const uint32_t executionInfo = ExecutableExecutionInfoAddress();
    const uint32_t currentTitleId = executionInfo ? PPC_LOAD_U32(executionInfo + 0x0C) : 0;
    if (!itemsPerEnumerate || bufferBytes64 > UINT32_MAX ||
        !bufferSizeAddress || (bufferSizeAddress & 3u) ||
        bufferSizeAddress > UINT32_MAX - 4u || !handleAddress ||
        (handleAddress & 3u) || handleAddress > UINT32_MAX - 4u ||
        userIndex >= 4 || !executionInfo ||
        (titleId && titleId != currentTitleId) || offset > sourceAchievements.size()) {
        result = kXErrorInvalidParameter;
    } else {
        struct PreparedAchievement {
            RuntimeAchievement details;
            std::u16string label;
            std::u16string description;
            std::u16string unachieved;
            uint64_t unlockFileTime{};
        };
        std::vector<PreparedAchievement> prepared;
        prepared.reserve(sourceAchievements.size() - offset);
        for (size_t i = offset; i < sourceAchievements.size(); ++i) {
            const auto& achievement = sourceAchievements[i];
            PreparedAchievement item{
                achievement,
                Utf8ToUtf16(achievement.label),
                Utf8ToUtf16(achievement.description),
                Utf8ToUtf16(achievement.unachievedDescription),
            };
            GetGuestXamState().IsAchievementUnlocked(achievement.id,
                                                     &item.unlockFileTime);
            prepared.push_back(std::move(item));
        }
        catalogItems = static_cast<uint32_t>(prepared.size());
        auto writer = [prepared = std::move(prepared), flags, itemsPerEnumerate](
                          uint32_t firstItem, uint32_t itemCount,
                          uint32_t destinationGuestAddress, uint8_t* destination,
                          uint32_t destinationBytes, uint32_t* itemsWritten) -> uint32_t {
            if (itemsWritten) *itemsWritten = 0;
            if (firstItem > prepared.size() ||
                itemCount > prepared.size() - firstItem) {
                return kXErrorInvalidParameter;
            }
            const uint64_t headerBytes64 = uint64_t(itemsPerEnumerate) * kDetailsBytes;
            const uint64_t tailBytes64 = (flags & 7u)
                ? uint64_t(itemCount) * kStringBytesPerItem
                : 0u;
            if (headerBytes64 + tailBytes64 > destinationBytes ||
                headerBytes64 + tailBytes64 > UINT32_MAX - destinationGuestAddress) {
                return kXErrorInsufficientBuffer;
            }
            const size_t outputBytes = static_cast<size_t>(headerBytes64 + tailBytes64);
            const RuntimeGuestWriteCompletion outputCompletion(
                destinationGuestAddress, static_cast<uint32_t>(outputBytes));
            std::memset(destination, 0, outputBytes);
            uint8_t* stringCursor = destination + static_cast<size_t>(headerBytes64);
            uint32_t guestStringCursor = destinationGuestAddress +
                                         static_cast<uint32_t>(headerBytes64);
            size_t stringBytesRemaining = static_cast<size_t>(tailBytes64);
            for (uint32_t i = 0; i < itemCount; ++i) {
                const auto& item = prepared[firstItem + i];
                uint8_t* details = destination + size_t(i) * kDetailsBytes;
                StoreBigEndian32(details + 0, item.details.id);
                uint32_t labelAddress{};
                uint32_t descriptionAddress{};
                uint32_t unachievedAddress{};
                if ((flags & 1u) &&
                    !AppendUtf16BigEndian(item.label, stringCursor,
                                          guestStringCursor, stringBytesRemaining,
                                          &labelAddress))
                    return kXErrorInsufficientBuffer;
                if ((flags & 2u) &&
                    !AppendUtf16BigEndian(item.description, stringCursor,
                                          guestStringCursor, stringBytesRemaining,
                                          &descriptionAddress))
                    return kXErrorInsufficientBuffer;
                if ((flags & 4u) &&
                    !AppendUtf16BigEndian(item.unachieved, stringCursor,
                                          guestStringCursor, stringBytesRemaining,
                                          &unachievedAddress))
                    return kXErrorInsufficientBuffer;
                StoreBigEndian32(details + 4, labelAddress);
                StoreBigEndian32(details + 8, descriptionAddress);
                StoreBigEndian32(details + 12, unachievedAddress);
                StoreBigEndian32(details + 16, item.details.imageId);
                StoreBigEndian32(details + 20, item.details.gamerscore);
                StoreBigEndian32(details + 24,
                                 static_cast<uint32_t>(item.unlockFileTime));
                StoreBigEndian32(details + 28,
                                 static_cast<uint32_t>(item.unlockFileTime >> 32));
                constexpr uint32_t kAchievedFlags = 0x00030000u;
                StoreBigEndian32(details + 32, item.details.flags |
                    (item.unlockFileTime ? kAchievedFlags : 0u));
            }
            if (itemsWritten) *itemsWritten = itemCount;
            return 0;
        };
        const auto enumerator = GetGuestObjects().CreateEnumerator(
            itemsPerEnumerate, itemSize, catalogItems, std::move(writer));
        handle = enumerator.handle;
        PPC_STORE_U32(bufferSizeAddress, static_cast<uint32_t>(bufferBytes64));
        PPC_STORE_U32(handleAddress, handle);
    }
    if (result) {
        if (bufferSizeAddress && !(bufferSizeAddress & 3u) &&
            bufferSizeAddress <= UINT32_MAX - 4u)
            PPC_STORE_U32(bufferSizeAddress, 0);
        if (handleAddress && !(handleAddress & 3u) &&
            handleAddress <= UINT32_MAX - 4u)
            PPC_STORE_U32(handleAddress, 0);
    }
    ctx.r3.u64 = result;
    std::cout << "IMPORT_CALL name=__imp__XamUserCreateAchievementEnumerator title=0x"
              << std::hex << titleId << " current_title=0x" << currentTitleId
              << " user=" << std::dec << userIndex << " xuid=0x" << std::hex << xuid
              << " flags=0x" << flags << " offset=" << std::dec << offset
              << " items_per_call=" << itemsPerEnumerate << " catalog_items="
              << catalogItems << " buffer_bytes=" << bufferBytes64 << " handle=0x"
              << std::hex << handle << " status=0x" << result << std::dec << '\n';
}

// The post-Start save scan uses the standard seven-argument XAM content
// enumerator ABI. The portable content device currently contains no
// runtime-created saves, so this creates a real empty enumerator rather than
// fabricating an entry or returning success without a usable handle.
namespace {
// Test harness only (level-start runs, scripts/run-level-coverage.ps1): the
// game's developer -MAP start path runs before any controller is assigned, so
// its save-device calls carry user index 0xFFFFFFFF. With
// REX_TEST_UNSET_USER_AS_LOCAL=1 those calls use the local profile (slot 0).
// Unset in every ordinary launch, where this returns the index unchanged.
uint32_t ContentUserIndex(uint32_t userIndex) {
    static const bool enabled = [] {
        const char* value = std::getenv("REX_TEST_UNSET_USER_AS_LOCAL");
        return value && value[0] == '1';
    }();
    if (!enabled || userIndex != 0xFFFFFFFFu) return userIndex;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) std::cout << "TEST_HOOK unset_user_as_local=1\n";
    return 0;
}
}  // namespace

PPC_FUNC(__imp__XamContentCreateEnumerator) {
    RuntimeTraceImport("XamContentCreateEnumerator", ctx);
    constexpr uint32_t kXContentDataBytes = 0x134;
    const uint32_t userIndex = ContentUserIndex(ctx.r3.u32);
    const uint32_t deviceId = ctx.r4.u32;
    const uint32_t contentType = ctx.r5.u32;
    const uint32_t contentFlags = ctx.r6.u32;
    const uint32_t itemsPerEnumerate = ctx.r7.u32;
    const uint32_t bufferSizeAddress = ctx.r8.u32;
    const uint32_t handleAddress = ctx.r9.u32;
    uint32_t result = 0;
    uint32_t handle = 0;
    uint32_t catalogItems = 0;
    const uint64_t bufferBytes64 = uint64_t(kXContentDataBytes) * itemsPerEnumerate;
    if (userIndex != 0 || (deviceId != 0 && deviceId != kGuestPortableContentDeviceId) ||
        !itemsPerEnumerate || bufferBytes64 > UINT32_MAX || !handleAddress ||
        (handleAddress & 3u) || handleAddress > UINT32_MAX - 4u ||
        (bufferSizeAddress && ((bufferSizeAddress & 3u) ||
                               bufferSizeAddress > UINT32_MAX - 4u))) {
        result = kXErrorInvalidParameter;
    } else if (deviceId && !EnsureGuestPortableContentDevice(0)) {
        result = kXErrorNotFound;
    } else {
        std::vector<uint8_t> items;
        const uint32_t executionInfo = ExecutableExecutionInfoAddress();
        if (executionInfo && (deviceId == 0 || deviceId == kGuestPortableContentDeviceId)) {
            items = EnumerateGuestPortableContent(PPC_LOAD_U32(executionInfo + 0x0C),
                                                  contentType);
            catalogItems = static_cast<uint32_t>(items.size() / kGuestXContentDataBytes);
        }
        const auto enumerator = GetGuestObjects().CreateEnumerator(
            itemsPerEnumerate, kXContentDataBytes, std::move(items));
        handle = enumerator.handle;
        PPC_STORE_U32(handleAddress, handle);
    }
    if (bufferSizeAddress) {
        PPC_STORE_U32(bufferSizeAddress,
                      result ? 0u : static_cast<uint32_t>(bufferBytes64));
    }
    ctx.r3.u64 = result;
    std::cout << "IMPORT_CALL name=__imp__XamContentCreateEnumerator user=" << userIndex
              << " device=0x" << std::hex << deviceId << " content_type=0x" << contentType
              << " content_flags=0x" << contentFlags << " items=" << std::dec
              << itemsPerEnumerate << " catalog_items=" << catalogItems
              << " buffer_bytes=" << bufferBytes64 << " handle=0x"
              << std::hex << handle << " status=0x" << result << std::dec << '\n';
}

// The post-Start path creates the title's first real saved-game container and
// mounts it as "savedrive:". Persist the exact XCONTENT_DATA record for later
// enumeration, keep all writable data below runtime_data/content, and expose
// only that package through the explicit host mount. The extracted title tree
// remains read-only and no console XUID or signing identity is invented.
PPC_FUNC(__imp__XamContentCreateEx) {
    RuntimeTraceImport("XamContentCreateEx", ctx);
    const uint32_t userIndex = ContentUserIndex(ctx.r3.u32);
    const uint32_t rootNameAddress = ctx.r4.u32;
    const uint32_t contentDataAddress = ctx.r5.u32;
    const uint32_t flags = ctx.r6.u32;
    const uint32_t dispositionAddress = ctx.r7.u32;
    const uint32_t licenseMaskAddress = ctx.r8.u32;
    const uint32_t cacheSize = ctx.r9.u32;
    const uint64_t contentSize = ctx.r10.u64;
    const uint32_t overlapped =
        (ctx.r1.u32 <= UINT32_MAX - 88u) ? PPC_LOAD_U32(ctx.r1.u32 + 84u) : 0;

    uint32_t result = 0;
    uint32_t disposition = 0;
    std::string rootName;
    std::string fileName;
    std::filesystem::path packagePath;
    if (userIndex != 0 || !contentDataAddress ||
        contentDataAddress > UINT32_MAX - kGuestXContentDataBytes ||
        (dispositionAddress && ((dispositionAddress & 3u) ||
                                dispositionAddress > UINT32_MAX - 4u)) ||
        (licenseMaskAddress && ((licenseMaskAddress & 3u) ||
                                licenseMaskAddress > UINT32_MAX - 4u))) {
        result = kXErrorInvalidParameter;
    } else {
        rootName = ReadGuestCString(base, rootNameAddress, 64);
        const char* rawFileName = reinterpret_cast<const char*>(
            base + contentDataAddress + 0x108);
        const size_t fileNameBytes = strnlen(rawFileName, 42);
        fileName.assign(rawFileName, fileNameBytes);
        std::array<uint8_t, kGuestXContentDataBytes> contentData{};
        std::memcpy(contentData.data(), base + contentDataAddress, contentData.size());
        const uint32_t executionInfo = ExecutableExecutionInfoAddress();
        if (!executionInfo) {
            result = kXErrorInvalidParameter;
        } else {
            const auto operation = OpenGuestPortableContent(
                rootName, PPC_LOAD_U32(executionInfo + 0x0C),
                PPC_LOAD_U32(contentDataAddress + 4), fileName, contentData,
                flags & 0xFu, contentSize);
            disposition = operation.disposition;
            packagePath = operation.packagePath;
            switch (operation.result) {
            case GuestPortableContentResult::Success:
                result = 0;
                break;
            case GuestPortableContentResult::AlreadyExists:
                result = kXErrorAlreadyExists;
                break;
            case GuestPortableContentResult::FileNotFound:
                result = kXErrorFileNotFound;
                break;
            case GuestPortableContentResult::PathNotFound:
                result = kXErrorPathNotFound;
                break;
            case GuestPortableContentResult::AccessDenied:
                result = kXErrorAccessDenied;
                break;
            default:
                result = kXErrorInvalidParameter;
                break;
            }
        }
    }
    if (dispositionAddress) PPC_STORE_U32(dispositionAddress, disposition);
    if (licenseMaskAddress && !result) PPC_STORE_U32(licenseMaskAddress, 0);
    if (overlapped) {
        CompleteXamOverlappedImmediateEx(base, overlapped, result,
                                         XamHresultFromWin32(result), disposition);
        ctx.r3.u64 = kXErrorIoPending;
    } else {
        ctx.r3.u64 = result;
    }
    std::cout << "IMPORT_CALL name=__imp__XamContentCreateEx user=" << userIndex
              << " root=" << rootName << " device=0x" << std::hex
              << (contentDataAddress ? PPC_LOAD_U32(contentDataAddress) : 0u)
              << " content_type=0x"
              << (contentDataAddress ? PPC_LOAD_U32(contentDataAddress + 4) : 0u)
              << " file=" << fileName << " flags=0x" << flags << " cache=0x"
              << cacheSize << " content_size=0x" << contentSize << " disposition="
              << std::dec << disposition << " overlapped=0x" << std::hex << overlapped
              << " completion=0x" << result << " status=0x" << ctx.r3.u32
              << " package=" << packagePath.string() << std::dec << '\n';
}

// The title's 16-state content worker has a direct synchronous delete branch
// with the standard (user, XCONTENT_DATA*, XAM_OVERLAPPED*) ABI. Delete only
// the matching package and catalog header beneath the runtime-owned portable
// device. A mounted package remains open and is rejected, matching the pinned
// ReXGlue content-manager contract; the extracted title tree is never in scope.
PPC_FUNC(__imp__XamContentDelete) {
    RuntimeTraceImport("XamContentDelete", ctx);
    const uint32_t userIndex = ContentUserIndex(ctx.r3.u32);
    const uint32_t contentDataAddress = ctx.r4.u32;
    const uint32_t overlapped = ctx.r5.u32;
    uint32_t result = kXErrorInvalidParameter;
    std::string fileName;
    std::filesystem::path packagePath;
    if (userIndex == 0 && contentDataAddress &&
        contentDataAddress <= UINT32_MAX - kGuestXContentDataBytes) {
        std::array<uint8_t, kGuestXContentDataBytes> contentData{};
        std::memcpy(contentData.data(), base + contentDataAddress,
                    contentData.size());
        const char* rawFileName = reinterpret_cast<const char*>(
            base + contentDataAddress + 0x108);
        fileName.assign(rawFileName, strnlen(rawFileName, 42));
        const uint32_t executionInfo = ExecutableExecutionInfoAddress();
        if (executionInfo) {
            const auto operation = DeleteGuestPortableContent(
                PPC_LOAD_U32(executionInfo + 0x0C),
                PPC_LOAD_U32(contentDataAddress + 4), fileName, contentData);
            packagePath = operation.packagePath;
            switch (operation.result) {
            case GuestPortableContentResult::Success:
                result = 0;
                break;
            case GuestPortableContentResult::FileNotFound:
            case GuestPortableContentResult::PathNotFound:
                result = kXErrorFileNotFound;
                break;
            case GuestPortableContentResult::AccessDenied:
                result = kXErrorAccessDenied;
                break;
            default:
                result = kXErrorInvalidParameter;
                break;
            }
        }
    }
    if (overlapped) {
        CompleteXamOverlappedImmediate(base, overlapped, result);
        ctx.r3.u64 = kXErrorIoPending;
    } else {
        ctx.r3.u64 = result;
    }
    std::cout << "IMPORT_CALL name=__imp__XamContentDelete user=" << userIndex
              << " device=0x" << std::hex
              << (contentDataAddress ? PPC_LOAD_U32(contentDataAddress) : 0u)
              << " content_type=0x"
              << (contentDataAddress ? PPC_LOAD_U32(contentDataAddress + 4) : 0u)
              << " file=" << fileName << " overlapped=0x" << overlapped
              << " completion=0x" << result << " status=0x" << ctx.r3.u32
              << " package=" << packagePath.string() << std::dec << '\n';
}

// Closing a content root unmounts its package without deleting the portable
// directory or its catalog header. This is the exact paired operation reached
// after The Darkness creates DEFAULT; later boots must still enumerate and
// reopen that saved-game container.
PPC_FUNC(__imp__XamContentClose) {
    RuntimeTraceImport("XamContentClose", ctx);
    const uint32_t rootNameAddress = ctx.r3.u32;
    const uint32_t overlapped = ctx.r4.u32;
    const std::string rootName = ReadGuestCString(base, rootNameAddress, 64);
    const uint32_t result = CloseGuestPortableContent(rootName)
                                ? 0u
                                : kXErrorFileNotFound;
    if (overlapped) {
        CompleteXamOverlappedImmediate(base, overlapped, result);
        ctx.r3.u64 = kXErrorIoPending;
    } else {
        ctx.r3.u64 = result;
    }
    std::cout << "IMPORT_CALL name=__imp__XamContentClose root=" << rootName
              << " overlapped=0x" << std::hex << overlapped << " completion=0x"
              << result << " status=0x" << ctx.r3.u32 << std::dec << '\n';
}

// The title queries the selected storage device immediately after creating
// and closing its initial DEFAULT container. Device 1 is connected exactly
// when the runtime-owned portable content root is available. Match the pinned
// XAM synchronous and event-backed overlapped contracts; do not report a
// disconnected or unwritable host directory as a usable Xbox device.
PPC_FUNC(__imp__XamContentGetDeviceState) {
    RuntimeTraceImport("XamContentGetDeviceState", ctx);
    const uint32_t deviceId = ctx.r3.u32;
    const uint32_t overlapped = ctx.r4.u32;
    const bool connected = deviceId == kGuestPortableContentDeviceId &&
                           EnsureGuestPortableContentDevice(0);
    const uint32_t result = connected ? 0u : kXErrorDeviceNotConnected;
    if (overlapped) {
        if (connected) {
            CompleteXamOverlappedImmediate(base, overlapped, 0);
        } else {
            CompleteXamOverlappedImmediateEx(base, overlapped,
                                              kXErrorFunctionFailed,
                                              kXErrorDeviceNotConnected, 0);
        }
        ctx.r3.u64 = kXErrorIoPending;
    } else {
        ctx.r3.u64 = result;
    }
    std::cout << "IMPORT_CALL name=__imp__XamContentGetDeviceState device=0x"
              << std::hex << deviceId << " connected=" << (connected ? 1 : 0)
              << " overlapped=0x" << overlapped << " completion=0x" << result
              << " status=0x" << ctx.r3.u32 << std::dec << '\n';
}

PPC_FUNC(__imp__XamEnumerate) {
    RuntimeTraceImport("XamEnumerate", ctx);
    const uint32_t handle = ctx.r3.u32;
    const uint32_t flags = ctx.r4.u32;
    const uint32_t bufferAddress = ctx.r5.u32;
    const uint32_t bufferLength = ctx.r6.u32;
    const uint32_t itemsReturnedAddress = ctx.r7.u32;
    const uint32_t overlapped = ctx.r8.u32;
    uint32_t itemCount = 0;
    uint32_t result = kXErrorInvalidParameter;
    if (!flags && bufferAddress && bufferLength <= UINT32_MAX - bufferAddress &&
        (!itemsReturnedAddress || (!(itemsReturnedAddress & 3u) &&
         itemsReturnedAddress <= UINT32_MAX - 4u))) {
        result = GetGuestObjects().Enumerate(handle, bufferAddress,
                                             base + bufferAddress, bufferLength,
                                             &itemCount);
    }
    if (!overlapped) {
        if (itemsReturnedAddress) PPC_STORE_U32(itemsReturnedAddress, itemCount);
        ctx.r3.u64 = result;
    } else {
        CompleteXamOverlappedImmediateEx(base, overlapped, result,
                                         XamHresultFromWin32(result), itemCount);
        ctx.r3.u64 = kXErrorIoPending;
    }
    std::cout << "IMPORT_CALL name=__imp__XamEnumerate handle=0x" << std::hex << handle
              << " flags=0x" << flags << " buffer=0x" << bufferAddress << " length=0x"
              << bufferLength << " items=" << std::dec << itemCount << " overlapped=0x"
              << std::hex << overlapped << " completion=0x" << result << " status=0x"
              << ctx.r3.u32 << std::dec << '\n';
}

// The Darkness reaches this while enumerating users 0 through 3. Preserve the
// Xbox XAM validation and big-endian output contract, but obtain connection and
// capability data from the host's real XInput provider.
PPC_FUNC(__imp__XamInputGetCapabilities) {
    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t flags = ctx.r4.u32;
    const uint32_t output = ctx.r5.u32;
    const uint32_t result = QueryGuestInputCapabilities(base, userIndex, flags, output);
    ctx.r3.u64 = result;
    if (xamInputCapabilitiesTransitions[InputDiagnosticSlot(userIndex)].Update(result, 0)) {
        RuntimeTraceImport("XamInputGetCapabilities", ctx);
        QueueRuntimeInputDiagnostic(MakeInputDiagnosticEvent(
            RuntimeInputDiagnosticKind::Capabilities, ctx, userIndex, flags, output, result));
    }
}

// The title polls this from its input manager. Preserve the same user/flags
// selection as the capability query and expose only the host's actual XInput
// connection and state; disconnected users remain disconnected.
namespace {
const bool traceStickCommands = [] {
    const char* value = std::getenv("DARKNESS_TRACE_STICK_COMMANDS");
    return value && value[0] == '1';
}();
std::atomic<bool> traceStickDeflected{};
}  // namespace

void RuntimeNoteRightStick(int16_t x, int16_t y) noexcept {
    if (!traceStickCommands) return;
    traceStickDeflected.store(x > 8000 || x < -8000 || y > 8000 || y < -8000,
                              std::memory_order_relaxed);
}

bool RuntimeStickCommandTraceActive() noexcept {
    return traceStickCommands && traceStickDeflected.load(std::memory_order_relaxed);
}

PPC_FUNC(__imp__XamInputGetState) {
    // Developer check of the fatal-stop report (V380): with
    // DARKNESS_TEST_FATAL_AFTER_SECONDS=N the game stops here after N seconds,
    // through the same path as a real fatal stop. Unset in every player launch.
    static const double testFatalAfterSeconds = [] {
        const char* value = std::getenv("DARKNESS_TEST_FATAL_AFTER_SECONDS");
        return value ? std::atof(value) : 0.0;
    }();
    if (testFatalAfterSeconds > 0.0) {
        static const auto started = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >
            testFatalAfterSeconds) {
            char lr[24];
            _snprintf_s(lr, _TRUNCATE, "0x%08llX", static_cast<unsigned long long>(ctx.lr));
            RuntimeFatalRecordDetail(std::string("developer test stop lr=") + lr);
            RuntimeFatalWriteDump("developer-test");
            throw std::runtime_error(
                "developer test of the fatal-stop report (DARKNESS_TEST_FATAL_AFTER_SECONDS)");
        }
    }
    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t flags = ctx.r4.u32;
    const uint32_t output = ctx.r5.u32;
    const uint32_t result = QueryGuestInputState(base, userIndex, flags, output);
    ctx.r3.u64 = result;
    uint32_t packet = 0;
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t thumbLX = 0;
    int16_t thumbLY = 0;
    int16_t thumbRX = 0;
    int16_t thumbRY = 0;
    if (result == ERROR_SUCCESS) {
        packet = PPC_LOAD_U32(output);
        buttons = PPC_LOAD_U16(output + 4);
        leftTrigger = base[output + 6];
        rightTrigger = base[output + 7];
        thumbLX = static_cast<int16_t>(PPC_LOAD_U16(output + 8));
        thumbLY = static_cast<int16_t>(PPC_LOAD_U16(output + 10));
        thumbRX = static_cast<int16_t>(PPC_LOAD_U16(output + 12));
        thumbRY = static_cast<int16_t>(PPC_LOAD_U16(output + 14));
        RuntimeNoteRightStick(thumbRX, thumbRY);
    }
    if (xamInputStateTransitions[InputDiagnosticSlot(userIndex)].Update(result, buttons)) {
        RuntimeTraceImport("XamInputGetState", ctx);
        auto event = MakeInputDiagnosticEvent(RuntimeInputDiagnosticKind::State, ctx,
                                              userIndex, flags, output, result);
        event.packetNumber = packet;
        event.buttons = buttons;
        event.leftTrigger = leftTrigger;
        event.rightTrigger = rightTrigger;
        event.thumbLX = thumbLX;
        event.thumbLY = thumbLY;
        event.thumbRX = thumbRX;
        event.thumbRY = thumbRY;
        QueueRuntimeInputDiagnostic(event);
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        RuntimeGraphicsNoteInputTransition(event.hostPerformanceCounter,
                                           frequency.QuadPart, packet, buttons);
    }
}

// Xbox vibration is two big-endian motor speeds. Send the observed title
// request to the same real XInput provider used for state polling and return
// its connection/error status unchanged.
PPC_FUNC(__imp__XamInputSetState) {
    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t flags = ctx.r4.u32;
    const uint32_t input = ctx.r5.u32;
    RuntimeInputVibration vibration{};
    if (input && input <= UINT32_MAX - sizeof(uint32_t)) {
        vibration = LoadGuestInputVibration(base, input);
    }
    const uint32_t result = SetGuestInputVibration(base, userIndex, flags, input);
    ctx.r3.u64 = result;
    const uint32_t signature =
        (uint32_t(vibration.leftMotorSpeed) << 16) | vibration.rightMotorSpeed;
    if (xamInputVibrationTransitions[InputDiagnosticSlot(userIndex)].Update(result,
                                                                            signature)) {
        RuntimeTraceImport("XamInputSetState", ctx);
        auto event = MakeInputDiagnosticEvent(RuntimeInputDiagnosticKind::Vibration, ctx,
                                              userIndex, flags, input, result);
        event.leftMotorSpeed = vibration.leftMotorSpeed;
        event.rightMotorSpeed = vibration.rightMotorSpeed;
        QueueRuntimeInputDiagnostic(event);
    }
}

// NetDll_XNetRandom(caller, buffer, length) fills the caller-supplied guest
// buffer with random bytes and returns zero on success. The title dynamically
// reaches this with caller 1 and an eight-byte buffer while loading registry
// state. Use the host system RNG rather than Xenia's deterministic 0xBB stub,
// which is useful for emulator replay but is not the function's semantics.
PPC_FUNC(__imp__NetDll_XNetRandom) {
    RuntimeTraceImport("NetDll_XNetRandom", ctx);
    const uint32_t caller = ctx.r3.u32;
    const uint32_t buffer = ctx.r4.u32;
    const uint32_t length = ctx.r5.u32;
    if ((length && !buffer) || length > UINT32_MAX - buffer)
        throw std::runtime_error("NetDll_XNetRandom reached an invalid guest buffer");
    if (length && BCryptGenRandom(nullptr, base + buffer, length,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("NetDll_XNetRandom host RNG failed");
    ctx.r3.u64 = 0;
    std::cout << "IMPORT_CALL name=__imp__NetDll_XNetRandom caller=" << caller
              << " buffer=0x" << std::hex << buffer << " bytes=0x" << length
              << " status=0x0" << std::dec << '\n';
}

// XamNotifyCreateListener(mask, max_version) returns a waitable notification
// listener handle. No title notifications are synthesized here: the queue is
// initially empty and can only become signalled when a real producer exists.
PPC_FUNC(__imp__XamNotifyCreateListener) {
    RuntimeTraceImport("XamNotifyCreateListener", ctx);
    const uint64_t mask = ctx.r3.u64;
    const uint32_t maxVersion = std::min(ctx.r4.u32, uint32_t(10));
    const auto listener = GetGuestObjects().CreateNotifyListener(mask, maxVersion);
    ctx.r3.u64 = listener.handle;
    std::cout << "IMPORT_CALL name=__imp__XamNotifyCreateListener mask=0x" << std::hex
              << mask << " max_version=" << std::dec << maxVersion << " handle=0x"
              << std::hex << listener.handle << std::dec << '\n';
}

// Position is guest-visible XAM configuration even though the runtime has no
// notification overlay yet. Retain it rather than treating the call as a
// meaningless success stub.
PPC_FUNC(__imp__XNotifyPositionUI) {
    RuntimeTraceImport("XNotifyPositionUI", ctx);
    notifyUiPosition.store(ctx.r3.u32, std::memory_order_relaxed);
    std::cout << "IMPORT_CALL name=__imp__XNotifyPositionUI position=" << ctx.r3.u32 << '\n';
}

PPC_FUNC(__imp__XNotifyGetNext) {
    RuntimeTraceImport("XNotifyGetNext", ctx);
    const uint32_t handle = ctx.r3.u32;
    const uint32_t matchId = ctx.r4.u32;
    const uint32_t idAddress = ctx.r5.u32;
    const uint32_t parameterAddress = ctx.r6.u32;
    if (parameterAddress) PPC_STORE_U32(parameterAddress, 0);
    if (!idAddress) {
        ctx.r3.u64 = 0;
        return;
    }
    PPC_STORE_U32(idAddress, 0);
    uint32_t id{};
    uint32_t parameter{};
    const bool dequeued = GetGuestObjects().DequeueNotification(
        handle, matchId, &id, &parameter);
    if (dequeued) {
        PPC_STORE_U32(idAddress, id);
        if (parameterAddress) PPC_STORE_U32(parameterAddress, parameter);
    }
    ctx.r3.u64 = dequeued ? 1 : 0;
    if (dequeued || kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__XNotifyGetNext handle=0x" << std::hex
                  << handle << " match=0x" << matchId << " id=0x" << id
                  << " parameter=0x" << parameter << std::dec
                  << " dequeued=" << dequeued << '\n';
    }
}

// The reached XGI message is XGIUserSetContextEx. Xenia dispatches the
// asynchronous API synchronously when no XAM_OVERLAPPED is supplied, reading
// a 24-byte big-endian payload and returning X_E_SUCCESS. Retain the context
// value because later XGI queries may depend on it. The second reached message
// is XMPCaptureOutput. The title requests it asynchronously with a zeroed
// XAM_OVERLAPPED, and both pinned ReXGlue and current Xenia report X_E_FAIL
// because no system-music output capture service is available. Complete that
// real failure asynchronously; never claim that host audio capture started.
PPC_FUNC(__imp__XMsgStartIORequest) {
    RuntimeTraceImport("XMsgStartIORequest", ctx);
    const uint32_t app = ctx.r3.u32;
    const uint32_t message = ctx.r4.u32;
    const uint32_t overlapped = ctx.r5.u32;
    const uint32_t buffer = ctx.r6.u32;
    const uint32_t length = ctx.r7.u32;
    std::ostringstream contract;
    contract << "XMSG_CONTRACT thread=" << CurrentGuestThreadId() << " lr=0x" << std::hex
             << ctx.lr << " app=0x" << app << " message=0x" << message
             << " overlapped=0x" << overlapped << " buffer=0x" << buffer << std::dec
             << " length=" << length;
    if (buffer && length && length <= UINT32_MAX - buffer) {
        const uint32_t captured = std::min<uint32_t>(length, 64);
        contract << " payload=";
        static constexpr char kHex[] = "0123456789abcdef";
        for (uint32_t offset = 0; offset < captured; ++offset) {
            const uint8_t value = PPC_LOAD_U8(buffer + offset);
            contract << kHex[value >> 4] << kHex[value & 0xFu];
        }
        if (captured != length) contract << "...";
    }
    std::cout << contract.str() << std::endl;
    if (app == 0xFBu && message == 0x000B0006u && !overlapped &&
        length == 24 && buffer && length <= UINT32_MAX - buffer) {
        const uint32_t userIndex = PPC_LOAD_U32(buffer + 0);
        const uint32_t contextId = PPC_LOAD_U32(buffer + 16);
        const uint32_t value = PPC_LOAD_U32(buffer + 20);
        GetGuestXamState().SetUserContext(userIndex, contextId, value);
        ctx.r3.u64 = 0;
        std::cout << "IMPORT_CALL name=__imp__XMsgStartIORequest app=0x" << std::hex
                  << app << " message=0x" << message << " user=0x" << userIndex
                  << " context=0x" << contextId << " value=0x" << value
                  << " status=0x0" << std::dec << '\n';
        return;
    }
    // The Probe73 gameplay blocker is the standard XGIUserWriteAchievements
    // message. The title's wrapper at 0x828A7528 constructs the exact 8-byte
    // outer payload, and its caller at 0x827A9DA8 constructs each 8-byte
    // XUSER_ACHIEVEMENT as {user index, achievement id}. Persist only catalog
    // IDs from this title's XDBF; never claim success for an invalid record or
    // a failed portable-store write. ReXGlue and Xenia both dispatch this
    // synchronously before applying the ordinary XAM_OVERLAPPED completion.
    if (app == 0xFBu && message == 0x000B0008u) {
        uint32_t completion = 0;
        uint32_t achievementCount{};
        uint32_t achievements{};
        uint32_t unlocked{};
        uint32_t alreadyUnlocked{};
        if (length != 8 || !buffer || (buffer & 3u) ||
            buffer > UINT32_MAX - 8u) {
            completion = kXErrorFail;
        } else {
            achievementCount = PPC_LOAD_U32(buffer + 0);
            achievements = PPC_LOAD_U32(buffer + 4);
            const auto& catalog = ExecutableAchievements();
            const uint64_t achievementBytes = uint64_t(achievementCount) * 8u;
            if (!achievementCount || achievementCount > catalog.size() ||
                !achievements || (achievements & 3u) ||
                achievementBytes > UINT32_MAX ||
                achievementBytes > UINT32_MAX - achievements) {
                completion = kXErrorFail;
            } else {
                for (uint32_t index = 0; index < achievementCount; ++index) {
                    const uint32_t record = achievements + index * 8u;
                    const uint32_t userIndex = PPC_LOAD_U32(record + 0);
                    const uint32_t achievementId = PPC_LOAD_U32(record + 4);
                    const auto result = GetGuestXamState().UnlockAchievement(
                        userIndex, achievementId);
                    if (result == GuestAchievementUnlockResult::Unlocked) {
                        ++unlocked;
                    } else if (result == GuestAchievementUnlockResult::AlreadyUnlocked) {
                        ++alreadyUnlocked;
                    } else {
                        completion = kXErrorFail;
                        break;
                    }
                }
            }
        }
        if (overlapped) {
            CompleteXamOverlappedImmediate(base, overlapped, completion);
            ctx.r3.u64 = kXErrorIoPending;
        } else {
            ctx.r3.u64 = completion;
        }
        std::cout << "IMPORT_CALL name=__imp__XMsgStartIORequest app=0x" << std::hex
                  << app << " message=0x" << message << " count=" << std::dec
                  << achievementCount << " entries=0x" << std::hex << achievements
                  << std::dec << " unlocked=" << unlocked << " already_unlocked="
                  << alreadyUnlocked << " completion=0x" << std::hex << completion
                  << " status=0x" << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    if (app == 0xFAu && message == 0x0007003Du && length == 16 && buffer &&
        length <= UINT32_MAX - buffer && PPC_LOAD_U32(buffer) == 2) {
        if (overlapped) {
            CompleteXamOverlappedImmediate(base, overlapped, kXErrorFail);
            ctx.r3.u64 = kXErrorIoPending;
        } else {
            ctx.r3.u64 = kXErrorFail;
        }
        std::cout << "IMPORT_CALL name=__imp__XMsgStartIORequest app=0x" << std::hex
                  << app << " message=0x" << message << " callback=0x"
                  << PPC_LOAD_U32(buffer + 4) << " callback_context=0x"
                  << PPC_LOAD_U32(buffer + 8) << " enabled=0x"
                  << PPC_LOAD_U32(buffer + 12) << " completion=0x" << kXErrorFail
                  << " status=0x" << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    if (app == 0xFAu && message == 0x0007000Cu && length == 8 && buffer &&
        length <= UINT32_MAX - buffer && PPC_LOAD_U32(buffer) == 2) {
        const uint32_t volumeBits = PPC_LOAD_U32(buffer + 4);
        GetGuestXamState().SetXmpVolumeBits(volumeBits);
        if (overlapped) {
            CompleteXamOverlappedImmediate(base, overlapped, 0);
            ctx.r3.u64 = kXErrorIoPending;
        } else {
            ctx.r3.u64 = 0;
        }
        std::cout << "IMPORT_CALL name=__imp__XMsgStartIORequest app=0x" << std::hex
                  << app << " message=0x" << message << " volume_bits=0x" << volumeBits
                  << " status=0x" << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    if (app == 0xFAu && message == 0x0007001Au && length == 12 && buffer &&
        length <= UINT32_MAX - buffer) {
        const uint32_t xmpClient = PPC_LOAD_U32(buffer + 0);
        const uint32_t controller = PPC_LOAD_U32(buffer + 4);
        const uint32_t playbackClient = PPC_LOAD_U32(buffer + 8);
        // The Darkness sub_828D5510 explicitly emits (2, 4, 0) when it
        // releases playback control. Controller 4 is therefore a title-used
        // release selector, not an arbitrary controller index. Keep this
        // acceptance narrow so other unverified payloads still fail loudly.
        const bool titleReleaseController =
            xmpClient == 2 && controller == 4 && playbackClient == 0;
        const bool validController =
            (xmpClient == 2 && controller == 0) ||
            (xmpClient == 0 && controller == 1) || titleReleaseController;
        if (!validController || playbackClient > 1)
            throw std::runtime_error("XMPSetPlaybackController reached an invalid payload");
        GetGuestXamState().SetXmpPlaybackClient(playbackClient);
        constexpr uint32_t kPlaybackControllerChanged = 0x0A000003u;
        const uint32_t notifications = GetGuestObjects().BroadcastNotification(
            kPlaybackControllerChanged, playbackClient ? 0u : 1u);
        if (overlapped) {
            CompleteXamOverlappedImmediate(base, overlapped, 0);
            ctx.r3.u64 = kXErrorIoPending;
        } else {
            ctx.r3.u64 = 0;
        }
        std::cout << "IMPORT_CALL name=__imp__XMsgStartIORequest app=0x" << std::hex
                  << app << " message=0x" << message << " xmp_client=0x" << xmpClient
                  << " controller=0x" << controller << " playback_client=0x"
                  << playbackClient << std::dec << " notifications=" << notifications
                  << " status=0x" << std::hex << ctx.r3.u32 << std::dec << '\n';
        return;
    }
    // Xbox LIVE sessions and statistics (XUserSetProperty 0xB0007, the
    // XSession family 0xB0010-0xB001C, XUserReadStats 0xB0021,
    // XSessionWriteStats 0xB0025, 0xB0041): a console without LIVE fails them
    // and the title copes. Before V380 they ended the game; answer like an
    // offline console instead (logged once per message).
    if (app == 0xFBu && RuntimeIsOfflineXgiMessage(message)) {
        if (overlapped) {
            CompleteXamOverlappedImmediate(base, overlapped, kXErrorFunctionFailed);
            ctx.r3.u64 = kXErrorIoPending;
        } else {
            ctx.r3.u64 = kXErrorFunctionFailed;
        }
        static std::atomic<uint64_t> logged{};
        const uint64_t bit = 1ull << ((message & 0xFFu) % 64u);
        if (!(logged.fetch_or(bit, std::memory_order_relaxed) & bit)) {
            std::cout << "IMPORT_CALL name=__imp__XMsgStartIORequest app=0x" << std::hex << app
                      << " message=0x" << message << " offline_live_failure status=0x"
                      << ctx.r3.u32 << std::dec << '\n';
        }
        return;
    }
    throw std::runtime_error("XMsgStartIORequest reached an unverified XAM message: " +
                             contract.str());
}

// The Ex form adds an optional sixth argument that the pinned XAM dispatcher
// forwards but does not consume. All reached message semantics, including
// overlapped completion and notification delivery, are shared with the base
// entry point rather than duplicated or reduced to success-only behavior.
PPC_FUNC(__imp__XMsgStartIORequestEx) {
    const uint32_t unknown = ctx.r8.u32;
    RuntimeTraceImport("XMsgStartIORequestEx", ctx);
    std::cout << "IMPORT_CALL name=__imp__XMsgStartIORequestEx unknown=0x"
              << std::hex << unknown << std::dec << '\n';
    __imp__XMsgStartIORequest(ctx, base);
}

// Xbox X_VIDEO_MODE is a 48-byte, big-endian structure. The title's three
// call sites consume width, height, widescreen state, and the IEEE-754 refresh
// rate at +0x14. Use the conservative Xbox 360 720p60 NTSC mode also returned
// by the verified VdQueryVideoMode reference contract.
PPC_FUNC(__imp__XGetVideoMode) {
    const uint32_t output = ctx.r3.u32;
    RuntimeBeginGraphicsTrace(ctx);
    RuntimeTraceImport("XGetVideoMode", ctx);
    WriteVerifiedVideoMode(base, output, "XGetVideoMode");
    uint64_t ordinal = 0;
    if (ShouldTraceHotPath(videoModeTraceCount, ordinal)) {
        std::cout << "IMPORT_CALL name=__imp__XGetVideoMode thread="
                  << CurrentGuestThreadId() << " lr=0x" << std::hex << ctx.lr
                  << " output=0x" << output << std::dec
                  << " width=1280 height=720 refresh=60 interlaced=0 widescreen=1 hd=1"
                     " standard=1 bytes=48 ordinal="
                  << ordinal << '\n';
    }
}

// VdQueryVideoMode exposes the same X_VIDEO_MODE contract through xboxkrnl.
// Keep one writer for XAM and Vd so their guest-visible mode cannot diverge.
PPC_FUNC(__imp__VdQueryVideoMode) {
    RuntimeTraceImport("VdQueryVideoMode", ctx);
    const uint32_t output = ctx.r3.u32;
    WriteVerifiedVideoMode(base, output, "VdQueryVideoMode");
    std::cout << "IMPORT_CALL name=__imp__VdQueryVideoMode thread="
              << CurrentGuestThreadId() << " lr=0x" << std::hex << ctx.lr
              << " output=0x" << output << std::dec
              << " width=1280 height=720 refresh=60 bytes=48\n";
}

// The kernel flags are derived from the active mode: bit 0 is widescreen,
// bit 1 is at least 1024 pixels wide, and bit 2 is at least 1920 pixels wide.
// The shared verified 1280x720 mode therefore reports bits 0 and 1.
PPC_FUNC(__imp__VdQueryVideoFlags) {
    RuntimeTraceImport("VdQueryVideoFlags", ctx);
    constexpr uint32_t kVideoFlags = 0x3;
    ctx.r3.u64 = kVideoFlags;
    std::cout << "IMPORT_CALL name=__imp__VdQueryVideoFlags thread="
              << CurrentGuestThreadId() << " lr=0x" << std::hex << ctx.lr
              << " flags=0x" << kVideoFlags << std::dec
              << " widescreen=1 width_ge_1024=1 width_ge_1920=0\n";
}

// The Xbox display-gamma contract returns a type through r3 and a floating-
// point exponent through r4. The pinned ReXGlue implementation identifies the
// configured HDTV mode as BT.709 (type 2) and writes float(2.22222233). Both
// title call sites consume these outputs while constructing real gamma ramps.
PPC_FUNC(__imp__VdGetCurrentDisplayGamma) {
    RuntimeTraceImport("VdGetCurrentDisplayGamma", ctx);
    const uint32_t typeOutput = ctx.r3.u32;
    const uint32_t powerOutput = ctx.r4.u32;
    if (!typeOutput || typeOutput > UINT32_MAX - 4 ||
        !powerOutput || powerOutput > UINT32_MAX - 4)
        throw std::runtime_error("VdGetCurrentDisplayGamma reached an invalid output buffer");
    constexpr uint32_t kBt709GammaType = 2;
    constexpr uint32_t kBt709GammaPower = 0x400E38E4; // float(2.22222233)
    PPC_STORE_U32(typeOutput, kBt709GammaType);
    PPC_STORE_U32(powerOutput, kBt709GammaPower);
    std::cout << "IMPORT_CALL name=__imp__VdGetCurrentDisplayGamma thread="
              << CurrentGuestThreadId() << " lr=0x" << std::hex << ctx.lr
              << " type_output=0x" << typeOutput << " power_output=0x" << powerOutput
              << " type=0x" << kBt709GammaType << " power_bits=0x" << kBt709GammaPower
              << std::dec << " power=2.22222233\n";
}

// These exports calibrate the physical Xbox 360 EDRAM link. The runtime's
// D3D12 render-target/EDRAM model has no physical link to retrain. Both the
// pinned ReXGlue implementation and the title's checked call contract use a
// zero result for this host case. Report unavailable/failure explicitly and
// leave the caller's candidate output buffers untouched.
PPC_FUNC(__imp__VdRetrainEDRAMWorker) {
    RuntimeTraceImport("VdRetrainEDRAMWorker", ctx);
    const uint32_t request = ctx.r3.u32;
    ctx.r3.u64 = 0;
    std::cout << "IMPORT_CALL name=__imp__VdRetrainEDRAMWorker request=0x" << std::hex
              << request << std::dec << " physical_edram_link=0 result=0\n";
}

PPC_FUNC(__imp__VdRetrainEDRAM) {
    RuntimeTraceImport("VdRetrainEDRAM", ctx);
    const uint32_t device = ctx.r3.u32;
    const uint32_t output0 = ctx.r4.u32;
    const uint32_t bytes0 = ctx.r5.u32;
    const uint32_t output1 = ctx.r6.u32;
    const uint32_t output2 = ctx.r7.u32;
    const uint32_t bytes1 = ctx.r8.u32;
    ctx.r3.u64 = 0;
    uint64_t ordinal = 0;
    if (ShouldTraceHotPath(retrainEdramTraceCount, ordinal)) {
        std::cout << "IMPORT_CALL name=__imp__VdRetrainEDRAM device=0x" << std::hex
                  << device << " output0=0x" << output0 << " bytes0=0x" << bytes0
                  << " output1=0x" << output1 << " output2=0x" << output2
                  << " bytes1=0x" << bytes1 << std::dec
                  << " physical_edram_link=0 result=0 ordinal=" << ordinal << '\n';
    }
}

// The platform receives four big-endian 16-bit dimensions describing a
// front-buffer/back-buffer scaling transition. This title imports neither
// VdRegisterGraphicsNotification nor VdRegisterXamGraphicsNotification, so it
// has no title-registered routine to invoke. Retain the real notification
// state and return the reference kernel result without mutating the record.
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines) {
    RuntimeTraceImport("VdCallGraphicsNotificationRoutines", ctx);
    const uint32_t notificationClass = ctx.r3.u32;
    const uint32_t scaling = ctx.r4.u32;
    if (notificationClass != 1 || !scaling || scaling > UINT32_MAX - 8)
        throw std::runtime_error("VdCallGraphicsNotificationRoutines reached an invalid contract");
    const uint32_t frontWidth = PPC_LOAD_U16(scaling + 0);
    const uint32_t frontHeight = PPC_LOAD_U16(scaling + 2);
    const uint32_t backWidth = PPC_LOAD_U16(scaling + 4);
    const uint32_t backHeight = PPC_LOAD_U16(scaling + 6);
    lastFrontBufferWidth.store(frontWidth, std::memory_order_release);
    lastFrontBufferHeight.store(frontHeight, std::memory_order_release);
    lastBackBufferWidth.store(backWidth, std::memory_order_release);
    lastBackBufferHeight.store(backHeight, std::memory_order_release);
    const uint32_t sequence =
        graphicsNotificationSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    ctx.r3.u64 = 0;
    std::cout << "IMPORT_CALL name=__imp__VdCallGraphicsNotificationRoutines thread="
              << CurrentGuestThreadId() << " lr=0x" << std::hex << ctx.lr
              << " class=0x" << notificationClass << " record=0x" << scaling << std::dec
              << " front=" << frontWidth << 'x' << frontHeight
              << " back=" << backWidth << 'x' << backHeight
              << " registered_title_callbacks=0 result=0 sequence=" << sequence << '\n';
}

// Xbox KeQuerySystemTime follows the NT LARGE_INTEGER contract: r3 points to
// an eight-byte count of 100-nanosecond intervals since 1601-01-01 UTC.
PPC_FUNC(__imp__KeQuerySystemTime) {
    RuntimeTraceImport("KeQuerySystemTime", ctx);
    const uint32_t output = ctx.r3.u32;
    constexpr uint64_t kWindowsToUnixEpochTicks = 11644473600ULL * 10'000'000ULL;
    const uint64_t unixTicks = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() / 100);
    const uint64_t systemTime = kWindowsToUnixEpochTicks + unixTicks;
    if (output) PPC_STORE_U64(output, systemTime);
    static std::atomic<uint64_t> traceCount{};
    const uint64_t ordinal = traceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal <= 16 || (ordinal & 0xFFu) == 0) {
        std::cout << "IMPORT_CALL name=__imp__KeQuerySystemTime output=0x" << std::hex << output
                  << " value=0x" << systemTime << std::dec << " ordinal=" << ordinal << '\n';
    }
}

// NT TIME_FIELDS is eight signed 16-bit fields in this order: year, month,
// day, hour, minute, second, millisecond, weekday (Sunday == 0). The guest
// representation is big-endian, while the input is a 100-nanosecond FILETIME.
PPC_FUNC(__imp__RtlTimeToTimeFields) {
    RuntimeTraceImport("RtlTimeToTimeFields", ctx);
    const uint32_t input = ctx.r3.u32;
    const uint32_t output = ctx.r4.u32;
    if (!input || !output) return;

    constexpr uint64_t kTicksPerMillisecond = 10'000;
    constexpr uint64_t kMillisecondsPerDay = 86'400'000;
    constexpr int64_t kDays1601To1970 = 134'774;
    const uint64_t totalMilliseconds = PPC_LOAD_U64(input) / kTicksPerMillisecond;
    const int64_t daysSince1601 = static_cast<int64_t>(totalMilliseconds / kMillisecondsPerDay);
    uint64_t dayMilliseconds = totalMilliseconds % kMillisecondsPerDay;

    // Howard Hinnant's proleptic-Gregorian civil_from_days conversion, with
    // its input shifted from FILETIME's 1601 epoch to the Unix 1970 epoch.
    int64_t z = daysSince1601 - kDays1601To1970 + 719'468;
    const int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
    const uint32_t dayOfEra = static_cast<uint32_t>(z - era * 146'097);
    const uint32_t yearOfEra =
        (dayOfEra - dayOfEra / 1'460 + dayOfEra / 36'524 - dayOfEra / 146'096) / 365;
    int32_t year = static_cast<int32_t>(yearOfEra) + static_cast<int32_t>(era * 400);
    const uint32_t dayOfYear =
        dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const uint32_t monthPrime = (5 * dayOfYear + 2) / 153;
    const uint32_t day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
    const int32_t month = static_cast<int32_t>(monthPrime) + (monthPrime < 10 ? 3 : -9);
    year += month <= 2;

    const uint16_t hour = static_cast<uint16_t>(dayMilliseconds / 3'600'000);
    dayMilliseconds %= 3'600'000;
    const uint16_t minute = static_cast<uint16_t>(dayMilliseconds / 60'000);
    dayMilliseconds %= 60'000;
    const uint16_t second = static_cast<uint16_t>(dayMilliseconds / 1'000);
    const uint16_t millisecond = static_cast<uint16_t>(dayMilliseconds % 1'000);
    const uint16_t weekday = static_cast<uint16_t>((daysSince1601 + 1) % 7);

    PPC_STORE_U16(output + 0, static_cast<uint16_t>(year));
    PPC_STORE_U16(output + 2, static_cast<uint16_t>(month));
    PPC_STORE_U16(output + 4, static_cast<uint16_t>(day));
    PPC_STORE_U16(output + 6, hour);
    PPC_STORE_U16(output + 8, minute);
    PPC_STORE_U16(output + 10, second);
    PPC_STORE_U16(output + 12, millisecond);
    PPC_STORE_U16(output + 14, weekday);
    static std::atomic<uint64_t> traceCount{};
    const uint64_t ordinal = traceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal <= 16 || (ordinal & 0xFFu) == 0) {
        std::cout << "IMPORT_CALL name=__imp__RtlTimeToTimeFields input=0x" << std::hex << input
                  << " output=0x" << output << std::dec << " value=" << year << '-' << month << '-'
                  << day << 'T' << hour << ':' << minute << ':' << second << '.' << millisecond
                  << " weekday=" << weekday << " ordinal=" << ordinal << '\n';
    }
}

// Inverse of RtlTimeToTimeFields: validate the big-endian NT TIME_FIELDS
// record and return a 100-nanosecond FILETIME count since 1601-01-01 UTC.
// Weekday is descriptive only and is intentionally ignored by the kernel
// conversion contract. The output is left untouched when validation fails.
PPC_FUNC(__imp__RtlTimeFieldsToTime) {
    RuntimeTraceImport("RtlTimeFieldsToTime", ctx);
    const uint32_t input = ctx.r3.u32;
    const uint32_t output = ctx.r4.u32;
    if (!input || !output || input > UINT32_MAX - 16u || output > UINT32_MAX - 8u) {
        ctx.r3.u64 = 0;
        return;
    }

    const uint32_t year = PPC_LOAD_U16(input + 0);
    const uint32_t month = PPC_LOAD_U16(input + 2);
    const uint32_t day = PPC_LOAD_U16(input + 4);
    const uint32_t hour = PPC_LOAD_U16(input + 6);
    const uint32_t minute = PPC_LOAD_U16(input + 8);
    const uint32_t second = PPC_LOAD_U16(input + 10);
    const uint32_t millisecond = PPC_LOAD_U16(input + 12);
    const bool leap = (year % 4u == 0u) &&
                      ((year % 100u != 0u) || (year % 400u == 0u));
    constexpr uint8_t kMonthDays[12] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const uint32_t maximumDay = month >= 1u && month <= 12u
        ? uint32_t(kMonthDays[month - 1u]) + (month == 2u && leap ? 1u : 0u)
        : 0u;
    if (year < 1601u || !maximumDay || day < 1u || day > maximumDay ||
        hour > 23u || minute > 59u || second > 59u || millisecond > 999u) {
        ctx.r3.u64 = 0;
        return;
    }

    // Howard Hinnant's proleptic-Gregorian days_from_civil conversion.
    // Convert its Unix-epoch result to FILETIME's 1601 epoch and reject
    // values that cannot be represented by the unsigned 64-bit tick count.
    int64_t adjustedYear = static_cast<int64_t>(year) - (month <= 2u ? 1 : 0);
    const int64_t era = (adjustedYear >= 0 ? adjustedYear : adjustedYear - 399) / 400;
    const uint32_t yearOfEra =
        static_cast<uint32_t>(adjustedYear - era * 400);
    const uint32_t monthPrime = month > 2u ? month - 3u : month + 9u;
    const uint32_t dayOfYear = (153u * monthPrime + 2u) / 5u + day - 1u;
    const uint32_t dayOfEra =
        yearOfEra * 365u + yearOfEra / 4u - yearOfEra / 100u + dayOfYear;
    constexpr int64_t kDays1601To1970 = 134'774;
    const int64_t daysSince1970 = era * 146'097 + dayOfEra - 719'468;
    const int64_t daysSince1601 = daysSince1970 + kDays1601To1970;
    constexpr uint64_t kMillisecondsPerDay = 86'400'000;
    constexpr uint64_t kTicksPerMillisecond = 10'000;
    if (daysSince1601 < 0 ||
        static_cast<uint64_t>(daysSince1601) >
            (UINT64_MAX / kTicksPerMillisecond) / kMillisecondsPerDay) {
        ctx.r3.u64 = 0;
        return;
    }
    const uint64_t dayMilliseconds =
        uint64_t(hour) * 3'600'000u + uint64_t(minute) * 60'000u +
        uint64_t(second) * 1'000u + millisecond;
    const uint64_t totalMilliseconds =
        static_cast<uint64_t>(daysSince1601) * kMillisecondsPerDay + dayMilliseconds;
    if (totalMilliseconds > UINT64_MAX / kTicksPerMillisecond) {
        ctx.r3.u64 = 0;
        return;
    }
    const uint64_t result = totalMilliseconds * kTicksPerMillisecond;
    PPC_STORE_U64(output, result);
    ctx.r3.u64 = 1;

    static std::atomic<uint64_t> traceCount{};
    const uint64_t ordinal = traceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal <= 16 || (ordinal & 0xFFu) == 0) {
        std::cout << "IMPORT_CALL name=__imp__RtlTimeFieldsToTime input=0x" << std::hex
                  << input << " output=0x" << output << " result=0x" << result << std::dec
                  << " value=" << year << '-' << month << '-' << day << 'T' << hour << ':'
                  << minute << ':' << second << '.' << millisecond
                  << " ordinal=" << ordinal << '\n';
    }
}

// The result layout and the two validation statuses are established by the
// public Xbox 360 API reference used in docs/MM_QUERY_STATISTICS_USAGE.md.
// Values below are deliberately restricted to state this runtime owns: the
// 512 MiB physical backing and allocations reserved through its accounting
// layer. Kernel/title virtual-pool breakdowns are not known and remain zero.
PPC_FUNC(__imp__MmQueryStatistics) {
    const uint32_t address = ctx.r3.u32;
    if (!address) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    if (PPC_LOAD_U32(address) != kMmQueryStatisticsSize) {
        ctx.r3.u64 = kStatusBufferTooSmall;
        return;
    }

    RuntimeGeneratedMemset(base, base + address, 0, kMmQueryStatisticsSize, __FILE__, __LINE__);
    const auto& memory = GetGuestMemoryAccounting();
    PPC_STORE_U32(address + 0, kMmQueryStatisticsSize);
    PPC_STORE_U32(address + 4, memory.total_physical_pages());
    PPC_STORE_U32(address + 12, memory.available_physical_pages());
    PPC_STORE_U32(address + 100, memory.highest_physical_page());
    ctx.r3.u64 = kStatusSuccess;
    std::cout << "IMPORT_CALL name=__imp__MmQueryStatistics total_pages="
              << memory.total_physical_pages() << " available_pages="
              << memory.available_physical_pages() << '\n';
}

// The allocation ABI (flags, byte size, protection, physical bounds and
// alignment) is verified by the game's wrapper at 0x828AA018. The returned
// address is an alias of the runtime's mapped 512 MiB physical backing.
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx) {
    const uint32_t size = ctx.r4.u32;
    const uint32_t protect = ctx.r5.u32;
    const uint32_t minimumAddress = ctx.r6.u32;
    const uint32_t maximumAddress = ctx.r7.u32;
    const uint32_t requestedAlignment = ctx.r8.u32;
    if (!(protect & (kPageReadOnly | kPageReadWrite)) || !size) {
        ctx.r3.u64 = 0;
        return;
    }

    uint32_t pageBytes = GuestMemoryAccounting::kPageSize;
    if (protect & kMemLargePages) pageBytes = 64 * 1024;
    if (protect & kMem16MbPages) pageBytes = 16 * 1024 * 1024;
    const uint32_t roundedSize = RoundUp(size, pageBytes);
    const uint32_t roundedAlignment = RoundUp(requestedAlignment, pageBytes);
    if (!roundedSize) {
        ctx.r3.u64 = 0;
        return;
    }

    const uint32_t pageCount = roundedSize / GuestMemoryAccounting::kPageSize;
    const uint32_t alignmentPages = std::max<uint32_t>(
        1, (roundedAlignment ? roundedAlignment : pageBytes) / GuestMemoryAccounting::kPageSize);
    const uint32_t minPage = minimumAddress / GuestMemoryAccounting::kPageSize;
    const uint32_t maxPage = maximumAddress >= GuestMemoryAccounting::kPhysicalBytes
        ? GuestMemoryAccounting::kPhysicalPages - 1
        : maximumAddress / GuestMemoryAccounting::kPageSize;

    uint32_t firstPage{};
    auto& memory = GetGuestMemoryAccounting();
    const uint32_t allocationProtection = protect & kPageProtectionMask;
    if (!memory.ReservePhysicalPages(pageCount, alignmentPages, minPage, maxPage,
                                     allocationProtection, &firstPage)) {
        ctx.r3.u64 = 0;
        return;
    }
    const uint32_t guestAddress = kPhysicalGuestBase + firstPage * GuestMemoryAccounting::kPageSize;
    ctx.r3.u64 = guestAddress;
    std::cout << "IMPORT_CALL name=__imp__MmAllocatePhysicalMemoryEx result=0x" << std::hex
              << guestAddress << " bytes=0x" << roundedSize << " protect=0x"
              << allocationProtection << " available_pages=0x"
              << memory.available_physical_pages() << std::dec << '\n';
}

PPC_FUNC(__imp__MmFreePhysicalMemory) {
    const uint32_t guestAddress = ctx.r4.u32;
    if (guestAddress < kPhysicalGuestBase ||
        (guestAddress - kPhysicalGuestBase) % GuestMemoryAccounting::kPageSize) {
        return;
    }
    const uint32_t firstPage = (guestAddress - kPhysicalGuestBase) / GuestMemoryAccounting::kPageSize;
    const bool released = GetGuestMemoryAccounting().ReleasePhysicalPages(firstPage);
    std::cout << "IMPORT_CALL name=__imp__MmFreePhysicalMemory address=0x" << std::hex
              << guestAddress << " released=" << released << std::dec << '\n';
}

// The live caller at 0x821FC928 passes the base address returned by
// MmAllocatePhysicalMemoryEx. Xenia's implementation returns the allocation
// size from its owning heap and returns zero when no allocation is found. The
// runtime only owns tracked physical allocations, so it applies that contract
// only to the documented 0xA0000000-0xBFFFFFFF physical alias.
PPC_FUNC(__imp__MmQueryAllocationSize) {
    const uint32_t guestAddress = ctx.r3.u32;
    uint32_t size{};
    if (guestAddress >= kPhysicalGuestBase && guestAddress < 0xC0000000 &&
        (guestAddress - kPhysicalGuestBase) % GuestMemoryAccounting::kPageSize == 0) {
        const uint32_t firstPage = (guestAddress - kPhysicalGuestBase) / GuestMemoryAccounting::kPageSize;
        size = GetGuestMemoryAccounting().AllocationSizeAtBasePage(firstPage);
    }
    ctx.r3.u64 = size;
    std::cout << "IMPORT_CALL name=__imp__MmQueryAllocationSize address=0x" << std::hex
              << guestAddress << " size=0x" << size << std::dec << '\n';
}

// The live 0x82250A84 caller queries an interior address of the physical
// allocation at 0xBF300000. ReXGlue resolves the owning heap allocation and
// returns its XDK protection flags, or zero when the address is untracked.
// Runtime physical aliases all share the same backing store, so use the
// allocation ledger rather than inferring permissions from the host mapping.
PPC_FUNC(__imp__MmQueryAddressProtect) {
    const uint32_t guestAddress = ctx.r3.u32;
    uint32_t protection{};
    if (guestAddress >= kPhysicalGuestBase && guestAddress < 0xC0000000) {
        const uint32_t page =
            (guestAddress - kPhysicalGuestBase) / GuestMemoryAccounting::kPageSize;
        protection = GetGuestMemoryAccounting().AllocationProtectionAtPage(page);
    }
    ctx.r3.u64 = protection;
    if (kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__MmQueryAddressProtect address=0x" << std::hex
                  << guestAddress << " protect=0x" << protection << std::dec << '\n';
    }
}

// Xbox physical aliases map the low 512 MiB into A/C segments (and the
// E-segment 4 KiB-offset view). The Darkness calls this for its real command
// ring allocation before VdInitializeRingBuffer.
PPC_FUNC(__imp__MmGetPhysicalAddress) {
    const uint32_t address = ctx.r3.u32;
    uint32_t physical = UINT32_MAX;
    if (address >= 0x7F000000u && address < 0x80000000u) {
        physical = address - 0x7F000000u;
    } else if (address >= 0xA0000000u && address < 0xE0000000u) {
        physical = address & 0x1FFFFFFFu;
    } else if (address >= 0xE0000000u && address < 0xFFD00000u) {
        physical = address - 0xE0000000u + 0x1000u;
    }
    ctx.r3.u64 = physical;
    if (physical == UINT32_MAX || kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__MmGetPhysicalAddress virtual=0x" << std::hex
                  << address << " physical=0x" << physical << std::dec << '\n';
    }
}

// This import is the first real graphics boundary. Success is returned only
// after the pinned ReXGlue D3D12 provider and Xenos command processor have
// adopted the runtime's existing guest mappings and started their workers.
PPC_FUNC(__imp__VdInitializeEngines) {
    RuntimeTraceImport("VdInitializeEngines", ctx);
    const uint32_t flags = ctx.r3.u32;
    const uint32_t engineCallback = ctx.r4.u32;
    const uint32_t callbackArgument = ctx.r5.u32;
    const uint32_t pfpMicrocode = ctx.r6.u32;
    const uint32_t meMicrocode = ctx.r7.u32;
    const uint32_t r8 = ctx.r8.u32;
    const uint32_t r9 = ctx.r9.u32;
    if (engineCallback != 0x8286FD20u || callbackArgument != 0 ||
        pfpMicrocode != 0x820556F0u || meMicrocode != 0x82055B70u ||
        r8 != 0x20u || r9 != 1u) {
        throw std::runtime_error("VdInitializeEngines reached an unverified title contract");
    }
    if (!RuntimeGeneratedAddressInRange(engineCallback) ||
        !PPC_LOOKUP_FUNC(base, engineCallback))
        throw std::runtime_error("VdInitializeEngines callback is not generated");
    if (!InitializeRuntimeGraphics(base)) {
        ctx.r3.u64 = 0;
        throw std::runtime_error("VdInitializeEngines could not initialize the real GPU path");
    }
    if (!RuntimeAudioInitializeXmaHardware(base)) {
        ctx.r3.u64 = 0;
        throw std::runtime_error("VdInitializeEngines could not initialize the real XMA path");
    }
    // ReXGlue interprets PM4 directly and therefore does not execute the PFP/ME
    // firmware blobs. Preserve and report the title-provided pointers; no
    // synthetic firmware is substituted and no guest bytes are copied.
    ctx.r3.u64 = 1;
    std::cout << "IMPORT_CALL name=__imp__VdInitializeEngines flags=0x" << std::hex
              << flags << " callback=0x" << engineCallback << " callback_argument=0x"
              << callbackArgument << " pfp=0x" << pfpMicrocode << " me=0x" << meMicrocode
              << " r8=0x" << r8 << " r9=0x" << r9 << std::dec
              << " result=1 backend=d3d12 firmware_model=hle_pm4\n";
}

// X_EX_TITLE_TERMINATE_REGISTRATION is a big-endian 16-byte record containing
// {notification routine, priority, LIST_ENTRY}. The kernel retains the first
// two fields on registration and removes the first matching routine when the
// create flag is false. The list links remain guest-owned and are not rewritten.
PPC_FUNC(__imp__ExRegisterTitleTerminateNotification) {
    RuntimeTraceImport("ExRegisterTitleTerminateNotification", ctx);
    const uint32_t registration = ctx.r3.u32;
    const bool create = ctx.r4.u32 != 0;
    if (!registration)
        throw std::runtime_error("ExRegisterTitleTerminateNotification reached a null registration");
    const uint32_t routine = PPC_LOAD_U32(registration);
    const uint32_t priority = PPC_LOAD_U32(registration + 4);
    if (create) {
        GetRuntimeTitleTerminateNotifications().Register(routine, priority);
    } else {
        GetRuntimeTitleTerminateNotifications().Remove(routine);
    }
    std::cout << "IMPORT_CALL name=__imp__ExRegisterTitleTerminateNotification registration=0x"
              << std::hex << registration << " routine=0x" << routine << " priority=0x"
              << priority << " create=" << std::dec << create << " generated="
              << (RuntimeGeneratedAddressInRange(routine) &&
                      PPC_LOOKUP_FUNC(base, routine) ? 1 : 0)
              << '\n';
}

PPC_FUNC(__imp__VdSetGraphicsInterruptCallback) {
    RuntimeTraceImport("VdSetGraphicsInterruptCallback", ctx);
    RuntimeGraphicsSetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
}

PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) {
    RuntimeTraceImport("VdSetSystemCommandBufferGpuIdentifierAddress", ctx);
    RuntimeGraphicsSetSystemCommandBufferGpuIdentifierAddress(ctx.r3.u32);
}

PPC_FUNC(__imp__VdGetSystemCommandBuffer) {
    RuntimeTraceImport("VdGetSystemCommandBuffer", ctx);
    const uint32_t commandState = ctx.r3.u32;
    const uint32_t commandToken = ctx.r4.u32;
    if (!commandState || commandState > UINT32_MAX - 0x94u ||
        !commandToken || commandToken > UINT32_MAX - 4u) {
        throw std::runtime_error("VdGetSystemCommandBuffer reached invalid output storage");
    }
    RuntimeGeneratedMemset(base, base + commandState, 0, 0x94, __FILE__, __LINE__);
    PPC_STORE_U32(commandState, 0xBEEF0000u);
    PPC_STORE_U32(commandToken, 0xBEEF0001u);
    uint64_t ordinal = 0;
    if (ShouldTraceHotPath(systemCommandBufferTraceCount, ordinal)) {
        std::cout << "IMPORT_CALL name=__imp__VdGetSystemCommandBuffer state=0x"
                  << std::hex << commandState << " token_address=0x" << commandToken
                  << " state_tag=0xbeef0000 token=0xbeef0001 bytes=0x94" << std::dec
                  << " ordinal=" << ordinal << '\n';
    }
}

PPC_FUNC(__imp__VdSwap) {
    RuntimeTraceImport("VdSwap", ctx);
    RuntimeBeginPostSwapTrace(ctx);
    RuntimeFatalNoteSwap();
    // Developer check of the freeze report (V383): with
    // DARKNESS_TEST_STALL_AFTER_SECONDS=N the title's frame loop stops for
    // 25 s once, N seconds after the first frame. Unset in every player launch.
    static const double testStallAfterSeconds = [] {
        const char* value = std::getenv("DARKNESS_TEST_STALL_AFTER_SECONDS");
        return value ? std::atof(value) : 0.0;
    }();
    if (testStallAfterSeconds > 0.0) {
        static const auto firstSwap = std::chrono::steady_clock::now();
        static std::atomic<bool> stalled{};
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - firstSwap).count() >
                testStallAfterSeconds &&
            !stalled.exchange(true)) {
            std::cerr << "RUNTIME_TEST_STALL seconds=25\n";
            std::this_thread::sleep_for(std::chrono::seconds(25));
        }
    }
    // V380: the player closed the window (the plugin vetoes the close so the
    // title never runs on without one): stop the title cleanly. A hung title
    // never gets here; the plugin's watchdog ends the process then.
    if (RuntimeGraphicsCloseRequested()) {
        static std::atomic<bool> stopping{};
        if (!stopping.exchange(true)) {
            std::cerr << "RUNTIME_WINDOW_CLOSE stop=1\n";
            RequestGuestRuntimeStop();
        }
    }
    const uint32_t commandBuffer = ctx.r3.u32;
    const uint32_t textureFetch = ctx.r4.u32;
    const uint32_t systemWriteback = ctx.r5.u32;
    const uint32_t systemBuffer = ctx.r6.u32;
    const uint32_t systemToken = ctx.r7.u32;
    const uint32_t frontbufferAddress = ctx.r8.u32;
    const uint32_t textureFormat = ctx.r9.u32;
    const uint32_t colorSpace = ctx.r10.u32;
    const uint32_t width = PPC_LOAD_U32(ctx.r1.u32 + 0x54);
    const uint32_t height = PPC_LOAD_U32(ctx.r1.u32 + 0x5C);
    const RuntimeGraphicsSwapCommandInfo info = RuntimeGraphicsBuildSwapCommand(
        base, commandBuffer, textureFetch, systemBuffer, systemToken,
        frontbufferAddress, textureFormat, colorSpace, width, height);
    RuntimeGraphicsGuestSwapQueued();
    uint64_t ordinal = 0;
    if (ShouldTraceHotPath(vdSwapTraceCount, ordinal)) {
        std::cout << "IMPORT_CALL name=__imp__VdSwap command_buffer=0x" << std::hex
                  << commandBuffer << " texture_fetch=0x" << textureFetch
                  << " system_writeback=0x" << systemWriteback << " system_buffer=0x"
                  << systemBuffer << " system_token=0x" << systemToken
                  << " frontbuffer_virtual=0x" << info.frontbufferVirtual
                  << " frontbuffer_physical=0x" << info.frontbufferPhysical
                  << " format=0x" << info.textureFormat << std::dec << " width="
                  << info.width << " height=" << info.height << " ordinal=" << ordinal
                  << '\n';
    }
}

// The title constructs this bitfield directly from its selected render mode.
// The pinned kernel contract records the low-resolution, texture-format, and
// color-space flags but performs no host mode switch and returns zero. The
// D3D12 host mode remains the same verified 1280x720 mode exposed by the query
// imports; accepting this call therefore does not fabricate display progress.
PPC_FUNC(__imp__VdSetDisplayMode) {
    RuntimeTraceImport("VdSetDisplayMode", ctx);
    const uint32_t flags = ctx.r3.u32;
    constexpr uint32_t kKnownFlags = 0x78000003u;
    if (flags & ~kKnownFlags) {
        std::ostringstream error;
        error << "VdSetDisplayMode reached unknown flags 0x" << std::hex << flags;
        throw std::runtime_error(error.str());
    }
    ctx.r3.u64 = 0;
    std::cout << "IMPORT_CALL name=__imp__VdSetDisplayMode flags=0x" << std::hex
              << flags << " low_resolution=" << ((flags & 2u) ? 1 : 0)
              << " wide_format=" << ((flags & 0x08000000u) ? 1 : 0)
              << " color_space=" << ((flags >> 28) & 3u) << std::dec
              << " result=0 host_mode=1280x720\n";
}

// X_DISPLAY_INFO is an 88-byte big-endian kernel structure. Keep it derived
// from the same verified video-mode writer used by XGetVideoMode and
// VdQueryVideoMode so the title cannot observe contradictory display state.
PPC_FUNC(__imp__VdGetCurrentDisplayInformation) {
    RuntimeTraceImport("VdGetCurrentDisplayInformation", ctx);
    const uint32_t output = ctx.r3.u32;
    WriteVerifiedDisplayInfo(base, output, "VdGetCurrentDisplayInformation");
    std::cout << "IMPORT_CALL name=__imp__VdGetCurrentDisplayInformation output=0x"
              << std::hex << output << std::dec
              << " width=1280 height=720 actual_width=1280 refresh=60 bytes=88\n";
}

// RtlFillMemoryUlong writes a big-endian 32-bit pattern to every complete
// ULONG in the destination range. The Xbox contract requires both destination
// and length to be ULONG-aligned; reject a violated precondition instead of
// silently truncating a future caller. Use PPC_STORE so physical-memory writes
// continue to participate in the runtime's shared CPU/GPU invalidation path.
PPC_FUNC(__imp__RtlFillMemoryUlong) {
    RuntimeTraceImport("RtlFillMemoryUlong", ctx);
    const uint32_t destination = ctx.r3.u32;
    const uint32_t length = ctx.r4.u32;
    const uint32_t pattern = ctx.r5.u32;
    if ((destination & 3u) || (length & 3u) ||
        (length && (!destination || destination > UINT32_MAX - length))) {
        throw std::runtime_error("RtlFillMemoryUlong reached an invalid ULONG range");
    }
    for (uint32_t offset = 0; offset < length; offset += sizeof(uint32_t)) {
        PPC_STORE_U32(destination + offset, pattern);
    }
    std::cout << "IMPORT_CALL name=__imp__RtlFillMemoryUlong destination=0x"
              << std::hex << destination << " bytes=0x" << length << " pattern=0x"
              << pattern << std::dec << " words=" << (length / sizeof(uint32_t)) << '\n';
}

// The dynamically reached scaler request is identity scaling: source,
// output, and front-buffer extents are identical and both filter selectors
// are 7. Public Free60 Xenos register programming establishes the required
// identity path: bracket the update, disable the scaler, and program viewport
// start/size. Emit those as real PM4 type-0 register writes. Do not inherit
// the public Xenia placeholder that returns 200 type-2 NOPs.
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer) {
    RuntimeTraceImport("VdInitializeScalerCommandBuffer", ctx);
    const uint32_t scalerSourceXy = ctx.r3.u32;
    const uint32_t scalerSourceWh = ctx.r4.u32;
    const uint32_t scaledOutputXy = ctx.r5.u32;
    const uint32_t scaledOutputWh = ctx.r6.u32;
    const uint32_t frontBufferWh = ctx.r7.u32;
    const uint32_t verticalFilterType = ctx.r8.u32;
    const uint32_t verticalFilterParams = ctx.r9.u32;
    const uint32_t horizontalFilterType = ctx.r10.u32;
    const uint32_t horizontalFilterParams = PPC_LOAD_U32(ctx.r1.u32 + 0x54);
    const uint32_t auxiliary = PPC_LOAD_U32(ctx.r1.u32 + 0x5C);
    const uint32_t destination = PPC_LOAD_U32(ctx.r1.u32 + 0x64);
    const uint32_t destinationCapacity = PPC_LOAD_U32(ctx.r1.u32 + 0x6C);
    constexpr uint32_t kCommandDwords = 9;
    constexpr uint32_t kD1ModeViewportStart = 0x1960;
    constexpr uint32_t kD1ModeViewportSize = 0x1961;
    constexpr uint32_t kD1SclScalerEnable = 0x1964;
    constexpr uint32_t kD1SclUpdate = 0x1973;
    if (scalerSourceXy != scaledOutputXy || scalerSourceWh != scaledOutputWh ||
        scalerSourceWh != frontBufferWh || !scalerSourceWh ||
        verticalFilterType != 7 || horizontalFilterType != 7) {
        throw std::runtime_error(
            "VdInitializeScalerCommandBuffer reached an unverified non-identity scaler mode");
    }
    if (!verticalFilterParams || verticalFilterParams > UINT32_MAX - 12u ||
        !horizontalFilterParams || horizontalFilterParams > UINT32_MAX - 12u ||
        !destination || (destination & 3u) || destinationCapacity < kCommandDwords ||
        destination > UINT32_MAX - kCommandDwords * sizeof(uint32_t)) {
        throw std::runtime_error(
            "VdInitializeScalerCommandBuffer reached an invalid guest buffer contract");
    }

    uint32_t offset{};
    const auto writeType0 = [&](uint32_t firstRegister,
                                std::initializer_list<uint32_t> values) {
        PPC_STORE_U32(destination + offset++ * 4,
                      ((uint32_t(values.size()) - 1u) << 16) | firstRegister);
        for (uint32_t value : values) {
            PPC_STORE_U32(destination + offset++ * 4, value);
        }
    };
    writeType0(kD1SclUpdate, {1});
    writeType0(kD1SclScalerEnable, {0});
    writeType0(kD1ModeViewportStart, {scaledOutputXy, scaledOutputWh});
    writeType0(kD1SclUpdate, {0});
    if (offset != kCommandDwords) {
        throw std::runtime_error("VdInitializeScalerCommandBuffer internal word-count mismatch");
    }
    ctx.r3.u64 = kCommandDwords;
    std::cout << "IMPORT_CALL name=__imp__VdInitializeScalerCommandBuffer source_xy=0x"
              << std::hex << scalerSourceXy << " source_wh=0x" << scalerSourceWh
              << " output_xy=0x" << scaledOutputXy << " output_wh=0x" << scaledOutputWh
              << " frontbuffer_wh=0x" << frontBufferWh << " vertical_filter="
              << verticalFilterType << " vertical_params=0x" << verticalFilterParams
              << " horizontal_filter=" << horizontalFilterType << " horizontal_params=0x"
              << horizontalFilterParams << " auxiliary=0x" << auxiliary
              << " destination=0x" << destination << std::dec
              << " capacity_dwords=" << destinationCapacity
              << " emitted_dwords=" << kCommandDwords << " scaler=identity_pm4\n";
}

// The verified kernel contract reserves a 64-byte, 32-byte-aligned physical
// persistence token and writes its address through the second argument. The
// title immediately passes that address to MmFreePhysicalMemory when the call
// succeeds, so allocate it from the same authoritative physical-page ledger
// instead of returning a fabricated token.
PPC_FUNC(__imp__VdPersistDisplay) {
    RuntimeTraceImport("VdPersistDisplay", ctx);
    const uint32_t displayState = ctx.r3.u32;
    const uint32_t allocationOutput = ctx.r4.u32;
    if (!displayState || displayState > UINT32_MAX - 12u ||
        !allocationOutput || allocationOutput > UINT32_MAX - 4u) {
        ctx.r3.u64 = 0;
        std::cout << "IMPORT_CALL name=__imp__VdPersistDisplay state=0x" << std::hex
                  << displayState << " output=0x" << allocationOutput << std::dec
                  << " result=0 reason=invalid_contract\n";
        return;
    }
    uint32_t firstPage{};
    auto& memory = GetGuestMemoryAccounting();
    if (!memory.ReservePhysicalPages(1, 1, 0,
                                     GuestMemoryAccounting::kPhysicalPages - 1,
                                     1, &firstPage)) {
        ctx.r3.u64 = 0;
        std::cout << "IMPORT_CALL name=__imp__VdPersistDisplay state=0x" << std::hex
                  << displayState << " output=0x" << allocationOutput << std::dec
                  << " result=0 reason=physical_memory_exhausted\n";
        return;
    }
    const uint32_t allocation =
        kPhysicalGuestBase + firstPage * GuestMemoryAccounting::kPageSize;
    PPC_STORE_U32(allocationOutput, allocation);
    ctx.r3.u64 = 1;
    std::cout << "IMPORT_CALL name=__imp__VdPersistDisplay state=0x" << std::hex
              << displayState << " output=0x" << allocationOutput
              << " allocation=0x" << allocation << std::dec
              << " requested_bytes=64 alignment=32 reserved_bytes="
              << GuestMemoryAccounting::kPageSize << " result=1\n";
}

// Physical Xenos clock gating has no host-D3D12 register equivalent, but the
// policy transition is still guest-visible state. Retain the exact requested
// state in the graphics adapter and leave host device power management to the
// Windows display driver, matching the pinned kernel return contract.
PPC_FUNC(__imp__VdEnableDisableClockGating) {
    RuntimeTraceImport("VdEnableDisableClockGating", ctx);
    const uint32_t enabled = ctx.r3.u32;
    if (enabled > 1) {
        throw std::runtime_error("VdEnableDisableClockGating reached an invalid boolean");
    }
    RuntimeGraphicsSetClockGating(enabled != 0);
    ctx.r3.u64 = 0;
    std::cout << "IMPORT_CALL name=__imp__VdEnableDisableClockGating enabled="
              << enabled << " result=0\n";
}

// Despite its APC-derived name, this export is a synchronous two-argument
// helper in the reached video initialization path. The pinned ReXGlue/Xenia
// kernel implementation defines its complete contract as returning zero and
// performing no guest-memory mutation. The title immediately replaces r3
// after the call, but preserve the defined return value for other callers.
PPC_FUNC(__imp__KiApcNormalRoutineNop) {
    RuntimeTraceImport("KiApcNormalRoutineNop", ctx);
    const uint32_t output = ctx.r3.u32;
    const uint32_t selector = ctx.r4.u32;
    ctx.r3.u64 = 0;
    std::cout << "IMPORT_CALL name=__imp__KiApcNormalRoutineNop output=0x" << std::hex
              << output << " selector=0x" << selector << std::dec << " result=0\n";
}

PPC_FUNC(__imp__VdShutdownEngines) {
    RuntimeTraceImport("VdShutdownEngines", ctx);
    ShutdownRuntimeGraphics();
    std::cout << "IMPORT_CALL name=__imp__VdShutdownEngines result=stopped\n";
}

PPC_FUNC(__imp__VdInitializeRingBuffer) {
    RuntimeTraceImport("VdInitializeRingBuffer", ctx);
    RuntimeGraphicsInitializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
}

PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) {
    RuntimeTraceImport("VdEnableRingBufferRPtrWriteBack", ctx);
    RuntimeGraphicsEnableReadPointerWriteback(ctx.r3.u32, ctx.r4.u32);
}

PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded) {
    RuntimeTraceImport("VdIsHSIOTrainingSucceeded", ctx);
    ctx.r3.u64 = 1;
    std::cout << "IMPORT_CALL name=__imp__VdIsHSIOTrainingSucceeded result=1\n";
}

// The live engine-start caller at 0x827A5B78 supplies a 1020-byte buffer and
// continues normally when launch data is absent. No launch payload was given
// to this runtime, so report the documented XAM "not found" result without
// modifying guest memory. This is the same absent-payload contract used by
// Xenia's XamLoaderGetLaunchData implementation.
PPC_FUNC(__imp__XamLoaderGetLaunchData) {
    const uint32_t buffer = ctx.r3.u32;
    const uint32_t bufferSize = ctx.r4.u32;
    ctx.r3.u64 = kXErrorNotFound;
    std::cout << "IMPORT_CALL name=__imp__XamLoaderGetLaunchData buffer=0x" << std::hex
              << buffer << " bytes=0x" << bufferSize << " status=0x" << kXErrorNotFound << std::dec << '\n';
}

// The Xbox/XAM dirty-disc UI is a terminal error path, not a recoverable
// notification. The pinned ReXGlue implementation and current Xenia kernel
// model both terminate after reporting it. Preserve that contract explicitly;
// returning success here would let the title continue after failed content I/O
// with corrupted state.
PPC_FUNC(__imp__XamShowDirtyDiscErrorUI) {
    RuntimeTraceImport("XamShowDirtyDiscErrorUI", ctx);
    const uint32_t savedCaller = ctx.r1.u32 <= UINT32_MAX - 88u
        ? PPC_LOAD_U32(ctx.r1.u32 + 88u)
        : 0;
    std::cerr << "FATAL_IMPORT name=__imp__XamShowDirtyDiscErrorUI user=" << ctx.r3.u32
              << " lr=0x" << std::hex << ctx.lr << " caller=0x" << savedCaller
              << " r1=0x" << ctx.r1.u32 << std::dec
              << " thread=" << CurrentGuestThreadId()
              << " reason=guest_reported_disc_or_file_read_error\n";
    RuntimeWriteThreadSnapshot("fatal-dirty-disc-content-read");
    RuntimeFatalRecordDetail("XamShowDirtyDiscErrorUI lr=0x" + [&] {
        std::ostringstream text;
        text << std::hex << ctx.lr << " caller=0x" << savedCaller;
        return text.str();
    }());
    RuntimeFatalReport("the game reported a disc or file read error it cannot recover from");
    std::cout.flush();
    std::cerr.flush();
    RuntimeFatalShowDialog("the game reported a disc or file read error it cannot recover from");
    std::exit(EXIT_FAILURE);
}

// The first live use at 0x828A9CAC configures FSC selector 0 with 32
// elements. Public Xenia source records the two arguments as an unknown
// cache selector/count pair and returns STATUS_SUCCESS. Preserve the reached
// configuration for a future query path; no host cache is claimed here.
PPC_FUNC(__imp__FscSetCacheElementCount) {
    const uint32_t cache = ctx.r3.u32;
    const uint32_t count = ctx.r4.u32;
    GetGuestFileCacheConfiguration().SetElementCount(cache, count);
    ctx.r3.u64 = kStatusSuccess;
    std::cout << "IMPORT_CALL name=__imp__FscSetCacheElementCount cache=" << cache
              << " count=" << count << " status=0x0\n";
}

// Existing title files remain read-only. Explicit XAM portable-content mounts
// additionally implement the six NT create dispositions and may create or
// replace files beneath their isolated runtime_data package directory.
PPC_FUNC(__imp__NtCreateFile) {
    const uint32_t handleOut = ctx.r3.u32;
    const uint32_t desiredAccess = ctx.r4.u32;
    const uint32_t objectAttributes = ctx.r5.u32;
    const uint32_t ioStatus = ctx.r6.u32;
    const uint32_t root = objectAttributes ? PPC_LOAD_U32(objectAttributes) : 0;
    const uint32_t ansiString = objectAttributes ? PPC_LOAD_U32(objectAttributes + 4) : 0;
    const uint32_t attributes = objectAttributes ? PPC_LOAD_U32(objectAttributes + 8) : 0;
    // NtCreateFile's ninth integer argument is in the caller parameter area.
    // Keep it observable so the synchronous/asynchronous contract of the live
    // XDF handles can be classified from guest evidence before it is modeled.
    const uint32_t createOptions = PPC_LOAD_U32(ctx.r1.u32 + 0x54u);
    const uint32_t wrapperCaller = PPC_LOAD_U32(ctx.r1.u32 + 0xB8u);
    const uint16_t nameLength = ansiString ? PPC_LOAD_U16(ansiString) : 0;
    const uint32_t nameAddress = ansiString ? PPC_LOAD_U32(ansiString + 4) : 0;
    std::string path;
    if (nameAddress && nameLength <= 0x1000) {
        path.assign(reinterpret_cast<const char*>(base + nameAddress), nameLength);
    }
    const bool synchronous =
        (createOptions & (kFileSynchronousIoAlert | kFileSynchronousIoNonAlert)) != 0;
    constexpr uint32_t kGenericWrite = 0x40000000u;
    constexpr uint32_t kFileWriteData = 0x00000002u;
    constexpr uint32_t kFileAppendData = 0x00000004u;
    constexpr uint32_t kDelete = 0x00010000u;
    const bool writeRequested =
        (desiredAccess & (kGenericWrite | kFileWriteData | kFileAppendData)) != 0;
    const bool deleteRequested = (desiredAccess & kDelete) != 0;
    GuestFileOpenResult open{};
    if (GetGuestFileSystem().OpenRawCachePartition(path.data(), nameLength)) {
        open.status = GuestFileOpenStatus::Success;
    } else {
        open = GetGuestFileSystem().CreateOrOpenGameFile(
            path, synchronous, ctx.r10.u32, writeRequested, deleteRequested);
    }
    uint32_t status{};
    switch (open.status) {
    case GuestFileOpenStatus::Success: status = kStatusSuccess; break;
    case GuestFileOpenStatus::InvalidParameter: status = kStatusInvalidParameter; break;
    case GuestFileOpenStatus::NoSuchFile: status = kStatusObjectNameNotFound; break;
    case GuestFileOpenStatus::NameCollision: status = kStatusObjectNameCollision; break;
    case GuestFileOpenStatus::AccessDenied: status = kStatusAccessDenied; break;
    }
    const uint32_t handle = open.handle;
    if (handleOut) PPC_STORE_U32(handleOut, handle);
    if (ioStatus) {
        PPC_STORE_U32(ioStatus, status);
        PPC_STORE_U32(ioStatus + 4, status == kStatusSuccess ? open.information : 0);
    }
    ctx.r3.u64 = status;
    std::cout << "IMPORT_CALL name=__imp__NtCreateFile lr=0x" << std::hex << ctx.lr
              << " caller=0x" << wrapperCaller
              << " handle=0x" << handle << " access=0x" << desiredAccess
              << " root=0x" << root << " attrs=0x" << attributes
              << " share=0x" << ctx.r9.u32 << " disposition=0x" << ctx.r10.u32
              << " options=0x" << createOptions << " synchronous=" << std::dec << synchronous
              << std::hex
              << " path=" << path << " status=0x" << status << std::dec << '\n';
    static std::atomic<bool> capturedGuiPrecacheOpen{};
    static std::atomic<bool> capturedTextFontMiss{};
    if (handle && path == "d:\\content\\xdf\\guiprecache.xdf" &&
        RuntimeMilestoneSnapshotsEnabled() &&
        !capturedGuiPrecacheOpen.exchange(true, std::memory_order_relaxed)) {
        RuntimeWriteCurrentThreadSnapshot("guiprecache-xdf-open");
    }
    if (!handle && path == "d:\\content\\fonts\\text.xfc" &&
        RuntimeMilestoneSnapshotsEnabled() &&
        !capturedTextFontMiss.exchange(true, std::memory_order_relaxed)) {
        RuntimeWriteCurrentThreadSnapshot("text-xfc-loose-open-miss");
    }
}

// Xbox NtOpenFile is the FileDisposition::kOpen form of NtCreateFile. The
// reached request opens the real extracted D:\\ExtraContent directory before
// querying its volume geometry, so accept only paths that exist beneath the
// configured game root and preserve missing-path failures.
PPC_FUNC(__imp__NtOpenFile) {
    const uint32_t handleOut = ctx.r3.u32;
    const uint32_t desiredAccess = ctx.r4.u32;
    const uint32_t objectAttributes = ctx.r5.u32;
    const uint32_t ioStatus = ctx.r6.u32;
    const uint32_t shareAccess = ctx.r7.u32;
    const uint32_t openOptions = ctx.r8.u32;
    const uint32_t root = objectAttributes ? PPC_LOAD_U32(objectAttributes) : 0;
    const uint32_t ansiString = objectAttributes ? PPC_LOAD_U32(objectAttributes + 4) : 0;
    const uint32_t attributes = objectAttributes ? PPC_LOAD_U32(objectAttributes + 8) : 0;
    const uint16_t nameLength = ansiString ? PPC_LOAD_U16(ansiString) : 0;
    const uint32_t nameAddress = ansiString ? PPC_LOAD_U32(ansiString + 4) : 0;
    std::string path;
    if (nameAddress && nameLength <= 0x1000)
        path.assign(reinterpret_cast<const char*>(base + nameAddress), nameLength);

    const bool synchronous =
        (openOptions & (kFileSynchronousIoAlert | kFileSynchronousIoNonAlert)) != 0;
    constexpr uint32_t kDelete = 0x00010000u;
    const bool deleteRequested = (desiredAccess & kDelete) != 0;
    uint32_t handle =
        GetGuestFileSystem().OpenGameFile(path, synchronous, deleteRequested);
    if (!handle) handle = GetGuestFileSystem().OpenGameDirectory(path);
    const uint32_t status = handle ? kStatusSuccess : 0xC0000034; // STATUS_OBJECT_NAME_NOT_FOUND.
    if (handleOut) PPC_STORE_U32(handleOut, handle);
    if (ioStatus) {
        PPC_STORE_U32(ioStatus, status);
        PPC_STORE_U32(ioStatus + 4, handle ? 1 : 0); // FILE_OPEN.
    }
    ctx.r3.u64 = status;
    if (!handle || kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__NtOpenFile lr=0x" << std::hex << ctx.lr
                  << " handle=0x" << handle << " access=0x" << desiredAccess
                  << " root=0x" << root << " attrs=0x" << attributes
                  << " share=0x" << shareAccess << " options=0x" << openOptions
                  << " synchronous=" << std::dec << synchronous << std::hex
                  << " path=" << path << " status=0x" << status << std::dec << '\n';
    }
}

// Read only handles previously opened for real, extracted game files. This
// intentionally does not make the raw cache partition or missing files appear
// readable.
PPC_FUNC(__imp__NtReadFile) {
    const uint32_t handle = ctx.r3.u32;
    const uint32_t event = ctx.r4.u32;
    const uint32_t apc = ctx.r5.u32;
    const uint32_t apcContext = ctx.r6.u32;
    const uint32_t ioStatus = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32;
    const uint32_t byteCount = ctx.r9.u32;
    const uint32_t offsetPointer = ctx.r10.u32;
    uint32_t bytesRead{};
    const uint64_t offset = offsetPointer ? PPC_LOAD_U64(offsetPointer) : 0;
    bool synchronous{};
    const bool knownFile = GetGuestFileSystem().QueryOpenFileSynchronous(handle, &synchronous);
    const bool recordHitch = RuntimeHitchDiagnosticsEnabled();
    const auto readStart = recordHitch
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    bool supported = false;
    if (knownFile && buffer) {
        const RuntimeGuestSourceWriteScope source_write(buffer, byteCount);
        supported = GetGuestFileSystem().Read(handle, offset, base + buffer,
                                              byteCount, &bytesRead, !offsetPointer);
        // Publish cache invalidation while the guarded payload is still held.
        if (supported && bytesRead) RuntimeNotifyGuestPhysicalWrite(buffer, bytesRead);
    }
    const uint64_t readDurationUs = recordHitch
        ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - readStart).count())
        : 0;
    // File I/O writes through the host pointer rather than translated PPC
    // stores. Notify the shared GPU memory model before exposing completion so
    // resources previously cached from this physical range are invalidated.
    const uint32_t completionStatus = supported ? kStatusSuccess : kStatusInvalidHandle;
    const uint32_t returnStatus = supported && !synchronous ? kStatusPending : completionStatus;
    if (ioStatus) {
        PPC_STORE_U32(ioStatus, completionStatus);
        PPC_STORE_U32(ioStatus + 4, bytesRead);
    }
    if (supported && event && !GetGuestObjects().SetEvent(event, nullptr)) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (supported && apc && apcContext) {
        // The low bit is retained by the title's I/O helper; the queued normal
        // APC routine is the aligned address, exactly as the local Xbox kernel
        // reference performs for immediate file completions.
        QueueGuestApc(apc & ~1u, apcContext, ioStatus, 0);
    }
    ctx.r3.u64 = returnStatus;
    const uint64_t ordinal = ntReadTraceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    // Keep enough per-thread handle transition evidence to identify newly
    // opened streaming packages after the global startup trace becomes sparse.
    // This is observational only; the read and APC contracts above are
    // unchanged.
    static thread_local uint32_t tracedReadHandle = UINT32_MAX;
    const bool newReadHandle = handle != tracedReadHandle;
    if (newReadHandle) tracedReadHandle = handle;
    const bool traceRead = !supported || newReadHandle || ordinal <= 8 ||
                           (ordinal & 0xFFFu) == 0;
    const bool slowRead = recordHitch && supported && readDurationUs >= 75000;
    if (traceRead || slowRead) {
        std::string openPath;
        GetGuestFileSystem().QueryOpenFilePath(handle, &openPath);
        if (slowRead) {
            std::cout << "RUNTIME_IO_HITCH duration_us=" << readDurationUs
                      << " thread=" << CurrentGuestThreadId()
                      << " handle=0x" << std::hex << handle
                      << " buffer=0x" << buffer << " requested=0x" << byteCount
                      << " read=0x" << bytesRead << " offset=0x" << offset
                      << std::dec << " ordinal=" << ordinal
                      << " path=" << openPath << '\n';
        }
        if (traceRead) {
            std::cout << "IMPORT_CALL name=__imp__NtReadFile handle=0x" << std::hex << handle
                      << " event=0x" << event << " apc=0x" << apc
                      << " apc_context=0x" << apcContext << " iosb=0x" << ioStatus
                      << " buffer=0x" << buffer << " bytes=0x" << byteCount << " offset=0x"
                      << offset << " bytes_read=0x" << bytesRead
                      << " completion_status=0x" << completionStatus
                      << " return_status=0x" << returnStatus
                      << std::dec << " thread=" << CurrentGuestThreadId()
                      << " synchronous=" << synchronous << " new_handle=" << newReadHandle
                      << " ordinal=" << ordinal << " path=" << openPath << '\n';
        }
    }
}

// Writes are permitted only on handles opened for write beneath an explicit
// XAM portable-content mount. Completion follows the same immediate-backed,
// STATUS_PENDING-for-asynchronous contract as the pinned ReXGlue implementation.
PPC_FUNC(__imp__NtWriteFile) {
    const uint32_t handle = ctx.r3.u32;
    const uint32_t event = ctx.r4.u32;
    const uint32_t apc = ctx.r5.u32;
    const uint32_t apcContext = ctx.r6.u32;
    const uint32_t ioStatus = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32;
    const uint32_t byteCount = ctx.r9.u32;
    const uint32_t offsetPointer = ctx.r10.u32;
    const uint64_t offset = offsetPointer ? PPC_LOAD_U64(offsetPointer) : 0;
    uint32_t bytesWritten{};
    bool synchronous{};
    const bool knownFile = GetGuestFileSystem().QueryOpenFileSynchronous(handle, &synchronous);
    GuestFileWriteResult writeResult = GuestFileWriteResult::InvalidHandle;
    if (knownFile && (buffer || !byteCount)) {
        writeResult = GetGuestFileSystem().Write(
            handle, offset, buffer ? base + buffer : nullptr, byteCount, &bytesWritten,
            !offsetPointer);
    }
    uint32_t completionStatus = kStatusInvalidHandle;
    if (writeResult == GuestFileWriteResult::Success) completionStatus = kStatusSuccess;
    else if (writeResult == GuestFileWriteResult::AccessDenied)
        completionStatus = kStatusAccessDenied;
    const bool supported = completionStatus == kStatusSuccess;
    const uint32_t returnStatus = supported && !synchronous
        ? kStatusPending : completionStatus;
    if (ioStatus) {
        PPC_STORE_U32(ioStatus, completionStatus);
        PPC_STORE_U32(ioStatus + 4, supported ? bytesWritten : 0);
    }
    if (supported && event && !GetGuestObjects().SetEvent(event, nullptr)) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (supported && (apc & ~1u) && apcContext)
        QueueGuestApc(apc & ~1u, apcContext, ioStatus, 0);
    ctx.r3.u64 = returnStatus;

    const uint64_t ordinal = ntWriteTraceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!supported || ordinal <= 8 || (ordinal & 0xFFu) == 0) {
        std::string openPath;
        GetGuestFileSystem().QueryOpenFilePath(handle, &openPath);
        std::cout << "IMPORT_CALL name=__imp__NtWriteFile handle=0x" << std::hex << handle
                  << " event=0x" << event << " apc=0x" << apc
                  << " apc_context=0x" << apcContext << " iosb=0x" << ioStatus
                  << " buffer=0x" << buffer << " bytes=0x" << byteCount
                  << " offset=0x" << offset << " bytes_written=0x" << bytesWritten
                  << " completion_status=0x" << completionStatus
                  << " return_status=0x" << returnStatus << std::dec
                  << " thread=" << CurrentGuestThreadId()
                  << " synchronous=" << synchronous << " ordinal=" << ordinal
                  << " path=" << openPath << '\n';
    }
}

// The title has two static flush call sites, including the retained portable
// save path. NtFlushBuffersFile is synchronous: it validates the file handle,
// commits the authoritative host-backed file, and returns completion directly
// in both r3 and the required X_IO_STATUS_BLOCK. Information is always zero.
PPC_FUNC(__imp__NtFlushBuffersFile) {
    const uint32_t handle = ctx.r3.u32;
    const uint32_t ioStatus = ctx.r4.u32;
    uint32_t status = kStatusInvalidParameter;
    if (ioStatus && !(ioStatus & 3u) && ioStatus <= UINT32_MAX - 8u) {
        const GuestFileWriteResult flushResult = GetGuestFileSystem().Flush(handle);
        if (flushResult == GuestFileWriteResult::Success) status = kStatusSuccess;
        else if (flushResult == GuestFileWriteResult::InvalidHandle)
            status = kStatusInvalidHandle;
        else
            status = kStatusAccessDenied;
        PPC_STORE_U32(ioStatus, status);
        PPC_STORE_U32(ioStatus + 4, 0);
    }
    ctx.r3.u64 = status;
    std::string openPath;
    GetGuestFileSystem().QueryOpenFilePath(handle, &openPath);
    std::cout << "IMPORT_CALL name=__imp__NtFlushBuffersFile handle=0x" << std::hex
              << handle << " iosb=0x" << ioStatus << " status=0x" << status
              << " lr=0x" << ctx.lr << std::dec << " thread=" << CurrentGuestThreadId()
              << " path=" << openPath << '\n';
}

// Probe 58b reaches the standard eight-byte FilePositionInformation contract
// after reopening the newly persisted profile synchronously. The immediately
// following title routine also applies FileEndOfFileInformation and
// FileAllocationInformation to that position. Probe 65 additionally reaches
// the one-byte FileDispositionInformation contract while loading Chapter1.
// Keep these operations on the same host-backed file object; length and
// delete-on-close operations are real portable-storage mutations.
PPC_FUNC(__imp__NtSetInformationFile) {
    const uint32_t handle = ctx.r3.u32;
    const uint32_t ioStatus = ctx.r4.u32;
    const uint32_t information = ctx.r5.u32;
    const uint32_t informationLength = ctx.r6.u32;
    const uint32_t informationClass = ctx.r7.u32;
    uint32_t status = kStatusSuccess;
    uint32_t completedLength{};
    uint64_t informationValue{};
    if (informationClass != 13 && informationClass != 14 &&
        informationClass != 19 && informationClass != 20) {
        status = kStatusInvalidInfoClass;
    } else if (informationLength < (informationClass == 13 ? 1u : 8u)) {
        status = kStatusInfoLengthMismatch;
    } else if (!information) {
        status = kStatusInvalidParameter;
    } else if (informationClass == 13) {
        informationValue = PPC_LOAD_U8(information) ? 1 : 0;
        const GuestFileWriteResult result = GetGuestFileSystem().SetOpenFileDisposition(
            handle, informationValue != 0);
        if (result == GuestFileWriteResult::InvalidHandle)
            status = kStatusInvalidHandle;
        else if (result == GuestFileWriteResult::AccessDenied)
            status = kStatusAccessDenied;
        // X_FILE_DISPOSITION_INFORMATION updates no caller output bytes.
    } else {
        informationValue = PPC_LOAD_U64(information);
        if (informationClass == 14) {
            if (!GetGuestFileSystem().SetOpenFilePosition(handle, informationValue))
                status = kStatusInvalidHandle;
            else
                completedLength = 8;
        } else {
            const GuestFileWriteResult result =
                GetGuestFileSystem().SetOpenFileLength(handle, informationValue);
            if (result == GuestFileWriteResult::InvalidHandle)
                status = kStatusInvalidHandle;
            else if (result == GuestFileWriteResult::AccessDenied)
                status = kStatusAccessDenied;
            else
                completedLength = 8;
        }
    }
    if (ioStatus) {
        PPC_STORE_U32(ioStatus, status);
        PPC_STORE_U32(ioStatus + 4, completedLength);
    }
    ctx.r3.u64 = status;
    std::string openPath;
    GetGuestFileSystem().QueryOpenFilePath(handle, &openPath);
    std::cout << "IMPORT_CALL name=__imp__NtSetInformationFile handle=0x"
              << std::hex << handle << " iosb=0x" << ioStatus
              << " info=0x" << information << " length=0x" << informationLength
              << " class=0x" << informationClass << " value=0x" << informationValue
              << " completed=0x" << completedLength << " status=0x" << status
              << " lr=0x" << ctx.lr << std::dec
              << " thread=" << CurrentGuestThreadId() << " path=" << openPath << '\n';
}

// Resolve attributes only against the directory containing the supplied XEX.
// This makes the actual extracted game files visible while preserving genuine
// misses such as D:\\EnvironmentXbox.cfg and the absent Content\\P5.cfg.
PPC_FUNC(__imp__NtQueryFullAttributesFile) {
    const uint32_t objectAttributes = ctx.r3.u32;
    const uint32_t ansiString = objectAttributes ? PPC_LOAD_U32(objectAttributes + 4) : 0;
    const uint16_t nameLength = ansiString ? PPC_LOAD_U16(ansiString) : 0;
    const uint32_t nameAddress = ansiString ? PPC_LOAD_U32(ansiString + 4) : 0;
    std::string path;
    if (nameAddress && nameLength <= 0x1000) {
        path.assign(reinterpret_cast<const char*>(base + nameAddress), nameLength);
    }
    // The title reaches this import through its 0x828A85F0 wrapper. That
    // wrapper allocates 176 bytes after saving its caller LR at old-r1-8, so
    // the title call site is preserved at new-r1+168. Keep it observable for
    // missing packaged-resource investigations; this does not alter lookup
    // behavior or guest memory.
    const uint32_t wrapperCaller = ctx.r1.u32 <= UINT32_MAX - 0xA8u
        ? PPC_LOAD_U32(ctx.r1.u32 + 0xA8u)
        : 0;
    if (!HasXboxPathDevice(path)) {
        // Verified against pinned Xenia 95a5c3e: the title's initial
        // NtQueryFullAttributesFile("Content") returns 0xC000000F. The game
        // then constructs D:\\Content\\XDF\\GameContext_Create.XDF. Treating
        // the bare name as host-root-relative truncates the guest package
        // prefix and prevents the XDF streaming provider from ever matching.
        ctx.r3.u64 = kStatusNoSuchFile;
        if (kCompletedHotPathDiagnosticsEnabled) {
            std::cout << "IMPORT_CALL name=__imp__NtQueryFullAttributesFile lr=0x"
                      << std::hex << ctx.lr << " caller=0x" << wrapperCaller
                      << " r1=0x" << ctx.r1.u32 << std::dec << " path=" << path
                      << " status=0xc000000f\n";
        }
        return;
    }
    GuestFileAttributes attributes{};
    if (!GetGuestFileSystem().QueryGamePath(path, &attributes)) {
        ctx.r3.u64 = 0xC0000034u; // STATUS_OBJECT_NAME_NOT_FOUND.
        if (kCompletedHotPathDiagnosticsEnabled) {
            std::cout << "IMPORT_CALL name=__imp__NtQueryFullAttributesFile lr=0x" << std::hex
                      << ctx.lr << " caller=0x" << wrapperCaller << " r1=0x" << ctx.r1.u32
                      << std::dec << " path=" << path
                      << " status=0xc0000034\n";
        }
        return;
    }
    const uint32_t output = ctx.r4.u32;
    if (!output) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    PPC_STORE_U64(output + 0, attributes.creationTime);
    PPC_STORE_U64(output + 8, attributes.lastAccessTime);
    PPC_STORE_U64(output + 16, attributes.lastWriteTime);
    PPC_STORE_U64(output + 24, attributes.changeTime);
    PPC_STORE_U64(output + 32, attributes.allocationSize);
    PPC_STORE_U64(output + 40, attributes.endOfFile);
    PPC_STORE_U32(output + 48, attributes.attributes);
    ctx.r3.u64 = kStatusSuccess;
    if (kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__NtQueryFullAttributesFile lr=0x" << std::hex
                  << ctx.lr << " caller=0x" << wrapperCaller << " r1=0x" << ctx.r1.u32
                  << " path=" << path
                  << " bytes=0x" << std::hex << attributes.endOfFile
                  << " attrs=0x" << attributes.attributes << " status=0x0" << std::dec << '\n';
    }
}

// The first opened XDF asset asks for FileNetworkOpenInformation (class 0x22).
// Probe 59 then reaches FilePositionInformation (class 14) while finalizing the
// newly created portable profile. Return the live per-handle position; the
// guest immediately uses it as the file's EOF and allocation length.
PPC_FUNC(__imp__NtQueryInformationFile) {
    const uint32_t handle = ctx.r3.u32;
    const uint32_t ioStatus = ctx.r4.u32;
    const uint32_t output = ctx.r5.u32;
    const uint32_t outputSize = ctx.r6.u32;
    const uint32_t informationClass = ctx.r7.u32;
    uint32_t status = kStatusSuccess;
    uint32_t completedLength{};
    uint64_t reportedValue{};
    if (informationClass == 14) {
        if (!output || outputSize < 8) {
            status = kStatusInfoLengthMismatch;
        } else if (!GetGuestFileSystem().QueryOpenFilePosition(handle, &reportedValue)) {
            status = kStatusInvalidHandle;
        } else {
            PPC_STORE_U64(output, reportedValue);
            completedLength = 8;
        }
    } else if (informationClass == 0x22) {
        if (!output || outputSize < 56) {
            status = kStatusInfoLengthMismatch;
        } else {
            GuestFileAttributes attributes{};
            if (!GetGuestFileSystem().QueryOpenFile(handle, &attributes)) {
                status = kStatusInvalidHandle;
            } else {
                PPC_STORE_U64(output + 0, attributes.creationTime);
                PPC_STORE_U64(output + 8, attributes.lastAccessTime);
                PPC_STORE_U64(output + 16, attributes.lastWriteTime);
                PPC_STORE_U64(output + 24, attributes.changeTime);
                PPC_STORE_U64(output + 32, attributes.allocationSize);
                PPC_STORE_U64(output + 40, attributes.endOfFile);
                PPC_STORE_U32(output + 48, attributes.attributes);
                reportedValue = attributes.endOfFile;
                completedLength = 56;
            }
        }
    } else {
        status = kStatusInvalidInfoClass;
    }
    if (ioStatus) {
        PPC_STORE_U32(ioStatus, status);
        PPC_STORE_U32(ioStatus + 4, completedLength);
    }
    ctx.r3.u64 = status;
    std::string openPath;
    GetGuestFileSystem().QueryOpenFilePath(handle, &openPath);
    std::cout << "IMPORT_CALL name=__imp__NtQueryInformationFile handle=0x" << std::hex << handle
              << " iosb=0x" << ioStatus << " output=0x" << output
              << " size=0x" << outputSize << " class=0x" << informationClass
              << " value=0x" << reportedValue << " completed=0x" << completedLength
              << " status=0x" << status << " lr=0x" << ctx.lr << std::dec
              << " thread=" << CurrentGuestThreadId() << " path=" << openPath << '\n';
}

// The live ExtraContent probe requests FileFsSizeInformation (class 3), whose
// 24-byte Xbox ABI is two allocation-unit counts followed by sectors per
// allocation unit and bytes per sector. Values are obtained from the host
// volume actually holding the extracted game root.
PPC_FUNC(__imp__NtQueryVolumeInformationFile) {
    const uint32_t handle = ctx.r3.u32;
    const uint32_t ioStatus = ctx.r4.u32;
    const uint32_t output = ctx.r5.u32;
    const uint32_t outputSize = ctx.r6.u32;
    const uint32_t informationClass = ctx.r7.u32;
    if (informationClass != 3) {
        ctx.r3.u64 = kStatusNotImplemented;
        return;
    }
    if (!output || outputSize < 24) {
        ctx.r3.u64 = kStatusBufferTooSmall;
        return;
    }
    GuestVolumeSizeInformation information{};
    if (!GetGuestFileSystem().QueryVolumeSize(handle, &information)) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    PPC_STORE_U64(output + 0, information.totalAllocationUnits);
    PPC_STORE_U64(output + 8, information.availableAllocationUnits);
    PPC_STORE_U32(output + 16, information.sectorsPerAllocationUnit);
    PPC_STORE_U32(output + 20, information.bytesPerSector);
    if (ioStatus) {
        PPC_STORE_U32(ioStatus, kStatusSuccess);
        PPC_STORE_U32(ioStatus + 4, 24);
    }
    ctx.r3.u64 = kStatusSuccess;
    std::cout << "IMPORT_CALL name=__imp__NtQueryVolumeInformationFile handle=0x" << std::hex << handle
              << " class=0x3 clusters=0x" << information.totalAllocationUnits
              << " free=0x" << information.availableAllocationUnits
              << " sectors_per_unit=0x" << information.sectorsPerAllocationUnit
              << " bytes_per_sector=0x" << information.bytesPerSector << std::dec << '\n';
}

// The reached wildcard probe is NtQueryDirectoryFile against the actual
// extracted ExtraContent directory. X_FILE_DIRECTORY_INFORMATION is a
// 64-byte big-endian header followed by the ANSI child name.
PPC_FUNC(__imp__NtQueryDirectoryFile) {
    RuntimeTraceImport("NtQueryDirectoryFile", ctx);
    const uint32_t handle = ctx.r3.u32;
    const uint32_t ioStatus = ctx.r7.u32;
    const uint32_t output = ctx.r8.u32;
    const uint32_t outputSize = ctx.r9.u32;
    const uint32_t ansiString = ctx.r10.u32;
    const uint16_t nameLength = ansiString ? PPC_LOAD_U16(ansiString) : 0;
    const uint32_t nameAddress = ansiString ? PPC_LOAD_U32(ansiString + 4) : 0;
    std::string pattern;
    if (nameAddress && nameLength <= 0x1000)
        pattern.assign(reinterpret_cast<const char*>(base + nameAddress), nameLength);
    if (!output || outputSize < 72) {
        ctx.r3.u64 = kStatusInfoLengthMismatch;
        return;
    }

    GuestDirectoryEntry entry{};
    // NtQueryDirectoryFile has nine integer arguments. PowerPC passes the
    // ninth (RestartScan) in the caller's parameter area at r1 + 0x54; r11
    // contains the indirect import target at the dynamically reached calls.
    const bool restartScan = PPC_LOAD_U32(ctx.r1.u32 + 0x54u) != 0;
    const auto result = GetGuestFileSystem().QueryDirectory(handle, pattern, restartScan, &entry);
    uint32_t status = kStatusSuccess;
    if (result == GuestDirectoryQueryResult::InvalidHandle) status = kStatusInvalidHandle;
    if (result == GuestDirectoryQueryResult::NoSuchFile) status = kStatusNoSuchFile;
    if (result == GuestDirectoryQueryResult::NoMoreFiles) status = kStatusNoMoreFiles;
    if (result == GuestDirectoryQueryResult::Success && uint64_t(64) + entry.name.size() > outputSize)
        status = kStatusInfoLengthMismatch;

    if (result == GuestDirectoryQueryResult::Success && status == kStatusSuccess) {
        PPC_STORE_U32(output + 0, 0);
        PPC_STORE_U32(output + 4, entry.index);
        PPC_STORE_U64(output + 8, entry.attributes.creationTime);
        PPC_STORE_U64(output + 16, entry.attributes.lastAccessTime);
        PPC_STORE_U64(output + 24, entry.attributes.lastWriteTime);
        PPC_STORE_U64(output + 32, entry.attributes.changeTime);
        PPC_STORE_U64(output + 40, entry.attributes.endOfFile);
        PPC_STORE_U64(output + 48, entry.attributes.allocationSize);
        PPC_STORE_U32(output + 56, entry.attributes.attributes);
        PPC_STORE_U32(output + 60, static_cast<uint32_t>(entry.name.size()));
        if (!entry.name.empty()) {
            const RuntimeGuestWriteCompletion outputCompletion(uint64_t(output) + 64,
                static_cast<uint32_t>(entry.name.size()));
            std::memcpy(base + output + 64, entry.name.data(), entry.name.size());
        }
    }
    if (ioStatus) {
        PPC_STORE_U32(ioStatus, status);
        PPC_STORE_U32(ioStatus + 4, status == kStatusSuccess ? outputSize : 0);
    }
    ctx.r3.u64 = status;
    if (status == kStatusInvalidHandle || kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__NtQueryDirectoryFile handle=0x" << std::hex << handle
                  << " pattern=" << pattern << " entry=" << entry.name
                  << " restart=" << std::dec << restartScan << " status=0x" << std::hex << status
                  << std::dec << '\n';
    }
    if (status == kStatusSuccess) RuntimeTracePostContentCompleted();
}

// Category 3 is XCONFIG_USER_CATEGORY. The title dynamically requests the
// timezone fields (1..7), language (9), video flags (10), and retail flags
// (12). The pinned ReXGlue/Xenia-derived kernel model defines video flags as
// 0x00040000 and retail flags as 0x40. English is language value 1 and matches
// the title's Content_Eng tree.
PPC_FUNC(__imp__ExGetXConfigSetting) {
    RuntimeTraceImport("ExGetXConfigSetting", ctx);
    const uint16_t category = ctx.r3.u16;
    const uint16_t setting = ctx.r4.u16;
    const uint32_t buffer = ctx.r5.u32;
    const uint16_t bufferSize = ctx.r6.u16;
    const uint32_t requiredSize = ctx.r7.u32;
    constexpr uint16_t kSettingSize = 4;
    uint32_t value{};
    const bool zeroSetting = setting >= 1 && setting <= 7;
    if (category != 3 || (!zeroSetting && setting != 9 && setting != 10 && setting != 12)) {
        ctx.r3.u64 = kStatusInvalidParameter;
        std::cout << "IMPORT_CALL name=__imp__ExGetXConfigSetting category=" << category
                  << " setting=" << setting << " status=0x" << std::hex
                  << kStatusInvalidParameter << std::dec << '\n';
        return;
    }
    if (setting == 9) value = GetGuestXamState().Language();
    if (setting == 10) value = 0x00040000;
    if (setting == 12) value = 0x40;
    if (!buffer || bufferSize < kSettingSize) {
        if (requiredSize) PPC_STORE_U16(requiredSize, kSettingSize);
        ctx.r3.u64 = kStatusBufferTooSmall;
        return;
    }
    PPC_STORE_U32(buffer, value);
    if (requiredSize) PPC_STORE_U16(requiredSize, kSettingSize);
    ctx.r3.u64 = kStatusSuccess;
    static std::atomic<uint64_t> traceCount{};
    const uint64_t ordinal = traceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal <= 32 || (ordinal & 0xFFu) == 0) {
        std::cout << "IMPORT_CALL name=__imp__ExGetXConfigSetting category=" << category
                  << " setting=" << setting << " value=" << value << " bytes=" << kSettingSize
                  << " status=0 ordinal=" << ordinal << '\n';
    }
}

// Verified Xbox ABI: up to three input buffers, each paired with a size,
// followed by a caller-specified output buffer and byte count. SHA-1 output
// is always 20 bytes and is truncated only when the caller supplies less.
PPC_FUNC(__imp__XeCryptSha) {
    const uint32_t inputs[] = {ctx.r3.u32, ctx.r5.u32, ctx.r7.u32};
    const uint32_t inputSizes[] = {ctx.r4.u32, ctx.r6.u32, ctx.r8.u32};
    const uint32_t output = ctx.r9.u32;
    const uint32_t outputSize = ctx.r10.u32;
    sha1::SHA1 sha;
    for (size_t index = 0; index < 3; ++index) {
        if (inputs[index] && inputSizes[index]) sha.processBytes(base + inputs[index], inputSizes[index]);
    }
    uint8_t digest[20]{};
    sha.finalize(digest);
    if (output && outputSize) {
        const uint32_t bytes = std::min<uint32_t>(outputSize, sizeof(digest));
        const RuntimeGuestWriteCompletion outputCompletion(output, bytes);
        std::memcpy(base + output, digest, bytes);
    }
    std::cout << "IMPORT_CALL name=__imp__XeCryptSha input1=0x" << std::hex << inputs[0]
              << " bytes1=0x" << inputSizes[0] << " output=0x" << output
              << " output_bytes=0x" << outputSize << std::dec << '\n';
}

PPC_FUNC(__imp__KeTlsAlloc) {
    const uint32_t slot = GetGuestTls().Allocate();
    ctx.r3.u64 = slot;
    std::cout << "IMPORT_CALL name=__imp__KeTlsAlloc slot=" << slot << '\n';
}

PPC_FUNC(__imp__KeTlsFree) {
    const uint32_t slot = ctx.r3.u32;
    const bool freed = GetGuestTls().Free(slot);
    ctx.r3.u64 = freed ? 1 : 0;
    std::cout << "IMPORT_CALL name=__imp__KeTlsFree slot=" << slot
              << " result=" << freed << '\n';
}

PPC_FUNC(__imp__KeTlsGetValue) {
    ctx.r3.u64 = GetGuestTls().Get(ctx.r3.u32);
}

PPC_FUNC(__imp__KeTlsSetValue) {
    ctx.r3.u64 = GetGuestTls().Set(ctx.r3.u32, ctx.r4.u32) ? 1 : 0;
}

PPC_FUNC(__imp__NtCreateEvent) {
    const uint32_t handleAddress = ctx.r3.u32;
    const uint32_t attributesAddress = ctx.r4.u32;
    const uint32_t eventType = ctx.r5.u32;
    const uint32_t initialState = ctx.r6.u32;
    if (attributesAddress) {
        // Named-object attribute parsing has not been reached or verified.
        ctx.r3.u64 = kStatusNotImplemented;
        return;
    }
    if (eventType > 1) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    // NotificationEvent (0) is manual-reset; SynchronizationEvent (1) is
    // auto-reset. The initial boot call is the latter with initial state 0.
    const auto event = GetGuestObjects().CreateEvent(eventType == 0, initialState != 0);
    if (handleAddress) PPC_STORE_U32(handleAddress, event.handle);
    ctx.r3.u64 = kStatusSuccess;
    std::cout << "IMPORT_CALL name=__imp__NtCreateEvent handle=0x" << std::hex << event.handle
              << " type=" << eventType << " initial=" << initialState << std::dec << '\n';
}

PPC_FUNC(__imp__NtSetEvent) {
    const uint32_t handle = ctx.r3.u32;
    const uint32_t previousStateAddress = ctx.r4.u32;
    uint32_t previous{};
    if (!GetGuestObjects().SetEvent(handle, &previous)) {
        ctx.r3.u64 = kStatusInvalidHandle;
        std::cout << "IMPORT_CALL name=__imp__NtSetEvent thread="
                  << CurrentGuestThreadId() << " handle=0x" << std::hex << handle
                  << " lr=0x" << ctx.lr << " r1=0x" << ctx.r1.u32
                  << " status=0x" << kStatusInvalidHandle << std::dec
                  << " invalid_handle=1\n";
        return;
    }
    if (previousStateAddress) PPC_STORE_U32(previousStateAddress, previous);
    ctx.r3.u64 = kStatusSuccess;
    if (kCompletedHotPathDiagnosticsEnabled) {
        const uint64_t ordinal =
            ntSetEventTraceCount.fetch_add(1, std::memory_order_relaxed) + 1;
        std::cout << "IMPORT_CALL name=__imp__NtSetEvent thread=" << CurrentGuestThreadId()
                  << " handle=0x" << std::hex << handle << " previous=0x" << previous
                  << " lr=0x" << ctx.lr << " r1=0x" << ctx.r1.u32
                  << " status=0x0" << std::dec << " ordinal=" << ordinal << '\n';
    }
}

PPC_FUNC(__imp__NtClearEvent) {
    const uint32_t handle = ctx.r3.u32;
    if (!GetGuestObjects().ClearEvent(handle)) {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    ctx.r3.u64 = kStatusSuccess;
    std::cout << "IMPORT_CALL name=__imp__NtClearEvent thread=" << CurrentGuestThreadId()
              << " handle=0x" << std::hex << handle << " status=0x0" << std::dec << '\n';
}

PPC_FUNC(__imp__NtClose) {
    const uint32_t handle = ctx.r3.u32;
    ctx.r3.u64 = (GetGuestFileSystem().Close(handle) || GetGuestObjects().Close(handle))
        ? kStatusSuccess : kStatusInvalidHandle;
}

PPC_FUNC(__imp__NtCreateSemaphore) {
    const uint32_t handleAddress = ctx.r3.u32;
    const uint32_t attributesAddress = ctx.r4.u32;
    const uint32_t initialCount = ctx.r5.u32;
    const uint32_t limit = ctx.r6.u32;
    if (attributesAddress) {
        ctx.r3.u64 = kStatusNotImplemented;
        return;
    }
    if (!limit || initialCount > limit) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    const auto semaphore = GetGuestObjects().CreateSemaphore(initialCount, limit);
    if (handleAddress) PPC_STORE_U32(handleAddress, semaphore.handle);
    ctx.r3.u64 = kStatusSuccess;
    std::cout << "IMPORT_CALL name=__imp__NtCreateSemaphore handle=0x" << std::hex << semaphore.handle
              << " initial=" << initialCount << " limit=" << limit << std::dec << '\n';
}

PPC_FUNC(__imp__ExCreateThread) {
    const uint32_t handleAddress = ctx.r3.u32;
    const uint32_t stackSize = ctx.r4.u32;
    const uint32_t threadIdAddress = ctx.r5.u32;
    const uint32_t xapiStartup = ctx.r6.u32;
    const uint32_t startAddress = ctx.r7.u32;
    const uint32_t startContext = ctx.r8.u32;
    const uint32_t creationFlags = ctx.r9.u32;
    if (!startAddress) {
        ctx.r3.u64 = kStatusInvalidParameter;
        std::cout << "IMPORT_CALL name=__imp__ExCreateThread rejected=missing_start"
                  << " startup=0x" << std::hex << xapiStartup << " flags=0x"
                  << creationFlags << std::dec << '\n';
        return;
    }
    const auto thread = StartGuestThread(ctx, base, stackSize, xapiStartup, startAddress,
                                         startContext, creationFlags);
    const uint32_t returnedHandle = (creationFlags & 0x80u) ? thread.guestObject : thread.handle;
    if (handleAddress) PPC_STORE_U32(handleAddress, returnedHandle);
    if (threadIdAddress) PPC_STORE_U32(threadIdAddress, thread.threadId);
    ctx.r3.u64 = kStatusSuccess;
    std::cout << "IMPORT_CALL name=__imp__ExCreateThread parent=" << CurrentGuestThreadId()
              << " handle=0x" << std::hex << returnedHandle
              << " id=" << std::dec << thread.threadId << " startup=0x" << std::hex << xapiStartup
              << " entry=0x" << startAddress << " context=0x" << startContext
              << " flags=0x" << creationFlags << std::dec << '\n';
}

PPC_FUNC(__imp__NtResumeThread) {
    const uint32_t handle = ctx.r3.u32;
    const uint32_t suspendCountAddress = ctx.r4.u32;
    uint32_t previousSuspendCount{};
    GuestObjectInfo object{};
    uint32_t status = kStatusInvalidHandle;
    if (GetGuestObjects().GetObjectInfo(handle, &object) &&
        object.type == GuestObjectType::Thread &&
        ResumeGuestThread(object.guestObject, &previousSuspendCount)) {
        status = kStatusSuccess;
    }
    // NtResumeThread always publishes the observed prior count when the
    // optional output pointer is supplied. This includes zero on lookup
    // failure, matching the pinned Xenia/ReXGlue kernel contract.
    if (suspendCountAddress) PPC_STORE_U32(suspendCountAddress, previousSuspendCount);
    ctx.r3.u64 = status;
    std::cout << "IMPORT_CALL name=__imp__NtResumeThread handle=0x" << std::hex << handle
              << " thread_object=0x" << object.guestObject << " status=0x" << status
              << std::dec << " previous_suspend_count=" << previousSuspendCount << '\n';
}

PPC_FUNC(__imp__NtSuspendThread) {
    const uint32_t handle = ctx.r3.u32;
    const uint32_t suspendCountAddress = ctx.r4.u32;
    uint32_t previousSuspendCount{};
    GuestObjectInfo object{};
    uint32_t status = kStatusInvalidHandle;
    if (GetGuestObjects().GetObjectInfo(handle, &object) &&
        object.type == GuestObjectType::Thread) {
        status = SuspendGuestThread(object.guestObject, &previousSuspendCount)
            ? kStatusSuccess : kStatusUnsuccessful;
    }
    if (suspendCountAddress) PPC_STORE_U32(suspendCountAddress, previousSuspendCount);
    ctx.r3.u64 = status;
    std::cout << "IMPORT_CALL name=__imp__NtSuspendThread handle=0x" << std::hex << handle
              << " thread_object=0x" << object.guestObject << " status=0x" << status
              << std::dec << " previous_suspend_count=" << previousSuspendCount << '\n';
}

PPC_FUNC(__imp__KeResumeThread) {
    const uint32_t threadObject = ctx.r3.u32;
    uint32_t previousSuspendCount{};
    if (!ResumeGuestThread(threadObject, &previousSuspendCount)) {
        ctx.r3.u64 = kStatusInvalidHandle;
        std::cout << "IMPORT_CALL name=__imp__KeResumeThread thread_object=0x" << std::hex
                  << threadObject << " status=0x" << kStatusInvalidHandle << std::dec << '\n';
        return;
    }
    ctx.r3.u64 = previousSuspendCount;
    std::cout << "IMPORT_CALL name=__imp__KeResumeThread thread_object=0x" << std::hex
              << threadObject << std::dec << " previous_suspend_count=" << previousSuspendCount
              << '\n';
}

PPC_FUNC(__imp__ExTerminateThread) {
    const uint32_t exitCode = ctx.r3.u32;
    std::cout << "IMPORT_CALL name=__imp__ExTerminateThread thread=" << CurrentGuestThreadId()
              << " exit_code=0x" << std::hex << exitCode << " lr=0x" << ctx.lr << std::dec << '\n';
    MarkCurrentGuestThreadTerminated(base, exitCode);
    throw GuestThreadExit();
}

PPC_FUNC(__imp__ObReferenceObjectByHandle) {
    const uint32_t handle = ctx.r3.u32;
    const uint32_t typeToken = ctx.r4.u32;
    uint32_t guestObject{};
    const uint32_t status = GetGuestObjects().ReferenceByHandle(
        handle, typeToken, CurrentGuestThreadObject(), &guestObject);
    if (!status && ctx.r5.u32) PPC_STORE_U32(ctx.r5.u32, guestObject);
    ctx.r3.u64 = status;
    if constexpr (kCompletedHotPathDiagnosticsEnabled) {
        static std::atomic<uint64_t> callCount{};
        const uint64_t ordinal = callCount.fetch_add(1, std::memory_order_relaxed) + 1;
        std::cout << "IMPORT_CALL name=__imp__ObReferenceObjectByHandle thread="
                  << CurrentGuestThreadId() << " lr=0x" << std::hex << ctx.lr
                  << " r1=0x" << ctx.r1.u32 << " handle=0x" << handle << " type=0x"
                  << typeToken << " object=0x" << guestObject << " status=0x" << status
                  << std::dec << " ordinal=" << ordinal << '\n';
    }
}

PPC_FUNC(__imp__ObDereferenceObject) {
    const uint32_t guestObject = ctx.r3.u32;
    if (!GetGuestObjects().DereferenceGuestObject(ctx.r3.u32)) {
        throw std::runtime_error("ObDereferenceObject received an invalid or unreferenced guest object");
    }
    if constexpr (kCompletedHotPathDiagnosticsEnabled) {
        static std::atomic<uint64_t> callCount{};
        const uint64_t ordinal = callCount.fetch_add(1, std::memory_order_relaxed) + 1;
        std::cout << "IMPORT_CALL name=__imp__ObDereferenceObject thread="
                  << CurrentGuestThreadId() << " lr=0x" << std::hex << ctx.lr
                  << " object=0x" << guestObject << std::dec << " ordinal=" << ordinal << '\n';
    }
}

PPC_FUNC(__imp__KeSetAffinityThread) {
    uint32_t previous{};
    const uint32_t threadObject = ctx.r3.u32;
    const uint32_t affinity = ctx.r4.u32;
    if (!SetGuestThreadAffinity(base, threadObject, affinity, &previous)) {
        ctx.r3.u64 = kStatusInvalidParameter;
        std::cout << "IMPORT_CALL name=__imp__KeSetAffinityThread thread_object=0x"
                  << std::hex << threadObject << " affinity=0x" << affinity
                  << " status=0x" << kStatusInvalidParameter << std::dec << '\n';
        return;
    }
    if (ctx.r5.u32) PPC_STORE_U32(ctx.r5.u32, previous);
    ctx.r3.u64 = kStatusSuccess;
    if constexpr (kCompletedHotPathDiagnosticsEnabled) {
        uint8_t processor{};
        GuestProcessorNumberFromAffinity(affinity, &processor);
        std::cout << "IMPORT_CALL name=__imp__KeSetAffinityThread thread_object=0x"
                  << std::hex << threadObject << " affinity=0x" << affinity
                  << " previous=0x" << previous << std::dec
                  << " processor=" << uint32_t(processor) << " status=0x0\n";
    }
}

PPC_FUNC(__imp__KeSetBasePriorityThread) {
    int32_t previous{};
    if (!GetGuestObjects().SetThreadPriority(ctx.r3.u32, ctx.r4.s32, &previous)) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    ctx.r3.s64 = previous;
}

PPC_FUNC(__imp__KeDelayExecutionThread) {
    const uint32_t processorMode = ctx.r3.u32;
    const uint32_t alertable = ctx.r4.u32;
    const uint32_t intervalAddress = ctx.r5.u32;
    if (!intervalAddress) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    const int64_t interval = static_cast<int64_t>(PPC_LOAD_U64(intervalAddress));
    const uint32_t threadObject = CurrentGuestThreadObject();
    if (!threadObject || !GetGuestObjects().SetThreadState(threadObject, GuestThreadState::Waiting)) {
        throw std::runtime_error("KeDelayExecutionThread called without a live guest thread");
    }
    DeliverGuestApcs(ctx, base);
    // The job-dependency poll may end early once its dependency completed
    // (runtime_job_poll_wake.h); every other delay is unchanged.
    if (!RuntimeJobPollEarlyWake(ctx, base, interval, alertable)) {
        DelayForGuestInterval(interval);
    }
    GetGuestObjects().SetThreadState(threadObject, GuestThreadState::Running);
    ctx.r3.u64 = kStatusSuccess;
    static std::atomic<uint32_t> delayTraceCount{};
    const uint32_t count = delayTraceCount.fetch_add(1);
    if (count < 16 || (count & 0x3FF) == 0) {
        // sub_828AC000 saves its caller LR at r1+104 before issuing this
        // import. Capture that return site to distinguish the generic sleep
        // wrapper from the title loop that is polling it.
        const uint32_t caller = PPC_LOAD_U32(ctx.r1.u32 + 104);
        std::cout << "IMPORT_CALL name=__imp__KeDelayExecutionThread thread=" << CurrentGuestThreadId()
                  << " mode=" << processorMode
                  << " alertable=" << alertable << " interval=" << interval
                  << " lr=0x" << std::hex << ctx.lr << " caller=0x" << caller << std::dec
                  << " count=" << count + 1 << "\n";
    }
}

// The title reaches this through a worker bootstrap with enabled=0. The
// recompiled FP helpers own host FP control state, so changing the host mask
// here would make a guest request affect unrelated host code. Keep the
// per-guest-thread mode explicitly; a future generated FP exception path can
// consult it before delivering a guest exception.
PPC_FUNC(__imp__KeEnableFpuExceptions) {
    const bool enabled = ctx.r3.u32 != 0;
    if (!GetGuestObjects().SetThreadFpuExceptionsEnabled(CurrentGuestThreadObject(), enabled)) {
        throw std::runtime_error("KeEnableFpuExceptions called without a live guest thread");
    }
    std::cout << "IMPORT_CALL name=__imp__KeEnableFpuExceptions enabled=" << enabled << '\n';
}

// ---- V380: imports that were unresolved-import traps ----------------------
// Each trap ended the game without a trace for players. These are the ones a
// single-player session can reach; the rest stay fatal (and now reported).
namespace {
std::string SafeGuestCString(uint8_t* base, uint32_t address, uint32_t maximumBytes) {
    std::string value;
    if (!address || address > UINT32_MAX - maximumBytes) return value;
    for (uint32_t index = 0; index < maximumBytes; ++index) {
        const char character = static_cast<char>(PPC_LOAD_U8(address + index));
        if (!character) break;
        value.push_back(character);
    }
    return value;
}

std::u16string SafeGuestWideString(uint8_t* base, uint32_t address, uint32_t maximumChars) {
    std::u16string value;
    if (!address || address > UINT32_MAX - maximumChars * 2u) return value;
    for (uint32_t index = 0; index < maximumChars; ++index) {
        const char16_t character = static_cast<char16_t>(PPC_LOAD_U16(address + index * 2u));
        if (!character) break;
        value.push_back(character);
    }
    return value;
}

RuntimeGuestFormatArgs GuestFormatArgs(uint8_t* base, std::function<uint64_t()> next) {
    RuntimeGuestFormatArgs args;
    args.next = std::move(next);
    args.read_string = [base](uint32_t address) { return SafeGuestCString(base, address, 4096); };
    args.read_wide = [base](uint32_t address) {
        return SafeGuestWideString(base, address, 4096);
    };
    return args;
}

bool RuntimeIsOfflineXgiMessage(uint32_t message) noexcept {
    switch (message) {
        case 0x000B0007u: case 0x000B0010u: case 0x000B0011u: case 0x000B0012u:
        case 0x000B0013u: case 0x000B0014u: case 0x000B0015u: case 0x000B0018u:
        case 0x000B001Au: case 0x000B001Bu: case 0x000B001Cu: case 0x000B0021u:
        case 0x000B0025u: case 0x000B0041u:
            return true;
        default:
            return false;
    }
}
}  // namespace

// Retail kernels print nothing without a debugger; the title's library debug
// prints (sub_8286C3C8 formats with _vsnprintf, then calls this) continue.
PPC_FUNC(__imp__DbgPrint) {
    RuntimeTraceImport("DbgPrint", ctx);
    static std::atomic<uint32_t> printed{};
    if (printed.fetch_add(1, std::memory_order_relaxed) < 64) {
        std::cerr << "GUEST_DBGPRINT lr=0x" << std::hex << ctx.lr << std::dec
                  << " text=" << SafeGuestCString(base, ctx.r3.u32, 512) << '\n';
    }
    ctx.r3.u64 = kStatusSuccess;
}

// int _vsnprintf(char* buffer, size_t count, const char* format, va_list args):
// Microsoft CRT semantics (-1 and no terminator when the text does not fit).
PPC_FUNC(__imp___vsnprintf) {
    RuntimeTraceImport("_vsnprintf", ctx);
    const uint32_t buffer = ctx.r3.u32;
    const uint32_t count = ctx.r4.u32;
    const uint32_t format = ctx.r5.u32;
    const uint32_t list = ctx.r6.u32;
    uint32_t index = 0;
    auto args = GuestFormatArgs(base, [&]() -> uint64_t {
        const uint32_t slot = list + 8u * index++;
        return list && slot <= UINT32_MAX - 8u ? PPC_LOAD_U64(slot) : 0ull;
    });
    const std::string text =
        RuntimeFormatGuestString(SafeGuestCString(base, format, 4096).c_str(), args);
    if (!buffer || !count || buffer > UINT32_MAX - count) {
        ctx.r3.s64 = -1;
        return;
    }
    const uint32_t copied = static_cast<uint32_t>(std::min<size_t>(text.size(), count));
    for (uint32_t i = 0; i < copied; ++i) PPC_STORE_U8(buffer + i, uint8_t(text[i]));
    if (text.size() < count) {
        PPC_STORE_U8(buffer + copied, 0);
        ctx.r3.s64 = static_cast<int64_t>(text.size());
    } else {
        ctx.r3.s64 = -1;
    }
}

// int sprintf(char* buffer, const char* format, ...): arguments from r5..r10,
// then the caller's parameter area (r1 + 0x50 + 8 per argument).
PPC_FUNC(__imp__sprintf) {
    RuntimeTraceImport("sprintf", ctx);
    const uint32_t buffer = ctx.r3.u32;
    const uint32_t format = ctx.r4.u32;
    const uint64_t registers[8] = {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64,
                                   ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
    const uint32_t stack = ctx.r1.u32;
    uint32_t index = 2;
    auto args = GuestFormatArgs(base, [&]() -> uint64_t {
        const uint32_t argument = index++;
        if (argument < 8) return registers[argument];
        const uint32_t slot = stack + 0x50u + 8u * (argument - 8u);
        return slot <= UINT32_MAX - 8u ? PPC_LOAD_U64(slot) : 0ull;
    });
    const std::string text =
        RuntimeFormatGuestString(SafeGuestCString(base, format, 4096).c_str(), args);
    if (!buffer || buffer > UINT32_MAX - static_cast<uint32_t>(text.size()) - 1u) {
        ctx.r3.s64 = -1;
        return;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        PPC_STORE_U8(buffer + static_cast<uint32_t>(i), uint8_t(text[i]));
    }
    PPC_STORE_U8(buffer + static_cast<uint32_t>(text.size()), 0);
    ctx.r3.s64 = static_cast<int64_t>(text.size());
}

// Xenia returns 6 (VGA). The title reads it only on its CRT fatal path
// (sub_828AA200) to choose how to show its error message.
PPC_FUNC(__imp__XGetAVPack) {
    RuntimeTraceImport("XGetAVPack", ctx);
    ctx.r3.u64 = 6;
}

// Whole-game audit (V432): system calls a player reaches from the menus
// (Extra Content > Achievements, Multiplayer) or that ran code gets close to
// (function coverage), answered as a console does for a local profile
// without Xbox Live instead of stopping the game (ReXGlue/Xenia semantics).

// The disc's game region: every region.
PPC_FUNC(__imp__XGetGameRegion) {
    RuntimeTraceImport("XGetGameRegion", ctx);
    ctx.r3.u64 = 0xFFFF;
}

// Guide UIs (achievements, friends, gamer card, player review): there is no
// guide overlay; the call succeeds and the game carries on.
PPC_FUNC(__imp__XamShowAchievementsUI) {
    RuntimeTraceImport("XamShowAchievementsUI", ctx);
    std::cout << "IMPORT_CALL name=__imp__XamShowAchievementsUI user=" << ctx.r3.u32
              << " result=no-guide-ui\n";
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XamShowFriendsUI) {
    RuntimeTraceImport("XamShowFriendsUI", ctx);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XamShowFriendRequestUI) {
    RuntimeTraceImport("XamShowFriendRequestUI", ctx);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XamShowGamerCardUIForXUID) {
    RuntimeTraceImport("XamShowGamerCardUIForXUID", ctx);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XamShowPlayerReviewUI) {
    RuntimeTraceImport("XamShowPlayerReviewUI", ctx);
    ctx.r3.u64 = 0;
}

// Sign-in UI (asked before Xbox Live features): the guide opens and closes at
// once, as when the player backs out on a console (XN_SYS_UI on, then off).
// The profile stays signed in locally. Nothing about the sign-in changed, so
// there is no XN_SYS_SIGNINCHANGED: V432 sent one and the title answered
// "Restarting due to Gamer Profile changes" (V433 Multiplayer runs).
PPC_FUNC(__imp__XamShowSigninUI) {
    RuntimeTraceImport("XamShowSigninUI", ctx);
    constexpr uint32_t kSysUi = 0x00000009u;
    GetGuestObjects().BroadcastNotification(kSysUi, 1);
    GetGuestObjects().BroadcastNotification(kSysUi, 0);
    std::cout << "IMPORT_CALL name=__imp__XamShowSigninUI result=closed-local-profile\n";
    ctx.r3.u64 = 0;
}

// Xbox Live privileges (multiplayer, communication, content): a local profile
// without Live has none. r5 = BOOL result.
PPC_FUNC(__imp__XamUserCheckPrivilege) {
    RuntimeTraceImport("XamUserCheckPrivilege", ctx);
    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t result = ctx.r5.u32;
    constexpr uint32_t kErrorInvalidParameter = 87;
    constexpr uint32_t kErrorNoSuchUser = 0x525;
    if (userIndex != 0xFF && userIndex >= 4) {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    if (userIndex != 0xFF && userIndex != 0) {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    if (result) PPC_STORE_U32(result, 0);
    std::cout << "IMPORT_CALL name=__imp__XamUserCheckPrivilege user=" << userIndex
              << " privilege=" << ctx.r4.u32 << " granted=0\n";
    ctx.r3.u64 = 0;
}

// Another title (the dashboard, the marketplace) replaces this one: on a
// console the game ends here, and so it does here (a clean stop).
PPC_FUNC(__imp__XamLoaderLaunchTitle) {
    RuntimeTraceImport("XamLoaderLaunchTitle", ctx);
    const std::string path =
        ctx.r3.u32 ? std::string(reinterpret_cast<const char*>(base + ctx.r3.u32),
                                 strnlen(reinterpret_cast<const char*>(base + ctx.r3.u32), 260))
                   : std::string("(dashboard)");
    std::cerr << "GUEST_LAUNCH_TITLE path=" << path << " flags=0x" << std::hex << ctx.r4.u32
              << " lr=0x" << ctx.lr << std::dec << '\n';
    RequestGuestRuntimeStop();
    throw GuestRuntimeStop{};
}

// Data for the next title: none is ever launched from here.
PPC_FUNC(__imp__XamLoaderSetLaunchData) {
    RuntimeTraceImport("XamLoaderSetLaunchData", ctx);
    ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------
// Offline network and Xbox LIVE services (V433). Multiplayer > Quick Player
// Match started the title's network layer and its first call stopped the game
// (whole-game audit, V432 menu run). The runtime now answers as a console with
// a network adapter but no cable, no headset, and a local profile that is
// never signed in to Xbox LIVE: the network stack starts, sockets open and
// bind locally, nothing is ever received and no connection succeeds, and Xbox
// LIVE services fail as they do offline. Nothing reaches the host network.
namespace {
constexpr uint32_t kSocketError = 0xFFFFFFFFu;
constexpr uint32_t kWsaEFault = 10014;
constexpr uint32_t kWsaEInval = 10022;
constexpr uint32_t kWsaEWouldBlock = 10035;
constexpr uint32_t kWsaENotSock = 10038;
constexpr uint32_t kWsaEProtoNoSupport = 10043;
constexpr uint32_t kWsaEOpNotSupp = 10045;
constexpr uint32_t kWsaEAfNoSupport = 10047;
constexpr uint32_t kWsaENetDown = 10050;
constexpr uint32_t kWsaENotConn = 10057;
constexpr uint32_t kWsaVerNotSupported = 10092;
constexpr uint32_t kIoctlNonBlocking = 0x8004667Eu;  // FIONBIO
constexpr uint32_t kIoctlBytesReadable = 0x4004667Fu;  // FIONREAD
// XNET_GET_XNADDR_ETHERNET: the adapter has its link-layer address, no IP.
constexpr uint32_t kXnAddrEthernet = 0x00000002u;
// Locally administered unicast address for the offline adapter (never sent).
constexpr uint8_t kOfflineAdapterAddress[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
// The local profile (slot 0): an offline XUID and the gamertag the runtime
// reports for it. Both stay inside this process.
constexpr uint64_t kLocalProfileXuid = 0xE000000000000001ull;
constexpr char kLocalProfileName[] = "Player";
constexpr uint32_t kHresultFail = 0x80004005u;  // E_FAIL
constexpr uint32_t kHresultInvalidArg = 0x80070057u;
constexpr uint32_t kHresultNoSuchUser = 0x80070525u;
constexpr uint32_t kHresultOutOfMemory = 0x8007000Eu;
constexpr uint32_t kStatusDllNotFound = 0xC0000135u;
constexpr uint32_t kXErrorInvalidHandle = 0x00000006u;

thread_local uint32_t networkLastError = 0;

uint32_t NetworkFailure(uint32_t error) {
    networkLastError = error;
    return kSocketError;
}

struct OfflineSocket {
    uint32_t type{};  // 1 stream, 2 datagram
    uint16_t port{};
    bool nonBlocking{};
    bool listening{};
    bool connected{};  // datagram default peer only; streams never connect
};

// Sockets of the offline adapter: local state only, no host sockets.
class OfflineSockets {
public:
    uint32_t Open(uint32_t type) {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint32_t handle = nextHandle_;
        nextHandle_ += 4;
        sockets_[handle] = OfflineSocket{type};
        return handle;
    }
    bool Close(uint32_t handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        return sockets_.erase(handle) != 0;
    }
    bool Get(uint32_t handle, OfflineSocket* socket) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sockets_.find(handle);
        if (it == sockets_.end()) return false;
        if (socket) *socket = it->second;
        return true;
    }
    template <typename Change> bool Update(uint32_t handle, Change update) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sockets_.find(handle);
        if (it == sockets_.end()) return false;
        update(it->second);
        return true;
    }
    uint16_t EphemeralPort() {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint16_t port = nextPort_;
        nextPort_ = nextPort_ == 65535 ? 49152 : uint16_t(nextPort_ + 1);
        return port;
    }

private:
    std::mutex mutex_;
    std::unordered_map<uint32_t, OfflineSocket> sockets_;
    uint32_t nextHandle_ = 0x5C0C0004u;
    uint16_t nextPort_ = 49152;
};

OfflineSockets& GetOfflineSockets() {
    static OfflineSockets sockets;
    return sockets;
}

// Nothing ever arrives on the offline adapter: a blocking receive, accept or
// select without a timeout waits like a console with no traffic, until the
// runtime stops.
[[noreturn]] void WaitForeverOffline() {
    for (;;) {
        if (GuestRuntimeStopRequested()) throw GuestRuntimeStop{};
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void WaitOffline(std::chrono::microseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    for (;;) {
        if (GuestRuntimeStopRequested()) throw GuestRuntimeStop{};
        const auto now = std::chrono::steady_clock::now();
        if (now >= end) return;
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
            end - now, std::chrono::milliseconds(50)));
    }
}

// One line per entry point and value (a status or a message id): enough
// evidence without flooding the log.
void LogOfflineCall(const char* name, uint32_t value) {
    static std::mutex mutex;
    static std::unordered_set<std::string> logged;
    std::string key = std::string(name) + ':' + std::to_string(value);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!logged.insert(key).second) return;
    }
    std::cout << "IMPORT_CALL name=__imp__" << name << " offline=1 value=0x" << std::hex
              << value << std::dec << '\n';
}

bool ValidGuestRange(uint32_t address, uint32_t bytes) {
    return address && bytes <= UINT32_MAX - address;
}

void StoreGuestName(uint8_t* base, uint32_t buffer, uint32_t bytes) {
    if (!ValidGuestRange(buffer, bytes) || !bytes) return;
    const uint32_t count = std::min<uint32_t>(bytes - 1, sizeof(kLocalProfileName) - 1);
    for (uint32_t index = 0; index < bytes; ++index) {
        PPC_STORE_U8(buffer + index, index < count ? uint8_t(kLocalProfileName[index]) : 0);
    }
}

// XAM heap for the title's library (XamAlloc): whole pages from the physical
// backing model, zero-filled.
uint32_t AllocateXamPages(uint8_t* base, uint32_t bytes) {
    if (!bytes || bytes > 64u * 1024u * 1024u) return 0;
    const uint32_t pages = (bytes + GuestMemoryAccounting::kPageSize - 1) /
                           GuestMemoryAccounting::kPageSize;
    uint32_t firstPage{};
    if (!GetGuestMemoryAccounting().ReservePhysicalPages(
            pages, 1, 0, GuestMemoryAccounting::kPhysicalPages - 1, kPageReadWrite, &firstPage)) {
        return 0;
    }
    const uint32_t address = kPhysicalGuestBase + firstPage * GuestMemoryAccounting::kPageSize;
    RuntimeGeneratedMemset(base, base + address, 0, size_t(pages) * GuestMemoryAccounting::kPageSize,
                           __FILE__, __LINE__);
    return address;
}

bool FreeXamPages(uint32_t address) {
    if (address < kPhysicalGuestBase || address >= 0xC0000000u ||
        (address - kPhysicalGuestBase) % GuestMemoryAccounting::kPageSize) {
        return false;
    }
    return GetGuestMemoryAccounting().ReleasePhysicalPages(
        (address - kPhysicalGuestBase) / GuestMemoryAccounting::kPageSize);
}
}  // namespace

// --- Winsock (every NetDll entry takes the XNet caller id in r3) ----------

PPC_FUNC(__imp__NetDll_WSAStartup) {
    RuntimeTraceImport("NetDll_WSAStartup", ctx);
    const uint16_t requested = static_cast<uint16_t>(ctx.r4.u32);
    const uint32_t data = ctx.r5.u32;
    const uint8_t major = requested & 0xFF;
    const uint8_t minor = requested >> 8;
    if (major < 1) {
        ctx.r3.u64 = kWsaVerNotSupported;
        return;
    }
    // WSADATA: wVersion, wHighVersion, szDescription[257], szSystemStatus[129],
    // iMaxSockets, iMaxUdpDg (Winsock 2: unused), lpVendorInfo (left as is).
    const uint16_t version = (major > 2 || (major == 2 && minor > 2)) ? uint16_t(0x0202) : requested;
    if (data && ValidGuestRange(data, 0x18C)) {
        PPC_STORE_U16(data + 0, version);
        PPC_STORE_U16(data + 2, 0x0202);
        PPC_STORE_U8(data + 4, 0);
        PPC_STORE_U8(data + 4 + 257, 0);
        PPC_STORE_U16(data + 0x186, 0);
        PPC_STORE_U16(data + 0x188, 0);
    }
    ctx.r3.u64 = 0;
    std::cout << "IMPORT_CALL name=__imp__NetDll_WSAStartup requested=0x" << std::hex
              << requested << " version=0x" << version << std::dec
              << " adapter=offline status=0x0\n";
}

PPC_FUNC(__imp__NetDll_WSACleanup) {
    RuntimeTraceImport("NetDll_WSACleanup", ctx);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_WSAGetLastError) {
    RuntimeTraceImport("NetDll_WSAGetLastError", ctx);
    ctx.r3.u64 = networkLastError;
}

PPC_FUNC(__imp__NetDll_socket) {
    RuntimeTraceImport("NetDll_socket", ctx);
    const uint32_t family = ctx.r4.u32;
    const uint32_t type = ctx.r5.u32;
    const uint32_t protocol = ctx.r6.u32;
    constexpr uint32_t kInet = 2, kStream = 1, kDatagram = 2;
    constexpr uint32_t kTcp = 6, kUdp = 17, kVdp = 254;
    uint32_t result;
    if (family != kInet) {
        result = NetworkFailure(kWsaEAfNoSupport);
    } else if (type == kStream && (protocol == 0 || protocol == kTcp)) {
        result = GetOfflineSockets().Open(type);
    } else if (type == kDatagram && (protocol == 0 || protocol == kUdp || protocol == kVdp)) {
        result = GetOfflineSockets().Open(type);
    } else {
        result = NetworkFailure(kWsaEProtoNoSupport);
    }
    ctx.r3.u64 = result;
    std::cout << "IMPORT_CALL name=__imp__NetDll_socket family=" << family << " type=" << type
              << " protocol=" << protocol << " socket=0x" << std::hex << result << std::dec
              << '\n';
}

PPC_FUNC(__imp__NetDll_closesocket) {
    RuntimeTraceImport("NetDll_closesocket", ctx);
    ctx.r3.u64 = GetOfflineSockets().Close(ctx.r4.u32) ? 0u : NetworkFailure(kWsaENotSock);
}

PPC_FUNC(__imp__NetDll_bind) {
    RuntimeTraceImport("NetDll_bind", ctx);
    const uint32_t handle = ctx.r4.u32;
    const uint32_t address = ctx.r5.u32;
    const uint32_t length = ctx.r6.u32;
    if (!GetOfflineSockets().Get(handle, nullptr)) {
        ctx.r3.u64 = NetworkFailure(kWsaENotSock);
        return;
    }
    if (!ValidGuestRange(address, 16) || length < 16) {
        ctx.r3.u64 = NetworkFailure(kWsaEFault);
        return;
    }
    if (PPC_LOAD_U16(address + 0) != 2) {
        ctx.r3.u64 = NetworkFailure(kWsaEAfNoSupport);
        return;
    }
    uint16_t port = PPC_LOAD_U16(address + 2);
    if (!port) port = GetOfflineSockets().EphemeralPort();
    GetOfflineSockets().Update(handle, [port](OfflineSocket& socket) { socket.port = port; });
    ctx.r3.u64 = 0;
    std::cout << "IMPORT_CALL name=__imp__NetDll_bind socket=0x" << std::hex << handle
              << std::dec << " port=" << port << " status=0x0\n";
}

PPC_FUNC(__imp__NetDll_ioctlsocket) {
    RuntimeTraceImport("NetDll_ioctlsocket", ctx);
    const uint32_t handle = ctx.r4.u32;
    const uint32_t command = ctx.r5.u32;
    const uint32_t argument = ctx.r6.u32;
    if (!GetOfflineSockets().Get(handle, nullptr)) {
        ctx.r3.u64 = NetworkFailure(kWsaENotSock);
        return;
    }
    if (!ValidGuestRange(argument, 4)) {
        ctx.r3.u64 = NetworkFailure(kWsaEFault);
        return;
    }
    if (command == kIoctlNonBlocking) {
        const bool nonBlocking = PPC_LOAD_U32(argument) != 0;
        GetOfflineSockets().Update(handle, [nonBlocking](OfflineSocket& socket) {
            socket.nonBlocking = nonBlocking;
        });
        ctx.r3.u64 = 0;
    } else if (command == kIoctlBytesReadable) {
        PPC_STORE_U32(argument, 0);
        ctx.r3.u64 = 0;
    } else {
        ctx.r3.u64 = NetworkFailure(kWsaEInval);
    }
}

PPC_FUNC(__imp__NetDll_setsockopt) {
    RuntimeTraceImport("NetDll_setsockopt", ctx);
    ctx.r3.u64 = GetOfflineSockets().Get(ctx.r4.u32, nullptr) ? 0u : NetworkFailure(kWsaENotSock);
}

PPC_FUNC(__imp__NetDll_getpeername) {
    RuntimeTraceImport("NetDll_getpeername", ctx);
    OfflineSocket socket;
    ctx.r3.u64 = GetOfflineSockets().Get(ctx.r4.u32, &socket) ? NetworkFailure(kWsaENotConn)
                                                            : NetworkFailure(kWsaENotSock);
}

// No link: a stream connection fails; a datagram socket only records its
// default peer, as it does on any adapter.
PPC_FUNC(__imp__NetDll_connect) {
    RuntimeTraceImport("NetDll_connect", ctx);
    const uint32_t handle = ctx.r4.u32;
    OfflineSocket socket;
    if (!GetOfflineSockets().Get(handle, &socket)) {
        ctx.r3.u64 = NetworkFailure(kWsaENotSock);
    } else if (socket.type == 2) {
        GetOfflineSockets().Update(handle, [](OfflineSocket& s) { s.connected = true; });
        ctx.r3.u64 = 0;
    } else {
        ctx.r3.u64 = NetworkFailure(kWsaENetDown);
    }
    LogOfflineCall("NetDll_connect", ctx.r3.u32);
}

PPC_FUNC(__imp__NetDll_listen) {
    RuntimeTraceImport("NetDll_listen", ctx);
    const uint32_t handle = ctx.r4.u32;
    OfflineSocket socket;
    if (!GetOfflineSockets().Get(handle, &socket)) {
        ctx.r3.u64 = NetworkFailure(kWsaENotSock);
    } else if (socket.type != 1) {
        ctx.r3.u64 = NetworkFailure(kWsaEOpNotSupp);
    } else {
        GetOfflineSockets().Update(handle, [](OfflineSocket& s) { s.listening = true; });
        ctx.r3.u64 = 0;
    }
}

PPC_FUNC(__imp__NetDll_accept) {
    RuntimeTraceImport("NetDll_accept", ctx);
    OfflineSocket socket;
    if (!GetOfflineSockets().Get(ctx.r4.u32, &socket)) {
        ctx.r3.u64 = NetworkFailure(kWsaENotSock);
    } else if (!socket.listening) {
        ctx.r3.u64 = NetworkFailure(kWsaEInval);
    } else if (socket.nonBlocking) {
        ctx.r3.u64 = NetworkFailure(kWsaEWouldBlock);
    } else {
        WaitForeverOffline();
    }
}

PPC_FUNC(__imp__NetDll_recv) {
    RuntimeTraceImport("NetDll_recv", ctx);
    OfflineSocket socket;
    if (!GetOfflineSockets().Get(ctx.r4.u32, &socket)) {
        ctx.r3.u64 = NetworkFailure(kWsaENotSock);
    } else if (socket.type == 1 || !socket.connected) {
        ctx.r3.u64 = NetworkFailure(kWsaENotConn);
    } else if (socket.nonBlocking) {
        ctx.r3.u64 = NetworkFailure(kWsaEWouldBlock);
    } else {
        WaitForeverOffline();
    }
}

PPC_FUNC(__imp__NetDll_recvfrom) {
    RuntimeTraceImport("NetDll_recvfrom", ctx);
    OfflineSocket socket;
    if (!GetOfflineSockets().Get(ctx.r4.u32, &socket)) {
        ctx.r3.u64 = NetworkFailure(kWsaENotSock);
    } else if (socket.type == 1) {
        ctx.r3.u64 = NetworkFailure(kWsaENotConn);
    } else if (!socket.port) {
        ctx.r3.u64 = NetworkFailure(kWsaEInval);
    } else if (socket.nonBlocking) {
        ctx.r3.u64 = NetworkFailure(kWsaEWouldBlock);
    } else {
        WaitForeverOffline();
    }
}

PPC_FUNC(__imp__NetDll_send) {
    RuntimeTraceImport("NetDll_send", ctx);
    OfflineSocket socket;
    const uint32_t length = ctx.r6.u32;
    if (!GetOfflineSockets().Get(ctx.r4.u32, &socket)) {
        ctx.r3.u64 = NetworkFailure(kWsaENotSock);
    } else if (socket.type == 1 || !socket.connected) {
        ctx.r3.u64 = NetworkFailure(kWsaENotConn);
    } else {
        ctx.r3.u64 = length;  // the datagram leaves no cable
    }
}

// A datagram (System Link discovery, game traffic) is accepted and goes
// nowhere: with no cable nothing is sent and nothing answers.
PPC_FUNC(__imp__NetDll_sendto) {
    RuntimeTraceImport("NetDll_sendto", ctx);
    OfflineSocket socket;
    const uint32_t handle = ctx.r4.u32;
    const uint32_t buffer = ctx.r5.u32;
    const uint32_t length = ctx.r6.u32;
    if (!GetOfflineSockets().Get(handle, &socket)) {
        ctx.r3.u64 = NetworkFailure(kWsaENotSock);
    } else if (socket.type == 1) {
        ctx.r3.u64 = NetworkFailure(kWsaENotConn);
    } else if (length && !ValidGuestRange(buffer, length)) {
        ctx.r3.u64 = NetworkFailure(kWsaEFault);
    } else {
        if (!socket.port) {
            const uint16_t port = GetOfflineSockets().EphemeralPort();
            GetOfflineSockets().Update(handle, [port](OfflineSocket& s) { s.port = port; });
        }
        ctx.r3.u64 = length;
    }
    LogOfflineCall("NetDll_sendto", ctx.r3.u32 == kSocketError ? networkLastError : 0u);
}

// fd_set: count, then up to 64 socket handles. No socket ever becomes
// readable or raises an exception; datagram sockets are writable.
PPC_FUNC(__imp__NetDll_select) {
    RuntimeTraceImport("NetDll_select", ctx);
    const uint32_t sets[3] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
    const uint32_t timeout = ctx.r8.u32;
    uint32_t ready = 0;
    for (uint32_t kind = 0; kind < 3; ++kind) {
        const uint32_t set = sets[kind];
        if (!set) continue;
        if (!ValidGuestRange(set, 4 + 64 * 4)) {
            ctx.r3.u64 = NetworkFailure(kWsaEFault);
            return;
        }
        const uint32_t count = PPC_LOAD_U32(set);
        if (count > 64) {
            ctx.r3.u64 = NetworkFailure(kWsaEInval);
            return;
        }
        uint32_t kept = 0;
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t handle = PPC_LOAD_U32(set + 4 + index * 4);
            OfflineSocket socket;
            if (!GetOfflineSockets().Get(handle, &socket)) {
                ctx.r3.u64 = NetworkFailure(kWsaENotSock);
                return;
            }
            if (kind == 1 && socket.type == 2) {
                PPC_STORE_U32(set + 4 + kept * 4, handle);
                ++kept;
            }
        }
        PPC_STORE_U32(set, kept);
        ready += kept;
    }
    if (!ready) {
        if (!timeout) WaitForeverOffline();
        if (!ValidGuestRange(timeout, 8)) {
            ctx.r3.u64 = NetworkFailure(kWsaEFault);
            return;
        }
        const int64_t seconds = static_cast<int32_t>(PPC_LOAD_U32(timeout + 0));
        const int64_t micros = static_cast<int32_t>(PPC_LOAD_U32(timeout + 4));
        const int64_t total = std::max<int64_t>(0, seconds * 1'000'000 + micros);
        WaitOffline(std::chrono::microseconds(std::min<int64_t>(total, 3'600'000'000LL)));
    }
    ctx.r3.u64 = ready;
}

PPC_FUNC(__imp__NetDll___WSAFDIsSet) {
    RuntimeTraceImport("NetDll___WSAFDIsSet", ctx);
    const uint32_t handle = ctx.r3.u32;
    const uint32_t set = ctx.r4.u32;
    uint32_t found = 0;
    if (ValidGuestRange(set, 4 + 64 * 4)) {
        const uint32_t count = std::min<uint32_t>(PPC_LOAD_U32(set), 64);
        for (uint32_t index = 0; index < count && !found; ++index) {
            found = PPC_LOAD_U32(set + 4 + index * 4) == handle ? 1u : 0u;
        }
    }
    ctx.r3.u64 = found;
}

// No network event ever occurs on the offline adapter.
PPC_FUNC(__imp__NetDll_WSAEventSelect) {
    RuntimeTraceImport("NetDll_WSAEventSelect", ctx);
    const uint32_t handle = ctx.r4.u32;
    if (!GetOfflineSockets().Update(handle, [](OfflineSocket& s) { s.nonBlocking = true; })) {
        ctx.r3.u64 = NetworkFailure(kWsaENotSock);
        return;
    }
    ctx.r3.u64 = 0;
}

// --- XNet ------------------------------------------------------------------

PPC_FUNC(__imp__NetDll_XNetStartup) {
    RuntimeTraceImport("NetDll_XNetStartup", ctx);
    ctx.r3.u64 = 0;
    std::cout << "IMPORT_CALL name=__imp__NetDll_XNetStartup adapter=offline status=0x0\n";
}

// XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20] (36 bytes).
// No cable: the adapter has its link-layer address and no IP address.
PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr) {
    RuntimeTraceImport("NetDll_XNetGetTitleXnAddr", ctx);
    const uint32_t address = ctx.r4.u32;
    if (!ValidGuestRange(address, 36)) {
        ctx.r3.u64 = 0x00000001u;  // XNET_GET_XNADDR_NONE
        return;
    }
    for (uint32_t offset = 0; offset < 36; ++offset) PPC_STORE_U8(address + offset, 0);
    for (uint32_t index = 0; index < 6; ++index) {
        PPC_STORE_U8(address + 10 + index, kOfflineAdapterAddress[index]);
    }
    ctx.r3.u64 = kXnAddrEthernet;
    LogOfflineCall("NetDll_XNetGetTitleXnAddr", kXnAddrEthernet);
}

// Machine ids come from the online part of an address; there is none.
PPC_FUNC(__imp__NetDll_XNetXnAddrToMachineId) {
    RuntimeTraceImport("NetDll_XNetXnAddrToMachineId", ctx);
    const uint32_t output = ctx.r5.u32;
    if (ValidGuestRange(output, 8)) PPC_STORE_U64(output, 0);
    ctx.r3.u64 = kWsaEInval;
    LogOfflineCall("NetDll_XNetXnAddrToMachineId", kWsaEInval);
}

PPC_FUNC(__imp__NetDll_XNetXnAddrToInAddr) {
    RuntimeTraceImport("NetDll_XNetXnAddrToInAddr", ctx);
    const uint32_t output = ctx.r6.u32;
    if (ValidGuestRange(output, 4)) PPC_STORE_U32(output, 0);
    ctx.r3.u64 = kWsaENetDown;
    LogOfflineCall("NetDll_XNetXnAddrToInAddr", kWsaENetDown);
}

PPC_FUNC(__imp__NetDll_XNetInAddrToXnAddr) {
    RuntimeTraceImport("NetDll_XNetInAddrToXnAddr", ctx);
    ctx.r3.u64 = kWsaEInval;
    LogOfflineCall("NetDll_XNetInAddrToXnAddr", kWsaEInval);
}

// Session keys are local cryptography: a random key id (system link kind,
// flag bits clear) and key exchange key.
PPC_FUNC(__imp__NetDll_XNetCreateKey) {
    RuntimeTraceImport("NetDll_XNetCreateKey", ctx);
    const uint32_t keyId = ctx.r4.u32;
    const uint32_t key = ctx.r5.u32;
    if (!ValidGuestRange(keyId, 8) || !ValidGuestRange(key, 16)) {
        ctx.r3.u64 = kWsaEFault;
        return;
    }
    uint8_t bytes[24]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("NetDll_XNetCreateKey host RNG failed");
    bytes[0] &= 0x0F;
    for (uint32_t index = 0; index < 8; ++index) PPC_STORE_U8(keyId + index, bytes[index]);
    for (uint32_t index = 0; index < 16; ++index) PPC_STORE_U8(key + index, bytes[8 + index]);
    ctx.r3.u64 = 0;
    LogOfflineCall("NetDll_XNetCreateKey", 0);
}

PPC_FUNC(__imp__NetDll_XNetRegisterKey) {
    RuntimeTraceImport("NetDll_XNetRegisterKey", ctx);
    ctx.r3.u64 = 0;
    LogOfflineCall("NetDll_XNetRegisterKey", 0);
}

PPC_FUNC(__imp__NetDll_XNetUnregisterKey) {
    RuntimeTraceImport("NetDll_XNetUnregisterKey", ctx);
    ctx.r3.u64 = 0;
}

// A host's quality-of-service listener: nobody can probe it.
PPC_FUNC(__imp__NetDll_XNetQosListen) {
    RuntimeTraceImport("NetDll_XNetQosListen", ctx);
    ctx.r3.u64 = 0;
    LogOfflineCall("NetDll_XNetQosListen", 0);
}

// Probing other consoles needs the network. r3 caller, r4-r10 the first seven
// arguments; cProbes, dwBitsPerSec, dwFlags, hEvent and the XNQOS** result
// follow on the stack (+0x54 .. +0x74).
PPC_FUNC(__imp__NetDll_XNetQosLookup) {
    RuntimeTraceImport("NetDll_XNetQosLookup", ctx);
    const uint32_t result = ctx.r1.u32 <= UINT32_MAX - 0x78u ? PPC_LOAD_U32(ctx.r1.u32 + 0x74u) : 0;
    if (ValidGuestRange(result, 4)) PPC_STORE_U32(result, 0);
    ctx.r3.u64 = kWsaENetDown;
    LogOfflineCall("NetDll_XNetQosLookup", kWsaENetDown);
}

PPC_FUNC(__imp__NetDll_XNetQosRelease) {
    RuntimeTraceImport("NetDll_XNetQosRelease", ctx);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_XNetDnsLookup) {
    RuntimeTraceImport("NetDll_XNetDnsLookup", ctx);
    const uint32_t result = ctx.r6.u32;
    if (ValidGuestRange(result, 4)) PPC_STORE_U32(result, 0);
    ctx.r3.u64 = kWsaENetDown;
    LogOfflineCall("NetDll_XNetDnsLookup", kWsaENetDown);
}

PPC_FUNC(__imp__NetDll_XNetDnsRelease) {
    RuntimeTraceImport("NetDll_XNetDnsRelease", ctx);
    ctx.r3.u64 = 0;
}

// The title's XNet library looks up the Ex entry points in xam.xex by ordinal
// and falls back to its own imports when the module is not found. The runtime
// provides system calls only through the title's import table.
PPC_FUNC(__imp__XexGetModuleHandle) {
    RuntimeTraceImport("XexGetModuleHandle", ctx);
    const std::string name = SafeGuestCString(base, ctx.r3.u32, 64);
    if (ValidGuestRange(ctx.r4.u32, 4)) PPC_STORE_U32(ctx.r4.u32, 0);
    ctx.r3.u64 = kStatusDllNotFound;
    std::cout << "IMPORT_CALL name=__imp__XexGetModuleHandle module=" << name
              << " status=0x" << std::hex << kStatusDllNotFound << std::dec << '\n';
}

PPC_FUNC(__imp__XexGetProcedureAddress) {
    RuntimeTraceImport("XexGetProcedureAddress", ctx);
    if (ValidGuestRange(ctx.r5.u32, 4)) PPC_STORE_U32(ctx.r5.u32, 0);
    ctx.r3.u64 = kStatusInvalidHandle;
}

// --- The local profile and Xbox LIVE -----------------------------------------

PPC_FUNC(__imp__XamUserGetXUID) {
    RuntimeTraceImport("XamUserGetXUID", ctx);
    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t typeMask = ctx.r4.u32;
    const uint32_t output = ctx.r5.u32;
    uint32_t result = 0;
    uint64_t xuid = 0;
    if (userIndex >= 4 || !ValidGuestRange(output, 8)) {
        result = kHresultInvalidArg;
    } else if (userIndex != 0) {
        result = kHresultNoSuchUser;
    } else if (typeMask & 1u) {
        xuid = kLocalProfileXuid;  // offline XUID; there is no online one
    } else {
        result = kHresultNoSuchUser;
    }
    if (ValidGuestRange(output, 8)) PPC_STORE_U64(output, xuid);
    ctx.r3.u64 = result;
    LogOfflineCall("XamUserGetXUID", result);
}

PPC_FUNC(__imp__XamUserGetName) {
    RuntimeTraceImport("XamUserGetName", ctx);
    const uint32_t userIndex = ctx.r3.u32;
    if (userIndex >= 4) {
        ctx.r3.u64 = kHresultInvalidArg;
    } else if (userIndex != 0) {
        ctx.r3.u64 = kHresultNoSuchUser;
    } else {
        StoreGuestName(base, ctx.r4.u32, std::min<uint32_t>(ctx.r5.u32, 16));
        ctx.r3.u64 = 0;
    }
    LogOfflineCall("XamUserGetName", ctx.r3.u32);
}

// XUSER_SIGNIN_INFO: xuid, dwInfoFlags, state, dwGuestNumber,
// dwSponsorUserIndex, szUserName[16] (40 bytes). Flag 2 asks for the online
// XUID only, which a local profile does not have.
PPC_FUNC(__imp__XamUserGetSigninInfo) {
    RuntimeTraceImport("XamUserGetSigninInfo", ctx);
    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t flags = ctx.r4.u32;
    const uint32_t info = ctx.r5.u32;
    if (!ValidGuestRange(info, 40)) {
        ctx.r3.u64 = kHresultInvalidArg;
        return;
    }
    for (uint32_t offset = 0; offset < 40; offset += 4) PPC_STORE_U32(info + offset, 0);
    if (userIndex != 0) {
        ctx.r3.u64 = userIndex >= 4 ? kHresultInvalidArg : kHresultNoSuchUser;
        return;
    }
    PPC_STORE_U64(info + 0, (flags & 2u) ? 0 : kLocalProfileXuid);
    PPC_STORE_U32(info + 12, 1);  // signed in locally
    StoreGuestName(base, info + 24, 16);
    ctx.r3.u64 = 0;
    LogOfflineCall("XamUserGetSigninInfo", 0);
}

// A local profile has no friends list: nobody is a friend.
PPC_FUNC(__imp__XamUserAreUsersFriends) {
    RuntimeTraceImport("XamUserAreUsersFriends", ctx);
    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t result = ctx.r6.u32;
    const uint32_t overlapped = ctx.r7.u32;
    uint32_t status = 0;
    if (userIndex >= 4) status = kXErrorInvalidParameter;
    else if (userIndex != 0) status = kXErrorNoSuchUser;
    if (!status && ValidGuestRange(result, 4)) PPC_STORE_U32(result, 0);
    if (overlapped) {
        CompleteXamOverlappedImmediate(base, overlapped, status);
        ctx.r3.u64 = kXErrorIoPending;
    } else {
        ctx.r3.u64 = status;
    }
    LogOfflineCall("XamUserAreUsersFriends", status);
}

// Leaderboards are Xbox LIVE statistics (as XUserReadStats, V380).
PPC_FUNC(__imp__XamUserCreateStatsEnumerator) {
    RuntimeTraceImport("XamUserCreateStatsEnumerator", ctx);
    ctx.r3.u64 = kXErrorFunctionFailed;
    LogOfflineCall("XamUserCreateStatsEnumerator", kXErrorFunctionFailed);
}

PPC_FUNC(__imp__XamWriteGamerTile) {
    RuntimeTraceImport("XamWriteGamerTile", ctx);
    const uint32_t overlapped = ctx.r8.u32;
    if (overlapped) {
        CompleteXamOverlappedImmediate(base, overlapped, 0);
        ctx.r3.u64 = kXErrorIoPending;
    } else {
        ctx.r3.u64 = 0;
    }
    LogOfflineCall("XamWriteGamerTile", 0);
}

// No headset: the voice port stays empty (pinned ReXGlue/Xenia contract: a
// null voice object and a positive, non-HRESULT status the voice library
// accepts); the headset is never present.
PPC_FUNC(__imp__XamVoiceCreate) {
    RuntimeTraceImport("XamVoiceCreate", ctx);
    if (ValidGuestRange(ctx.r5.u32, 4)) PPC_STORE_U32(ctx.r5.u32, 0);
    ctx.r3.u64 = kXErrorAccessDenied;
    LogOfflineCall("XamVoiceCreate", kXErrorAccessDenied);
}

PPC_FUNC(__imp__XamVoiceHeadsetPresent) {
    RuntimeTraceImport("XamVoiceHeadsetPresent", ctx);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XamVoiceClose) {
    RuntimeTraceImport("XamVoiceClose", ctx);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XamVoiceSubmitPacket) {
    RuntimeTraceImport("XamVoiceSubmitPacket", ctx);
    ctx.r3.u64 = kHresultFail;
    LogOfflineCall("XamVoiceSubmitPacket", kHresultFail);
}

// XSession handles are kernel handles the title closes with CloseHandle. The
// session messages themselves fail as offline (XMsgStartIORequest, V380), so
// the object only has to exist: a non-signalled event stands in for it.
PPC_FUNC(__imp__XamSessionCreateHandle) {
    RuntimeTraceImport("XamSessionCreateHandle", ctx);
    const uint32_t output = ctx.r3.u32;
    if (!ValidGuestRange(output, 4)) {
        ctx.r3.u64 = kXErrorInvalidParameter;
        return;
    }
    const auto session = GetGuestObjects().CreateEvent(true, false);
    PPC_STORE_U32(output, session.handle);
    ctx.r3.u64 = 0;
    std::cout << "IMPORT_CALL name=__imp__XamSessionCreateHandle handle=0x" << std::hex
              << session.handle << std::dec << '\n';
}

PPC_FUNC(__imp__XamSessionRefObjByHandle) {
    RuntimeTraceImport("XamSessionRefObjByHandle", ctx);
    const uint32_t handle = ctx.r3.u32;
    const uint32_t output = ctx.r4.u32;
    uint32_t object{};
    const uint32_t status =
        GetGuestObjects().ReferenceByHandle(handle, 0, CurrentGuestThreadObject(), &object);
    if (ValidGuestRange(output, 4)) PPC_STORE_U32(output, status ? 0u : object);
    ctx.r3.u64 = status ? kXErrorInvalidHandle : 0u;
}

// Synchronous XAM messages. The title sends Xbox LIVE base messages (app
// 0xFC: logon id 0x58004, 0x5800E, friends 0x58020, game invites 0x58023),
// which fail offline, and the music player's XMPGetPlaybackController
// (0xFA/0x7001B: {client, controller*, locked*}), answered as the pinned
// ReXGlue app does (controller 0, not locked).
PPC_FUNC(__imp__XMsgInProcessCall) {
    RuntimeTraceImport("XMsgInProcessCall", ctx);
    const uint32_t app = ctx.r3.u32;
    const uint32_t message = ctx.r4.u32;
    const uint32_t buffer = ctx.r5.u32;
    if (app == 0xFCu) {
        ctx.r3.u64 = kHresultFail;
        LogOfflineCall("XMsgInProcessCall(0xFC)", message);
        return;
    }
    if (app == 0xFAu && message == 0x0007001Bu && ValidGuestRange(buffer, 12) &&
        PPC_LOAD_U32(buffer) == 2) {
        const uint32_t controller = PPC_LOAD_U32(buffer + 4);
        const uint32_t locked = PPC_LOAD_U32(buffer + 8);
        if (ValidGuestRange(controller, 4)) PPC_STORE_U32(controller, 0);
        if (ValidGuestRange(locked, 4)) PPC_STORE_U32(locked, 0);
        ctx.r3.u64 = 0;
        LogOfflineCall("XMsgInProcessCall(XMPGetPlaybackController)", 0);
        return;
    }
    std::ostringstream detail;
    detail << "XMsgInProcessCall reached an unverified XAM message: app=0x" << std::hex << app
           << " message=0x" << message;
    throw std::runtime_error(detail.str());
}

// Every XAM request here completes before it returns: nothing is pending.
PPC_FUNC(__imp__XMsgCancelIORequest) {
    RuntimeTraceImport("XMsgCancelIORequest", ctx);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XamAlloc) {
    RuntimeTraceImport("XamAlloc", ctx);
    const uint32_t bytes = ctx.r4.u32;
    const uint32_t output = ctx.r5.u32;
    if (!ValidGuestRange(output, 4)) {
        ctx.r3.u64 = kHresultInvalidArg;
        return;
    }
    const uint32_t address = AllocateXamPages(base, bytes);
    PPC_STORE_U32(output, address);
    ctx.r3.u64 = address ? 0u : kHresultOutOfMemory;
    LogOfflineCall("XamAlloc", ctx.r3.u32);
}

PPC_FUNC(__imp__XamFree) {
    RuntimeTraceImport("XamFree", ctx);
    ctx.r3.u64 = (!ctx.r3.u32 || FreeXamPages(ctx.r3.u32)) ? 0u : kHresultInvalidArg;
}

// --- Kernel objects reached by the voice engine --------------------------

// Options bit 0 is DUPLICATE_CLOSE_SOURCE.
PPC_FUNC(__imp__NtDuplicateObject) {
    RuntimeTraceImport("NtDuplicateObject", ctx);
    const uint32_t handle = ctx.r3.u32;
    const uint32_t output = ctx.r4.u32;
    const uint32_t options = ctx.r5.u32;
    uint32_t duplicate{};
    const uint32_t status = GetGuestObjects().DuplicateHandle(
        handle, CurrentGuestThreadObject(), (options & 1u) != 0, &duplicate);
    if (!status && ValidGuestRange(output, 4)) PPC_STORE_U32(output, duplicate);
    ctx.r3.u64 = status;
    std::cout << "IMPORT_CALL name=__imp__NtDuplicateObject handle=0x" << std::hex << handle
              << " duplicate=0x" << duplicate << " options=0x" << options << " status=0x"
              << status << std::dec << '\n';
}

// NtCreateTimer(handle*, attributes, type): 0 notification, 1 synchronization.
PPC_FUNC(__imp__NtCreateTimer) {
    RuntimeTraceImport("NtCreateTimer", ctx);
    const uint32_t output = ctx.r3.u32;
    const uint32_t attributes = ctx.r4.u32;
    const uint32_t type = ctx.r5.u32;
    if (attributes) {
        ctx.r3.u64 = kStatusNotImplemented;  // named timers: not reached
        return;
    }
    if (type > 1 || !ValidGuestRange(output, 4)) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    const auto timer = GetGuestObjects().CreateTimer(type == 0);
    PPC_STORE_U32(output, timer.handle);
    ctx.r3.u64 = kStatusSuccess;
    std::cout << "IMPORT_CALL name=__imp__NtCreateTimer handle=0x" << std::hex << timer.handle
              << std::dec << " type=" << type << '\n';
}

// NtSetTimerEx(handle, dueTime*, apcRoutine, apcMode, apcContext, resume,
// periodMs, previousState*). Timer APCs are not reached by the title.
PPC_FUNC(__imp__NtSetTimerEx) {
    RuntimeTraceImport("NtSetTimerEx", ctx);
    const uint32_t handle = ctx.r3.u32;
    const uint32_t dueAddress = ctx.r4.u32;
    const uint32_t apcRoutine = ctx.r5.u32;
    const int32_t period = static_cast<int32_t>(ctx.r9.u32);
    const uint32_t previousAddress = ctx.r10.u32;
    uint32_t status;
    bool previous = false;
    if (apcRoutine) {
        status = kStatusNotImplemented;
    } else if (!ValidGuestRange(dueAddress, 8) || period < 0) {
        status = kStatusInvalidParameter;
    } else {
        const int64_t due = static_cast<int64_t>(PPC_LOAD_U64(dueAddress));
        status = GetGuestObjects().SetTimer(handle, due, uint32_t(period), &previous);
    }
    if (!status && ValidGuestRange(previousAddress, 1)) PPC_STORE_U8(previousAddress, previous ? 1 : 0);
    ctx.r3.u64 = status;
    std::cout << "IMPORT_CALL name=__imp__NtSetTimerEx handle=0x" << std::hex << handle
              << std::dec << " period_ms=" << period << " apc=" << (apcRoutine != 0)
              << " status=0x" << std::hex << status << std::dec << '\n';
}

PPC_FUNC(__imp__NtCancelTimer) {
    RuntimeTraceImport("NtCancelTimer", ctx);
    const uint32_t handle = ctx.r3.u32;
    const uint32_t currentAddress = ctx.r4.u32;
    bool current = false;
    const uint32_t status = GetGuestObjects().CancelTimer(handle, &current);
    if (!status && ValidGuestRange(currentAddress, 1)) PPC_STORE_U8(currentAddress, current ? 1 : 0);
    ctx.r3.u64 = status;
}

// The system library's last resort after the loader fails to leave the title
// (sub_828AC8D0): a console returns to its dashboard; the game ends here.
PPC_FUNC(__imp__HalReturnToFirmware) {
    RuntimeTraceImport("HalReturnToFirmware", ctx);
    std::cerr << "GUEST_RETURN_TO_FIRMWARE routine=" << ctx.r3.u32 << " lr=0x" << std::hex
              << ctx.lr << std::dec << '\n';
    RequestGuestRuntimeStop();
    throw GuestRuntimeStop{};
}

// UNICODE_STRING / ANSI_STRING: Length, MaximumLength (bytes), Buffer. The
// library converts wide debug text (sub_828AB760) and frees it right after.
PPC_FUNC(__imp__RtlUnicodeStringToAnsiString) {
    RuntimeTraceImport("RtlUnicodeStringToAnsiString", ctx);
    const uint32_t destination = ctx.r3.u32;
    const uint32_t source = ctx.r4.u32;
    const bool allocate = (ctx.r5.u32 & 0xFFu) != 0;
    if (!ValidGuestRange(destination, 8) || !ValidGuestRange(source, 8)) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    const uint32_t sourceBytes = PPC_LOAD_U16(source + 0);
    const uint32_t sourceBuffer = PPC_LOAD_U32(source + 4);
    const uint32_t characters = sourceBytes / 2;
    if (characters && !ValidGuestRange(sourceBuffer, characters * 2)) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    uint32_t buffer = 0;
    uint32_t capacity = 0;
    if (allocate) {
        capacity = characters + 1;
        if (capacity > 0xFFFF) {
            ctx.r3.u64 = kStatusInvalidParameter;
            return;
        }
        buffer = AllocateXamPages(base, capacity);
        if (!buffer) {
            ctx.r3.u64 = 0xC0000017u;  // STATUS_NO_MEMORY
            return;
        }
    } else {
        capacity = PPC_LOAD_U16(destination + 2);
        buffer = PPC_LOAD_U32(destination + 4);
        if (capacity && !ValidGuestRange(buffer, capacity)) {
            ctx.r3.u64 = kStatusInvalidParameter;
            return;
        }
    }
    const uint32_t copied = capacity ? std::min(characters, capacity - 1) : 0;
    for (uint32_t index = 0; index < copied; ++index) {
        const uint16_t wide = PPC_LOAD_U16(sourceBuffer + index * 2);
        PPC_STORE_U8(buffer + index, wide < 0x100 ? uint8_t(wide) : uint8_t('?'));
    }
    if (capacity) PPC_STORE_U8(buffer + copied, 0);
    PPC_STORE_U16(destination + 0, uint16_t(copied));
    if (allocate) {
        PPC_STORE_U16(destination + 2, uint16_t(capacity));
        PPC_STORE_U32(destination + 4, buffer);
    }
    ctx.r3.u64 = copied < characters ? 0x80000005u : kStatusSuccess;  // STATUS_BUFFER_OVERFLOW
}

PPC_FUNC(__imp__RtlFreeAnsiString) {
    RuntimeTraceImport("RtlFreeAnsiString", ctx);
    const uint32_t string = ctx.r3.u32;
    if (!ValidGuestRange(string, 8)) return;
    const uint32_t buffer = PPC_LOAD_U32(string + 4);
    if (buffer) FreeXamPages(buffer);
    PPC_STORE_U16(string + 0, 0);
    PPC_STORE_U16(string + 2, 0);
    PPC_STORE_U32(string + 4, 0);
}

// No chatpad or keyboard keystroke is ever pending (ERROR_EMPTY).
PPC_FUNC(__imp__XamInputGetKeystrokeEx) {
    RuntimeTraceImport("XamInputGetKeystrokeEx", ctx);
    ctx.r3.u64 = kXErrorEmpty;
}

// The title shows a system message box only on its CRT fatal path
// (sub_828AA3E0 -> sub_828AA200 -> sub_828AA0C0), right before it ends itself:
// keep its words in the report and stop there.
PPC_FUNC(__imp__XamShowMessageBoxUIEx) {
    RuntimeTraceImport("XamShowMessageBoxUIEx", ctx);
    const std::string caption = RuntimeUtf16ToUtf8(SafeGuestWideString(base, ctx.r4.u32, 512));
    const std::string text = RuntimeUtf16ToUtf8(SafeGuestWideString(base, ctx.r5.u32, 2048));
    std::ostringstream detail;
    detail << "title message box lr=0x" << std::hex << ctx.lr << std::dec << " caption=\""
           << caption << "\" text=\"" << text << '"';
    std::cerr << "GUEST_FATAL_MESSAGE_BOX " << detail.str() << '\n';
    RuntimeFatalRecordDetail(detail.str());
    RuntimeFatalWriteDump("title-message-box");
    throw std::runtime_error("the game stopped itself with the message: " +
                             (text.empty() ? caption : text));
}

// The title ends itself: after its CRT fatal message (reported above) or at
// the end of _xstart. A stop without a blocker is not an error.
PPC_FUNC(__imp__XamLoaderTerminateTitle) {
    RuntimeTraceImport("XamLoaderTerminateTitle", ctx);
    std::cerr << "GUEST_TERMINATE_TITLE lr=0x" << std::hex << ctx.lr << std::dec << '\n';
    RequestGuestRuntimeStop();
    throw GuestRuntimeStop{};
}

// Kernel bug check (e.g. the CRT's stack-cookie failure): fatal, reported
// with its code.
PPC_FUNC(__imp__KeBugCheck) {
    RuntimeTraceImport("KeBugCheck", ctx);
    std::ostringstream message;
    message << "the game stopped itself with kernel bug check 0x" << std::hex << ctx.r3.u32
            << " lr=0x" << ctx.lr;
    RuntimeFatalRecordDetail(message.str());
    RuntimeFatalWriteDump("bugcheck");
    throw std::runtime_error(message.str());
}

// A guest exception (C++ throw = 0xE06D7363): the runtime has no guest SEH
// dispatch, so this stays fatal, now with the code and caller in the report.
PPC_FUNC(__imp__RtlRaiseException) {
    RuntimeTraceImport("RtlRaiseException", ctx);
    const uint32_t record = ctx.r3.u32;
    const uint32_t code = record && record <= UINT32_MAX - 4u ? PPC_LOAD_U32(record) : 0u;
    std::ostringstream message;
    message << "the game raised exception 0x" << std::hex << code << " lr=0x" << ctx.lr
            << " record=0x" << record;
    RuntimeFatalRecordDetail(message.str());
    RuntimeFatalWriteDump("guest-exception");
    throw std::runtime_error(message.str());
}

// Xenia classifies this Xbox debug export as a void stub. It is a debugger
// notification, not a guest failure result; preserving the normal return lets
// retail title code continue while retaining evidence of every reach.
PPC_FUNC(__imp__DbgBreakPoint) {
    RuntimeTraceImport("DbgBreakPoint", ctx);
    std::cout << "IMPORT_CALL name=__imp__DbgBreakPoint thread=" << CurrentGuestThreadId()
              << " lr=0x" << std::hex << ctx.lr << std::dec << " classification=debug-stub\n";
}

// Ke* dispatcher waits operate on an X_KEVENT embedded in guest-owned memory,
// unlike Nt* waits which receive an object-table handle. Preserve the 16-byte
// dispatcher header's event type and signal state while using host condition
// variables only as the scheduling mechanism.
PPC_FUNC(__imp__KeSetEvent) {
    RuntimeTraceImport("KeSetEvent", ctx);
    const uint32_t event = ctx.r3.u32;
    const uint32_t increment = ctx.r4.u32;
    const uint32_t wait = ctx.r5.u32;
    const int32_t previous = GetGuestDispatcherEvents().Set(base, event);
    ctx.r3.s64 = previous;
    static std::atomic<uint64_t> traceCount{};
    const uint64_t ordinal = kCompletedHotPathDiagnosticsEnabled
        ? traceCount.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    if (kCompletedHotPathDiagnosticsEnabled &&
        (ordinal <= 64 || (ordinal & 0xFFu) == 0)) {
        std::cout << "IMPORT_CALL name=__imp__KeSetEvent thread=" << CurrentGuestThreadId()
                  << " event=0x" << std::hex << event << " increment=0x" << increment
                  << " wait=0x" << wait << " previous=0x" << previous << std::dec
                  << " ordinal=" << ordinal << '\n';
    }
}

PPC_FUNC(__imp__KeResetEvent) {
    RuntimeTraceImport("KeResetEvent", ctx);
    const uint32_t event = ctx.r3.u32;
    const int32_t previous = GetGuestDispatcherEvents().Reset(base, event);
    ctx.r3.s64 = previous;
    uint64_t ordinal = 0;
    if (kCompletedHotPathDiagnosticsEnabled &&
        (ShouldTraceHotPath(resetEventTraceCount, ordinal) || previous != 1)) {
        std::cout << "IMPORT_CALL name=__imp__KeResetEvent thread="
                  << CurrentGuestThreadId() << " event=0x" << std::hex << event
                  << " previous=0x" << previous << std::dec << " ordinal=" << ordinal
                  << '\n';
    }
}

PPC_FUNC(__imp__KeInitializeSemaphore) {
    RuntimeTraceImport("KeInitializeSemaphore", ctx);
    const uint32_t semaphore = ctx.r3.u32;
    const uint32_t count = ctx.r4.u32;
    const uint32_t limit = ctx.r5.u32;
    GetGuestDispatcherSemaphores().Initialize(base, semaphore, count, limit);
    std::cout << "IMPORT_CALL name=__imp__KeInitializeSemaphore semaphore=0x"
              << std::hex << semaphore << std::dec << " count=" << count
              << " limit=" << limit << '\n';
}

PPC_FUNC(__imp__KeReleaseSemaphore) {
    RuntimeTraceImport("KeReleaseSemaphore", ctx);
    const uint32_t semaphore = ctx.r3.u32;
    const uint32_t increment = ctx.r4.u32;
    const uint32_t adjustment = ctx.r5.u32;
    const uint32_t wait = ctx.r6.u32;
    const int32_t previous =
        GetGuestDispatcherSemaphores().Release(base, semaphore, adjustment);
    ctx.r3.s64 = previous;
    std::cout << "IMPORT_CALL name=__imp__KeReleaseSemaphore semaphore=0x"
              << std::hex << semaphore << std::dec << " increment=" << increment
              << " adjustment=" << adjustment << " wait=" << wait
              << " previous=" << previous << '\n';
}

PPC_FUNC(__imp__KeWaitForSingleObject) {
    RuntimeTraceImport("KeWaitForSingleObject", ctx);
    const uint32_t event = ctx.r3.u32;
    const uint32_t waitReason = ctx.r4.u32;
    const uint32_t processorMode = ctx.r5.u32;
    const bool alertable = ctx.r6.u32 != 0;
    const uint32_t timeoutAddress = ctx.r7.u32;
    const bool hasTimeout = timeoutAddress != 0;
    const int64_t timeout = hasTimeout ? static_cast<int64_t>(PPC_LOAD_U64(timeoutAddress)) : 0;
    const uint32_t threadObject = CurrentGuestThreadObject();
    if (!threadObject || !GetGuestObjects().SetThreadState(threadObject, GuestThreadState::Waiting))
        throw std::runtime_error("KeWaitForSingleObject called without a live guest thread");

    static std::atomic<uint64_t> traceCount{};
    const uint64_t ordinal = traceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    static thread_local uint32_t tracedEvent = UINT32_MAX;
    static thread_local int64_t tracedTimeout = INT64_MIN;
    static thread_local bool tracedHasTimeout{};
    const bool newWaitSignature = kCompletedHotPathDiagnosticsEnabled &&
                                  (event != tracedEvent || timeout != tracedTimeout ||
                                   hasTimeout != tracedHasTimeout);
    if (newWaitSignature) {
        tracedEvent = event;
        tracedTimeout = timeout;
        tracedHasTimeout = hasTimeout;
    }
    const bool trace = kCompletedHotPathDiagnosticsEnabled &&
                       (ordinal <= 64 || (ordinal & 0xFFFu) == 0 || newWaitSignature);
    if (trace) {
        std::cout << "KE_WAIT_ENTER thread=" << CurrentGuestThreadId() << " event=0x"
                  << std::hex << event << " type=0x" << uint32_t(PPC_LOAD_U8(event))
                  << " state=0x" << PPC_LOAD_U32(event + 4) << std::dec
                  << " reason=" << waitReason << " mode=" << processorMode
                  << " alertable=" << alertable << " timeout=" << timeout
                  << " ordinal=" << ordinal << '\n';
    }
    uint32_t status{};
    try {
        const bool deliveredApc =
            alertable && HasPendingGuestApcs() && DeliverGuestApcs(ctx, base);
        if (deliveredApc) {
            status = kStatusUserApc;
        } else {
            const uint32_t dispatcherType = PPC_LOAD_U8(event);
            if (dispatcherType <= 1) {
                status = GetGuestDispatcherEvents().Wait(base, event, hasTimeout, timeout);
            } else if (dispatcherType == 5) {
                status = GetGuestDispatcherSemaphores().Wait(base, event, hasTimeout, timeout);
            } else {
                throw std::runtime_error("KeWaitForSingleObject reached an unsupported dispatcher type");
            }
        }
        GetGuestObjects().SetThreadState(threadObject, GuestThreadState::Running);
        if (alertable && !deliveredApc) DeliverGuestApcs(ctx, base);
    } catch (...) {
        GetGuestObjects().SetThreadState(threadObject, GuestThreadState::Running);
        throw;
    }
    ctx.r3.u64 = status;
    RuntimeTraceWait(event, hasTimeout, timeout, status, ctx);
    if (trace || (status != kStatusSuccess && status != kStatusTimeout)) {
        std::cout << "KE_WAIT_EXIT thread=" << CurrentGuestThreadId() << " event=0x"
                  << std::hex << event << " status=0x" << status << std::dec
                  << " ordinal=" << ordinal << '\n';
    }
}

PPC_FUNC(__imp__KeWaitForMultipleObjects) {
    RuntimeTraceImport("KeWaitForMultipleObjects", ctx);
    const uint32_t count = ctx.r3.u32;
    const uint32_t objectsPointer = ctx.r4.u32;
    const uint32_t waitType = ctx.r5.u32;
    const uint32_t waitReason = ctx.r6.u32;
    const uint32_t processorMode = ctx.r7.u32;
    const bool alertable = ctx.r8.u32 != 0;
    const uint32_t timeoutAddress = ctx.r9.u32;
    const bool hasTimeout = timeoutAddress != 0;
    const int64_t timeout = hasTimeout ? static_cast<int64_t>(PPC_LOAD_U64(timeoutAddress)) : 0;
    const uint32_t waitBlocks = ctx.r10.u32;
    if (!count || count > 64 || !objectsPointer || (objectsPointer & 3u) ||
        objectsPointer > UINT32_MAX - count * 4u || waitType > 1) {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    // WaitAll needs atomic acquisition across heterogeneous dispatcher
    // objects. The dynamically reached audio contract is WaitAny; stop if a
    // future path requests WaitAll until that family is evidenced.
    if (waitType == 0) {
        throw std::runtime_error("KeWaitForMultipleObjects reached unimplemented WaitAll semantics");
    }

    std::vector<uint32_t> objects;
    objects.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t object = PPC_LOAD_U32(objectsPointer + index * 4);
        const uint32_t type = object ? PPC_LOAD_U8(object) : UINT32_MAX;
        if (!object || (type > 1 && type != 5)) {
            ctx.r3.u64 = kStatusInvalidParameter;
            return;
        }
        objects.push_back(object);
    }

    const uint32_t threadObject = CurrentGuestThreadObject();
    if (!threadObject || !GetGuestObjects().SetThreadState(threadObject, GuestThreadState::Waiting))
        throw std::runtime_error("KeWaitForMultipleObjects called without a live guest thread");
    const auto deadline = hasTimeout ? GuestWaitDeadline(timeout)
                                     : std::chrono::steady_clock::time_point::max();
    uint32_t status = kStatusTimeout;
    try {
        while (true) {
            if (alertable && HasPendingGuestApcs()) {
                DeliverGuestApcs(ctx, base);
                status = kStatusUserApc;
                break;
            }
            const uint64_t generation = GetGuestDispatcherWaitCoordinator().Generation();
            bool acquired = false;
            for (uint32_t index = 0; index < count; ++index) {
                const uint32_t object = objects[index];
                const uint32_t type = PPC_LOAD_U8(object);
                const bool signalled = type <= 1
                    ? GetGuestDispatcherEvents().TryWait(base, object)
                    : GetGuestDispatcherSemaphores().TryWait(base, object);
                if (!signalled) continue;
                status = index;
                acquired = true;
                break;
            }
            if (acquired) break;
            if (hasTimeout && std::chrono::steady_clock::now() >= deadline) {
                status = kStatusTimeout;
                break;
            }
            if (!GetGuestDispatcherWaitCoordinator().WaitForChange(
                    generation, hasTimeout, deadline)) {
                status = kStatusTimeout;
                break;
            }
            if (GuestRuntimeStopRequested()) throw GuestRuntimeStop{};
        }
        GetGuestObjects().SetThreadState(threadObject, GuestThreadState::Running);
    } catch (...) {
        GetGuestObjects().SetThreadState(threadObject, GuestThreadState::Running);
        throw;
    }
    ctx.r3.u64 = status;
    static std::atomic<uint64_t> traceCount{};
    const uint64_t ordinal = traceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal <= 64 || (ordinal & 0xFFFu) == 0 || status == kStatusUserApc) {
        std::cout << "IMPORT_CALL name=__imp__KeWaitForMultipleObjects thread="
                  << CurrentGuestThreadId() << " count=" << count << " wait_type=" << waitType
                  << " reason=" << waitReason << " mode=" << processorMode
                  << " alertable=" << alertable << " timeout=" << timeout
                  << " wait_blocks=0x" << std::hex << waitBlocks << " result=0x" << status
                  << std::dec << " ordinal=" << ordinal << '\n';
    }
}

PPC_FUNC(__imp__NtWaitForSingleObjectEx) {
    RuntimeTraceImport("NtWaitForSingleObjectEx", ctx);
    // Xbox ABI: r3 handle, r4 wait mode, r5 alertable, r6 timeout pointer.
    const uint32_t timeoutAddress = ctx.r6.u32;
    const bool hasTimeout = timeoutAddress != 0;
    const int64_t timeout = hasTimeout ? static_cast<int64_t>(PPC_LOAD_U64(timeoutAddress)) : 0;
    const uint32_t handle = ctx.r3.u32;
    const bool alertable = ctx.r5.u32 != 0;
    const uint32_t threadObject = CurrentGuestThreadObject();
    if (!threadObject || !GetGuestObjects().SetThreadState(threadObject, GuestThreadState::Waiting)) {
        throw std::runtime_error("NtWaitForSingleObjectEx called without a live guest thread");
    }
    // Poll-heavy service threads issue these waits every 1-100 ms. Preserve
    // the first calls, signature changes, periodic liveness samples and
    // exceptional completions. Repeating every untimed wait here serializes
    // multiple guest threads through the unit-buffered diagnostic stream and
    // measurably perturbs title pacing.
    static std::atomic<uint64_t> waitTraceCount{};
    const uint64_t waitOrdinal = kCompletedHotPathDiagnosticsEnabled
        ? waitTraceCount.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    static thread_local uint32_t tracedHandle = UINT32_MAX;
    static thread_local int64_t tracedTimeout = INT64_MIN;
    static thread_local bool tracedAlertable{};
    const bool newWaitSignature = kCompletedHotPathDiagnosticsEnabled &&
                                  (handle != tracedHandle || timeout != tracedTimeout ||
                                   alertable != tracedAlertable);
    if (newWaitSignature) {
        tracedHandle = handle;
        tracedTimeout = timeout;
        tracedAlertable = alertable;
    }
    const bool traceWait = kCompletedHotPathDiagnosticsEnabled &&
                           (waitOrdinal <= 64 || (waitOrdinal & 0xFFFu) == 0 ||
                            newWaitSignature);
    if (traceWait) {
        std::cout << "WAIT_ENTER thread=" << CurrentGuestThreadId() << " handle=0x" << std::hex
                  << handle << " lr=0x" << ctx.lr << " r1=0x" << ctx.r1.u32
                  << " timeout=" << std::dec << timeout << " alertable=" << alertable
                  << " ordinal=" << waitOrdinal << "\n";
    }
    // NtReadFile queues immediate completions to the current guest thread.
    // At an alertable wait boundary, deliver that real queued APC before
    // blocking and report the verified NT interruption status. The APC may
    // itself signal the waited object; a subsequent title wait then observes
    // that real event state.
    const bool deliveredApc = alertable && HasPendingGuestApcs() && DeliverGuestApcs(ctx, base);
    const bool recordHitch = RuntimeHitchDiagnosticsEnabled();
    const auto waitStart = recordHitch
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    const uint32_t status = deliveredApc
        ? kStatusUserApc
        : GetGuestObjects().WaitForSingleObject(handle, hasTimeout, timeout);
    const uint64_t waitDurationUs = recordHitch
        ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - waitStart).count())
        : 0;
    GetGuestObjects().SetThreadState(threadObject, GuestThreadState::Running);
    if (alertable && !deliveredApc) DeliverGuestApcs(ctx, base);
    ctx.r3.u64 = status;
    RuntimeTraceWait(handle, hasTimeout, timeout, status, ctx);
    const bool routineCompletion = status == kStatusSuccess || status == kStatusUserApc;
    const bool traceCompletion = status != kStatusTimeout && !routineCompletion;
    if (recordHitch && waitDurationUs >= 75000) {
        static std::atomic<uint64_t> slowWaitCount{};
        const uint64_t slowOrdinal =
            slowWaitCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (slowOrdinal <= 32 || (slowOrdinal & 0xFFFu) == 0) {
            std::cout << "RUNTIME_WAIT_HITCH duration_us=" << waitDurationUs
                      << " thread=" << CurrentGuestThreadId()
                      << " handle=0x" << std::hex << handle
                      << " lr=0x" << ctx.lr << " status=0x" << status
                      << std::dec << " timeout=" << timeout
                      << " has_timeout=" << hasTimeout
                      << " alertable=" << alertable
                      << " delivered_apc=" << deliveredApc
                      << " ordinal=" << slowOrdinal << '\n';
        }
    }
    if (traceWait || traceCompletion) {
        std::cout << "WAIT_EXIT thread=" << CurrentGuestThreadId() << " handle=0x" << std::hex
                  << handle << " lr=0x" << ctx.lr << " r1=0x" << ctx.r1.u32
                  << " status=0x" << status << std::dec
                  << " ordinal=" << waitOrdinal << '\n';
    }
}

PPC_FUNC(__imp__NtReleaseSemaphore) {
    uint32_t previousCount{};
    const uint32_t handle = ctx.r3.u32;
    const uint32_t status = GetGuestObjects().ReleaseSemaphore(handle, ctx.r4.u32, &previousCount);
    if (ctx.r5.u32) PPC_STORE_U32(ctx.r5.u32, previousCount);
    ctx.r3.u64 = status;
    if (status != kStatusSuccess || kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__NtReleaseSemaphore handle=0x" << std::hex
                  << handle << " status=0x" << status << " previous=" << std::dec
                  << previousCount << '\n';
    }
}

PPC_FUNC(__imp__ObLookupThreadByThreadId) {
    uint32_t guestObject{};
    const uint32_t status = GetGuestObjects().ReferenceThreadById(ctx.r3.u32, &guestObject);
    if (!status && ctx.r4.u32) PPC_STORE_U32(ctx.r4.u32, guestObject);
    ctx.r3.u64 = status;
}

PPC_FUNC(__imp__ObOpenObjectByPointer) {
    const uint32_t guestObject = ctx.r3.u32;
    uint32_t handle{};
    const uint32_t status = GetGuestObjects().OpenObjectByGuestPointer(guestObject, &handle);
    if (!status && ctx.r4.u32) PPC_STORE_U32(ctx.r4.u32, handle);
    ctx.r3.u64 = status;
    if (status != kStatusSuccess || kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__ObOpenObjectByPointer object=0x" << std::hex
                  << guestObject << " handle=0x" << handle << " status=0x" << status
                  << std::dec << '\n';
    }
}

PPC_FUNC(__imp__XexCheckExecutablePrivilege) {
    ctx.r3.u64 = ExecutableHasPrivilege(ctx.r3.u32) ? 1 : 0;
}

PPC_FUNC(__imp__RtlLowerChar) {
    const uint8_t value = ctx.r3.u8;
    ctx.r3.u64 = (value >= 'A' && value <= 'Z') ? value + ('a' - 'A') : value;
}

// The cache-unavailable probe reaches STATUS_OBJECT_NAME_NOT_FOUND, an empty
// initial wildcard search reaches STATUS_NO_SUCH_FILE, and an exhausted
// directory enumeration reaches STATUS_NO_MORE_FILES. Xenia's Xbox error
// table maps the first two to ERROR_FILE_NOT_FOUND (2), and the last to
// ERROR_NO_MORE_FILES (18).
// Preserve NT-success and already-DOS-encoded values, and leave other NT
// status mappings unresolved until a caller supplies evidence for them.
PPC_FUNC(__imp__RtlNtStatusToDosError) {
    const uint32_t status = ctx.r3.u32;
    uint32_t result{};
    if (!status || (status & 0x20000000u)) {
        result = status;
    } else if ((status >> 16) == 0x8007u || (status >> 16) == 0xC001u) {
        result = status & 0xFFFFu;
    } else if (status == kStatusInvalidHandle) {
        result = 6; // ERROR_INVALID_HANDLE.
    } else if (status == kStatusAccessDenied) {
        // Pinned Xbox kernel error table entry for STATUS_ACCESS_DENIED.
        result = kXErrorAccessDenied;
    } else if (status == kStatusNoSuchFile ||
               status == 0xC0000034u || status == 0xD0000034u) {
        result = 2; // ERROR_FILE_NOT_FOUND.
    } else if (status == kStatusNoMoreFiles) {
        result = 18; // ERROR_NO_MORE_FILES.
    } else {
        std::ostringstream message;
        message << "RtlNtStatusToDosError reached unverified status=0x" << std::hex << status;
        throw std::runtime_error(message.str());
    }
    ctx.r3.u64 = result;
    if (kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__RtlNtStatusToDosError status=0x" << std::hex
                  << status << " result=" << std::dec << result << '\n';
    }
}

// Verified against the local Xbox kernel implementation: X_ANSI_STRING is
// big-endian { uint16 length, uint16 maximum_length, uint32 buffer }. The
// source is retained as a guest address; no host allocation is performed.
PPC_FUNC(__imp__RtlInitAnsiString) {
    const uint32_t destination = ctx.r3.u32;
    const uint32_t source = ctx.r4.u32;
    if (!destination) return;
    uint16_t length{};
    if (source) {
        const auto* terminator = static_cast<const uint8_t*>(std::memchr(base + source, 0, UINT16_MAX));
        length = terminator ? static_cast<uint16_t>(terminator - (base + source)) : UINT16_MAX;
        PPC_STORE_U16(destination, length);
        PPC_STORE_U16(destination + 2, static_cast<uint16_t>(length + 1));
    } else {
        PPC_STORE_U16(destination, 0);
        PPC_STORE_U16(destination + 2, 0);
    }
    PPC_STORE_U32(destination + 4, source);
    if (kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__RtlInitAnsiString destination=0x" << std::hex
                  << destination << " source=0x" << source << " length=" << std::dec << length;
        if (source)
            std::cout << " value="
                      << std::string(reinterpret_cast<const char*>(base + source), length);
        std::cout << '\n';
    }
}

// Verified against the pinned ReXGlue Xbox-kernel implementation:
// X_UNICODE_STRING uses the same big-endian descriptor layout as
// X_ANSI_STRING, but its lengths are byte counts for UTF-16 code units. The
// source remains guest-owned and no allocation or conversion is performed.
PPC_FUNC(__imp__RtlInitUnicodeString) {
    const uint32_t destination = ctx.r3.u32;
    const uint32_t source = ctx.r4.u32;
    if (!destination) return;

    uint16_t length{};
    uint16_t maximumLength{};
    if (source) {
        constexpr uint32_t kMaximumCharacters = (UINT16_MAX - 1u) / 2u;
        uint32_t characters{};
        while (characters < kMaximumCharacters &&
               PPC_LOAD_U16(source + characters * 2u) != 0) {
            ++characters;
        }
        if (characters == kMaximumCharacters &&
            PPC_LOAD_U16(source + characters * 2u) != 0) {
            throw std::runtime_error("RtlInitUnicodeString reached an unterminated or oversized guest string");
        }
        length = static_cast<uint16_t>(characters * 2u);
        maximumLength = static_cast<uint16_t>(length + 2u);
    }

    PPC_STORE_U16(destination, length);
    PPC_STORE_U16(destination + 2, maximumLength);
    PPC_STORE_U32(destination + 4, source);
}

// The pinned kernel contract widens each input byte directly into one
// big-endian UTF-16 code unit, copies only complete destination code units,
// optionally reports the byte count written, and returns STATUS_SUCCESS. It
// does not append a terminator.
PPC_FUNC(__imp__RtlMultiByteToUnicodeN) {
    const uint32_t destination = ctx.r3.u32;
    const uint32_t destinationLength = ctx.r4.u32;
    const uint32_t written = ctx.r5.u32;
    const uint32_t source = ctx.r6.u32;
    const uint32_t sourceLength = ctx.r7.u32;
    const uint32_t copyLength = std::min(destinationLength >> 1, sourceLength);

    if ((copyLength && (!destination || !source)) ||
        (copyLength &&
         (static_cast<uint64_t>(destination) + copyLength * 2u > UINT32_MAX + uint64_t{1} ||
          static_cast<uint64_t>(source) + copyLength > UINT32_MAX + uint64_t{1})) ||
        (written && static_cast<uint64_t>(written) + sizeof(uint32_t) > UINT32_MAX + uint64_t{1})) {
        throw std::runtime_error("RtlMultiByteToUnicodeN received an invalid guest range");
    }

    for (uint32_t i = 0; i < copyLength; ++i) {
        PPC_STORE_U16(destination + i * 2u, PPC_LOAD_U8(source + i));
    }
    if (written) PPC_STORE_U32(written, copyLength * 2u);
    ctx.r3.u64 = kStatusSuccess;
}

// ObCreate/DeleteSymbolicLink operate on the Xbox object-manager namespace,
// not the host filesystem. The runtime VFS retains these mappings and applies
// them before resolving a title path into the user's extracted game tree.
PPC_FUNC(__imp__ObCreateSymbolicLink) {
    RuntimeTraceImport("ObCreateSymbolicLink", ctx);
    const std::string path = ReadGuestAnsiString(base, ctx.r3.u32);
    const std::string target = ReadGuestAnsiString(base, ctx.r4.u32);
    const bool registered = GetGuestFileSystem().RegisterSymbolicLink(path, target);
    ctx.r3.u64 = registered ? kStatusSuccess : kStatusUnsuccessful;
    std::cout << "IMPORT_CALL name=__imp__ObCreateSymbolicLink path=" << path
              << " target=" << target << " status=0x" << std::hex << ctx.r3.u32
              << std::dec << '\n';
}

PPC_FUNC(__imp__ObDeleteSymbolicLink) {
    RuntimeTraceImport("ObDeleteSymbolicLink", ctx);
    const std::string path = ReadGuestAnsiString(base, ctx.r3.u32);
    const bool removed = GetGuestFileSystem().UnregisterSymbolicLink(path);
    ctx.r3.u64 = removed ? kStatusSuccess : kStatusUnsuccessful;
    std::cout << "IMPORT_CALL name=__imp__ObDeleteSymbolicLink path=" << path
              << " removed=" << removed << " status=0x" << std::hex << ctx.r3.u32
              << std::dec << '\n';
}

// The render-driver client is a bounded producer/consumer contract, not an
// initialization-only token. Registration starts a kernel-visible guest
// callback worker with eight frame credits; each real host playback completion
// returns one credit. Submitted 5.1 big-endian float frames are copied and
// converted by runtime_audio before the title can reuse their guest storage.
PPC_FUNC(__imp__XMACreateContext) {
    RuntimeTraceImport("XMACreateContext", ctx);
    const uint32_t contextOut = ctx.r3.u32;
    ctx.r3.u64 = RuntimeAudioCreateXmaContext(base, contextOut);
    if (ctx.r3.u32 || kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__XMACreateContext output=0x" << std::hex
                  << contextOut << " context=0x"
                  << ((!ctx.r3.u32 && contextOut) ? PPC_LOAD_U32(contextOut) : 0)
                  << " status=0x" << ctx.r3.u32 << std::dec << '\n';
    }
}

PPC_FUNC(__imp__XMAReleaseContext) {
    RuntimeTraceImport("XMAReleaseContext", ctx);
    const uint32_t context = ctx.r3.u32;
    ctx.r3.u64 = RuntimeAudioReleaseXmaContext(context);
    if (ctx.r3.u32 || kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__XMAReleaseContext context=0x" << std::hex
                  << context << " status=0x" << ctx.r3.u32 << std::dec << '\n';
    }
}

PPC_FUNC(__imp__XAudioRegisterRenderDriverClient) {
    RuntimeTraceImport("XAudioRegisterRenderDriverClient", ctx);
    const uint32_t callbackPair = ctx.r3.u32;
    const uint32_t driverOut = ctx.r4.u32;
    ctx.r3.u64 = RuntimeAudioRegisterRenderDriverClient(base, callbackPair, driverOut);
    std::cout << "IMPORT_CALL name=__imp__XAudioRegisterRenderDriverClient callback_pair=0x"
              << std::hex << callbackPair << " driver_out=0x" << driverOut
              << " result=0x" << ctx.r3.u32 << std::dec << '\n';
}

PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient) {
    RuntimeTraceImport("XAudioUnregisterRenderDriverClient", ctx);
    const uint32_t driver = ctx.r3.u32;
    ctx.r3.u64 = RuntimeAudioUnregisterRenderDriverClient(driver);
    std::cout << "IMPORT_CALL name=__imp__XAudioUnregisterRenderDriverClient driver=0x"
              << std::hex << driver << " result=0x" << ctx.r3.u32 << std::dec << '\n';
}

PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
    RuntimeTraceImport("XAudioSubmitRenderDriverFrame", ctx);
    const uint32_t driver = ctx.r3.u32;
    const uint32_t samples = ctx.r4.u32;
    ctx.r3.u64 = RuntimeAudioSubmitRenderDriverFrame(base, driver, samples);
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask) {
    RuntimeTraceImport("XAudioGetVoiceCategoryVolumeChangeMask", ctx);
    const uint32_t driver = ctx.r3.u32;
    const uint32_t output = ctx.r4.u32;
    if (!output || (output & 3u)) {
        ctx.r3.u64 = 0x80070057u;
        return;
    }
    uint32_t mask{};
    ctx.r3.u64 = RuntimeAudioGetVoiceCategoryVolumeChangeMask(driver, &mask);
    if (!ctx.r3.u32) PPC_STORE_U32(output, mask);
    if constexpr (kCompletedHotPathDiagnosticsEnabled) {
        std::cout << "IMPORT_CALL name=__imp__XAudioGetVoiceCategoryVolumeChangeMask driver=0x"
                  << std::hex << driver << " output=0x" << output << std::dec
                  << " mask=" << mask << '\n';
    }
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume) {
    RuntimeTraceImport("XAudioGetVoiceCategoryVolume", ctx);
    const uint32_t category = ctx.r3.u32;
    const uint32_t output = ctx.r4.u32;
    if (!output || (output & 3u)) {
        ctx.r3.u64 = 0x80070057u;
        return;
    }
    PPC_STORE_U32(output, 0x3F800000u);
    ctx.r3.u64 = 0;
    std::cout << "IMPORT_CALL name=__imp__XAudioGetVoiceCategoryVolume category="
              << category << " output=0x" << std::hex << output << std::dec
              << " volume=1\n";
}

PPC_FUNC(__imp__RtlInitializeCriticalSection) {
    GetGuestCriticalSections().Initialize(base, ctx.r3.u32);
}

PPC_FUNC(__imp__RtlEnterCriticalSection) {
    GetGuestCriticalSections().Enter(base, ctx.r3.u32, CurrentGuestThreadObject());
}

PPC_FUNC(__imp__RtlTryEnterCriticalSection) {
    RuntimeTraceImport("RtlTryEnterCriticalSection", ctx);
    ctx.r3.u64 = GetGuestCriticalSections().TryEnter(
        base, ctx.r3.u32, CurrentGuestThreadObject()) ? 1u : 0u;
}

PPC_FUNC(__imp__RtlLeaveCriticalSection) {
    GetGuestCriticalSections().Leave(base, ctx.r3.u32, CurrentGuestThreadObject());
}

PPC_FUNC(__imp__KeEnterCriticalRegion) {
    RuntimeTraceImport("KeEnterCriticalRegion", ctx);
    const int32_t disableCount = EnterGuestCriticalRegion();
    uint64_t ordinal = 0;
    if (ShouldTraceHotPath(criticalRegionTraceCount, ordinal)) {
        std::cout << "IMPORT_CALL name=__imp__KeEnterCriticalRegion thread="
                  << CurrentGuestThreadId() << " apc_disable_count=" << disableCount
                  << " ordinal=" << ordinal << '\n';
    }
}

PPC_FUNC(__imp__KeLeaveCriticalRegion) {
    RuntimeTraceImport("KeLeaveCriticalRegion", ctx);
    const int32_t disableCount = LeaveGuestCriticalRegion();
    uint64_t ordinal = 0;
    if (ShouldTraceHotPath(criticalRegionTraceCount, ordinal)) {
        std::cout << "IMPORT_CALL name=__imp__KeLeaveCriticalRegion thread="
                  << CurrentGuestThreadId() << " apc_disable_count=" << disableCount
                  << " ordinal=" << ordinal << '\n';
    }
}

// Xbox spin locks contain the owning PCR address in guest byte order. The
// pinned ReXGlue implementation raises KfAcquireSpinLock callers to DISPATCH
// IRQL, while the AtRaisedIrql pair changes only lock ownership.
PPC_FUNC(__imp__KfAcquireSpinLock) {
    const uint32_t address = ctx.r3.u32;
    const uint32_t oldIrql = AcquireGuestSpinLock(ctx, base, address, true);
    ctx.r3.u64 = oldIrql;
    TraceGuestSpinLock("acquire", address, oldIrql);
}

PPC_FUNC(__imp__KfReleaseSpinLock) {
    const uint32_t address = ctx.r3.u32;
    const uint32_t oldIrql = ctx.r4.u32;
    ReleaseGuestSpinLock(ctx, base, address, oldIrql, true);
    TraceGuestSpinLock("release", address, oldIrql);
}

PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql) {
    const uint32_t address = ctx.r3.u32;
    AcquireGuestSpinLock(ctx, base, address, false);
    TraceGuestSpinLock("acquire_raised", address,
                       PPC_LOAD_U8(ctx.r13.u32 + kPcrCurrentIrqlOffset));
}

PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql) {
    const uint32_t address = ctx.r3.u32;
    const uint32_t currentIrql = PPC_LOAD_U8(ctx.r13.u32 + kPcrCurrentIrqlOffset);
    ReleaseGuestSpinLock(ctx, base, address, 0, false);
    TraceGuestSpinLock("release_raised", address, currentIrql);
}
