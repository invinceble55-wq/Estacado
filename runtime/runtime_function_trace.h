#pragma once

#include <atomic>
#include <cstdint>

struct PPCContext;
uint32_t CurrentGuestThreadId();

// Generated-code hook policy (V285). Every generated function calls
// PPC_RUNTIME_FUNCTION_ENTER with its guest address as a literal, so the
// per-address duties below fold at compile time; ordinary functions pay two
// relaxed loads. RuntimeFunctionEnter itself is unchanged and re-checks
// everything, so skipping it only drops work that would have been a no-op
// (plus the diagnostic active-context TLS, which is refreshed on slow paths).
//  - g_runtime_entry_slow_mask: stop request and startup-enabled diagnostics.
//  - g_runtime_pending_interrupt_count: queued graphics interrupts (the same
//    counter DeliverRuntimeGraphicsInterrupts already consults).
enum : uint32_t {
    kRuntimeEntrySlowStop = 1u << 0,
    kRuntimeEntrySlowCadence = 1u << 1,
    kRuntimeEntrySlowCameraTrace = 1u << 2,
    kRuntimeEntrySlowCoverage = 1u << 3,
};
// Function coverage (diagnostic, DARKNESS_FUNCTION_COVERAGE=<file>): every
// guest function entered is recorded; the list (one hex address per line) is
// written at shutdown. Off by default: it routes every function entry through
// the slow path.
void ConfigureRuntimeFunctionCoverage(const wchar_t* path) noexcept;
void WriteRuntimeFunctionCoverage() noexcept;
extern std::atomic<uint32_t> g_runtime_entry_slow_mask;
extern std::atomic<uint32_t> g_runtime_pending_interrupt_count;
void RuntimeSetEntrySlowBit(uint32_t bit, bool enabled) noexcept;

// V376 gameplay field of view: only the title's memcpy (0x82899BF0) checks
// it, and only two callers per frame reach the scaler, which is off at the
// original view (runtime_camera_policy.h): the client's view copy (return
// 0x8249A12C) and, V382, the builder's write-back (0x8249A1EC), where the
// title gets its own FOV back.
extern std::atomic<uint32_t> g_runtime_view_fov_active;
void RuntimeScaleClientViewFov(PPCContext& context, uint8_t* base) noexcept;

// Title functions with a runtime duty at entry: the presentation-interval
// selector (timing modes), the FOV setters, the owned-camera input observer,
// the guest frame-limiter boundary, the frame start and the input processor.
constexpr uint32_t kRuntimeFrameLimiterBoundary = 0x820E2058u;
// The title's frame driver on its producer thread, before the frame-pool wait
// and input processing (the plugin's NVIDIA Reflex sleep).
constexpr uint32_t kRuntimeFrameStart = 0x820DE198u;
// The title's input processor, called by the frame driver after the frame-pool
// wait (the plugin's input-to-present latency measurement).
constexpr uint32_t kRuntimeInputSample = 0x820E29D8u;
constexpr bool RuntimeFunctionEnterHasStaticDuty(uint32_t address) noexcept {
    return address == 0x8286FD68u ||
           (address >= 0x82389CA0u && address <= 0x82389CF8u) ||
           address == 0x8259D3B8u ||
           address == kRuntimeFrameLimiterBoundary || address == kRuntimeFrameStart ||
           address == kRuntimeInputSample;
}

// Development probes (PC probes, control-read watchpoints) only feed retired
// investigations; player builds compile them away entirely.
#ifndef DARKNESS_RUNTIME_DEVELOPMENT_PROBES
#define DARKNESS_RUNTIME_DEVELOPMENT_PROBES 0
#endif

// Per-title function-entry policy plus development observability. Functional
// changes are narrowly delegated to versioned runtime subsystems; diagnostics
// never synthesize guest work.
void RuntimeFunctionEnter(PPCContext& context, uint8_t* base, uint32_t address);
void RuntimePcProbe(PPCContext& context, uint8_t* base, uint32_t address);
void ConfigureRuntimeFrameCadenceDiagnostics(bool enabled) noexcept;
void ConfigureRuntimeOneVblankExperiment(bool enabled, bool immediate = false) noexcept;
void ConfigureRuntimeHitchDiagnostics(bool enabled) noexcept;
bool RuntimeHitchDiagnosticsEnabled() noexcept;
void RuntimeDb16Cyc();
bool RuntimeGeneratedAddressInRange(uint32_t address) noexcept;
bool RuntimeInterruptDeliveryAllowedAtFunction(uint32_t address) noexcept;
void RuntimeIndirectDispatch(PPCContext& context, uint8_t* base, uint32_t address);
void RuntimeTraceThreadStarted(uint32_t threadId, uint32_t guestObject, uint32_t startAddress,
                               uint32_t pcr, uint32_t teb, uint32_t tls);
void RuntimeTraceThreadTerminated(uint32_t threadId);
void RuntimeTraceImport(const char* name, PPCContext& context);
void RuntimeBeginGraphicsTrace(PPCContext& context);
void RuntimeBeginPostSwapTrace(PPCContext& context);
void RuntimeTraceWait(uint32_t handle, bool hasTimeout, int64_t timeout, uint32_t status,
                      PPCContext& context);
void RuntimeTracePostContentCompleted();
void RuntimeTraceControlRead(uint32_t address, uint32_t value, uint32_t function, uint32_t threadId);
// Thread snapshots in <game folder>\logs: a stop or freeze writes one for
// its report; the development milestones (startup, content loading) only
// with DARKNESS_MILESTONE_SNAPSHOTS=1.
void RuntimeWriteThreadSnapshot(const char* phase);
void RuntimeWriteCurrentThreadSnapshot(const char* phase);
bool RuntimeMilestoneSnapshotsEnabled();

#ifdef PPC_RUNTIME_FUNCTION_ENTER
#undef PPC_RUNTIME_FUNCTION_ENTER
#endif
#ifdef PPC_RUNTIME_INDIRECT_CALL
#undef PPC_RUNTIME_INDIRECT_CALL
#endif
#ifdef PPC_RUNTIME_MEMORY_READ
#undef PPC_RUNTIME_MEMORY_READ
#endif
#ifdef PPC_RUNTIME_PC_PROBE
#undef PPC_RUNTIME_PC_PROBE
#endif
#ifdef PPC_RUNTIME_DB16CYC
#undef PPC_RUNTIME_DB16CYC
#endif
#define PPC_RUNTIME_FUNCTION_ENTER(address, context, imageBase)                  \
    do {                                                                       \
        if ((address) == 0x82899BF0u &&                                        \
            ((context).lr == 0x8249A12Cu || (context).lr == 0x8249A1ECu) &&    \
            g_runtime_view_fov_active.load(std::memory_order_relaxed))         \
            RuntimeScaleClientViewFov(context, imageBase);                     \
        if (RuntimeFunctionEnterHasStaticDuty(address) ||                      \
            (g_runtime_entry_slow_mask.load(std::memory_order_relaxed) |       \
             g_runtime_pending_interrupt_count.load(std::memory_order_relaxed)))\
            RuntimeFunctionEnter(context, imageBase, address);                 \
    } while (0)
#define PPC_RUNTIME_INDIRECT_CALL(address, context, imageBase) \
    RuntimeIndirectDispatch(context, imageBase, static_cast<uint32_t>(address))
#if DARKNESS_RUNTIME_DEVELOPMENT_PROBES
#define PPC_RUNTIME_MEMORY_READ(instruction, address, value) \
    RuntimeTraceControlRead(address, value, instruction, CurrentGuestThreadId())
#define PPC_RUNTIME_PC_PROBE(address, context, imageBase) \
    RuntimePcProbe(context, imageBase, address)
#else
#define PPC_RUNTIME_MEMORY_READ(instruction, address, value) ((void)0)
#define PPC_RUNTIME_PC_PROBE(address, context, imageBase) ((void)0)
#endif
#define PPC_RUNTIME_DB16CYC() RuntimeDb16Cyc()
