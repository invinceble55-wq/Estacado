#include "runtime_function_trace.h"

#include "runtime_camera.h"
#include "runtime_fatal.h"
#include "runtime_frame_cadence.h"
#include "runtime_present_interval_experiment.h"

#include "runtime_memory_access.h"
#include "ppc_recomp_shared.h"
#include "runtime_objects.h"
#include "runtime_graphics.h"
#include "runtime_threads.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <immintrin.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

void RuntimeSetActivePpcContext(PPCContext* context, uint8_t* base, uint32_t guestFunction);
PPCContext* RuntimeActivePpcContext();
uint32_t RuntimeActiveGuestFunction();
uint32_t RuntimeActiveGuestLr();
void RuntimeWatchGpuFloatConstantSource(uint32_t guestAddress) noexcept;
void RuntimeWatchGpuTransformSource(uint32_t guestAddress) noexcept;
void RuntimeWatchGpuTransformInputSource(uint32_t guestAddress) noexcept;

thread_local uint32_t activeNonHelperGuestFunction{};

uint32_t RuntimeActiveNonHelperGuestFunction() noexcept {
    return activeNonHelperGuestFunction;
}

void RuntimeDb16Cyc() {
    _mm_pause();
    if (GuestRuntimeStopRequested()) throw GuestRuntimeStop{};
}

namespace {
constexpr size_t kFunctionHistory = 512;
constexpr size_t kImportHistory = 32;
constexpr size_t kDeepTraceLimit = 256;
constexpr size_t kPostSwapTraceLimit = 32768;
// The 33-frame broad sample is preserved under logs/. It established a stable
// frame path and now imposes enough lock/file overhead to distort live timing,
// so subsequent work uses the narrower title-frame observer below.
constexpr bool kPostSwapTraceEnabled = false;
// M5/M6 development probes are intentionally excluded from normal execution.
// They performed allocator/audio/content file I/O and multiple shared atomic
// updates on every generated function entry, reducing this release build to
// roughly seven title input samples per second. Keep the implementations for a
// narrowly reproduced regression, but preserve only correctness-critical stop,
// active-PC, interrupt-delivery, and indirect-target checks in the live path.
constexpr bool kLegacyFunctionTracingEnabled = false;
// Keep the sparse title frame/state probe independent from the legacy global
// tracer. It records only early frames, powers of two, and actual state changes,
// and is needed to span the complete front-end -> first-level transition.
// The complete front-end -> Tunnel frame/state history is preserved in the
// Probe69-72 evidence. Calling this observer at every generated function entry
// still performs multiple address/LR comparisons and occasionally synchronous
// file I/O, so keep it available only for a deliberately targeted build.
constexpr bool kTitleFrameStateProbeEnabled = false;
// Probe 69 reaches a stable black first-level render loop after every level
// archive has completed and the static entity table is populated.  Preserve a
// narrow discriminator for the next genuine-input run: it observes client
// initialization, the two current entity IDs, dynamic entity lifecycle, and
// sparse render/view callbacks.  It never changes guest state and deliberately
// avoids the retired broad per-function logger.
// The registry/player/view frontier probe produced the preserved Probe69-72
// captures (including the multi-megabyte lifecycle log). It is not part of the
// runtime contract and materially distorts the timing baseline when left in
// every generated function entry.
constexpr bool kLevelGameplayFrontierProbeEnabled = false;
// The XDF provider/worker probes established the complete causal chain from
// NtQueryFullAttributesFile("Content") through the type-2 provider, buffered
// consumer and EOF. Their verified output is preserved under logs/m6d_xdf_fix_*
// and they are intentionally retired from the per-generated-function hot path.
// Keeping the implementations here makes them easy to re-enable if this exact
// subsystem regresses without burdening unrelated graphics/content execution.
constexpr bool kCompletedXdfTraceEnabled = false;
// The prompt NaN producer probes established the exact frsqrte divergence and
// the corrected instruction semantics are now covered by focused tests. Keep
// the probe implementation available for a targeted regression, but do not
// scan every generated function entry during ordinary title execution.
constexpr bool kCompletedPromptNanTraceEnabled = false;
// This attribution is deliberately much smaller than the retired title-frame
// observer. Exact title addresses/LRs are compared at function entry; clock
// reads, atomics, guest-memory reads, formatting, and sparse file output occur
// only at the owned simulation/build/queue/submit boundaries. It never changes
// a guest timeout, event, queue, clock, or return value.
std::atomic<bool> frameCadenceAttributionEnabled{};
std::atomic<bool> runtimeHitchDiagnosticsEnabled{};

struct TraceThread {
    std::atomic<uint32_t> id{};
    std::atomic<uint32_t> guestObject{};
    std::atomic<uint32_t> start{};
    std::atomic<uint32_t> pcr{};
    std::atomic<uint32_t> teb{};
    std::atomic<uint32_t> tls{};
    std::atomic<uint32_t> lastFunction{};
    std::atomic<uint32_t> lastLr{};
    std::atomic<uint32_t> lastR1{};
    std::atomic<uint32_t> lastR13{};
    std::atomic<uint32_t> waitHandle{};
    std::atomic<int64_t> waitTimeout{};
    std::atomic<uint32_t> waitStatus{};
    std::atomic<uint64_t> functionCount{};
    std::atomic<uint64_t> pollCount{};
    std::atomic<uint32_t> functionWrite{};
    std::atomic<uint32_t> importWrite{};
    std::array<std::atomic<uint32_t>, kFunctionHistory> functions{};
    std::array<std::atomic<const char*>, kImportHistory> imports{};
};

std::mutex traceMutex;
std::vector<std::shared_ptr<TraceThread>> traceThreads;
thread_local std::shared_ptr<TraceThread> currentTrace;
std::atomic<bool> postContent{};
std::atomic<bool> postContentSnapshot{};
std::atomic<uint64_t> postContentPolls{};
std::atomic<uint64_t> postContentWaits{};
std::atomic<uint32_t> nanConstantCopySourceSequence{};
std::atomic<uint32_t> nanTransformInputSequence{};
std::mutex nanTransformInputMutex;
std::atomic<uint32_t> nanTransformUpstreamSequence{};
std::mutex nanTransformUpstreamMutex;
std::mutex nanConstantCopySourceMutex;
std::atomic<uint32_t> frsqrteProbeSequence{};
std::mutex frsqrteProbeMutex;
std::mutex watchMutex;
std::unordered_map<uint32_t, uint32_t> watchedValues;
std::mutex deepTraceMutex;
bool deepTraceStarted{};
bool deepTraceComplete{};
uint32_t deepTraceThread{};
size_t deepTraceEvents{};
std::mutex graphicsTraceMutex;
bool graphicsTraceStarted{};
bool graphicsTraceComplete{};
uint32_t graphicsTraceThread{};
size_t graphicsTraceEvents{};
std::atomic<uint32_t> tlsTransitionCount{};
std::atomic<uint32_t> allocatorWrapperProbeCount{};
std::atomic<uint32_t> allocatorInnerFailureCount{};
std::atomic<uint32_t> allocatorCorruptDescriptorCount{};
std::atomic<uint32_t> tlsInitializerProbeCount{};
std::mutex allocatorCriticalMutex;
struct AllocatorCriticalOwner {
    uint32_t thread{};
    uint32_t depth{};
};
std::unordered_map<uint32_t, AllocatorCriticalOwner> allocatorCriticalOwners;
std::atomic<uint32_t> allocatorCriticalSequence{};
std::atomic<uint32_t> allocatorLockProbeSequence{};
std::atomic<uint32_t> allocatorLifecycleProbeSequence{};
std::atomic<uint32_t> allocatorSuspiciousFreeSequence{};
std::atomic<uint32_t> allocatorFixedPoolFreeSequence{};
std::mutex allocatorFixedPoolFreeMutex;
std::atomic<uint64_t> allocatorGeneralFreeOrdinal{};
std::atomic<uint64_t> allocatorGeneralLifecycleOrdinal{};
std::atomic<uint32_t> allocatorGeneralDuplicateSequence{};
std::atomic<uint32_t> allocatorGeneralReallocationSequence{};
std::atomic<uint32_t> allocatorSmartPointerStaleSequence{};
std::atomic<uint32_t> allocatorSmartPointerSourceSequence{};
std::atomic<uint32_t> allocatorItemPointerStoreSequence{};
struct AllocatorItemPointerAssignment {
    uint32_t thread{};
    uint32_t function{};
    uint32_t lr{};
    uint32_t oldValue{};
    uint32_t value{};
    void* hostCaller{};
    uint32_t historyCount{};
    std::array<uint32_t, 24> history{};
};
std::mutex allocatorItemPointerAssignmentMutex;
std::unordered_map<uint32_t, AllocatorItemPointerAssignment>
    allocatorItemPointerAssignments;
std::unordered_map<uint32_t, uint32_t> allocatorTargetReferenceItems;
std::unordered_map<uint32_t, uint32_t> allocatorTargetReferenceControls;
std::atomic<uint32_t> allocatorTargetReferenceLifecycleSequence{};
std::mutex allocatorTargetReferenceLifecycleLogMutex;
std::atomic<uint32_t> allocatorOwnerConsumerOrderSequence{};
std::mutex allocatorOwnerConsumerOrderLogMutex;
std::atomic<uint32_t> allocatorReferenceProviderSequence{};
std::mutex allocatorReferenceProviderLogMutex;
std::atomic<uint32_t> allocatorRegistryProducerSequence{};
std::mutex allocatorRegistryProducerLogMutex;
struct AllocatorRegistryCloneStats {
    bool active{};
    std::string path;
    uint32_t cloneCalls{};
    uint32_t rootSources{};
    uint32_t proxySources{};
    uint32_t otherSources{};
    uint32_t deepCopyCalls{};
    uint32_t deepRootSources{};
    uint32_t deepProxySources{};
    uint32_t deepOtherSources{};
};
thread_local AllocatorRegistryCloneStats allocatorRegistryCloneStats;
struct AllocatorSharedControlEvent {
    uint64_t ordinal{};
    uint32_t kind{};
    uint32_t thread{};
    uint32_t function{};
    uint32_t lr{};
    uint32_t outer{};
    uint32_t source{};
    uint32_t destination{};
    uint32_t control{};
    uint32_t count{};
    uint32_t backing{};
    uint32_t size{};
    uint32_t historyCount{};
    std::array<uint32_t, 24> history{};
};
std::mutex allocatorSharedControlMutex;
std::vector<AllocatorSharedControlEvent> allocatorSharedControlEvents;
uint64_t allocatorSharedControlOrdinal{};
std::atomic<uint32_t> allocatorSharedControlDumpSequence{};
std::mutex allocatorSharedControlDumpLogMutex;
struct AllocatorGeneralAllocationRequest {
    uint32_t allocator{};
    uint32_t size{};
    uint32_t alignment{};
    uint32_t caller{};
    uint32_t stack{};
};
thread_local std::vector<AllocatorGeneralAllocationRequest>
    allocatorGeneralAllocationRequests;
struct AllocatorGeneralPointerState {
    bool known{};
    bool allocated{};
    uint64_t allocationOrdinal{};
    uint32_t allocationThread{};
    uint32_t allocationCaller{};
    uint32_t allocationSize{};
    uint32_t allocationAlignment{};
    uint32_t allocationStack{};
    uint64_t freeOrdinal{};
    uint32_t freeThread{};
    uint32_t freeCaller{};
    uint32_t freeStack{};
    uint32_t freeR3{};
    uint32_t freeR28{};
    uint32_t freeR30{};
    uint32_t freeR31{};
};
std::mutex allocatorGeneralLifecycleMutex;
std::unordered_map<uint32_t, AllocatorGeneralPointerState>
    allocatorGeneralPointers;
std::atomic<uint64_t> allocatorFixedPoolLifecycleOrdinal{};
std::atomic<uint32_t> allocatorFixedPoolDuplicateSequence{};
struct AllocatorGeneralFreeContext {
    uint64_t ordinal{};
    uint32_t stack{};
    uint32_t pointer{};
    uint32_t caller{};
};
thread_local std::array<AllocatorGeneralFreeContext, 64> allocatorGeneralFreeHistory{};
thread_local uint32_t allocatorGeneralFreeHistoryWrite{};
thread_local uint32_t allocatorFixedPoolAllocationCaller{};
struct AllocatorFixedPoolNodeState {
    bool known{};
    bool allocated{};
    uint64_t allocationOrdinal{};
    uint32_t allocationThread{};
    uint32_t allocationCaller{};
    uint64_t freeOrdinal{};
    uint32_t freeThread{};
    uint32_t freeCaller{};
    uint32_t freeGeneralPointer{};
    uint32_t freeGeneralCaller{};
    uint32_t freeStack{};
};
std::mutex allocatorFixedPoolLifecycleMutex;
std::unordered_map<uint32_t, AllocatorFixedPoolNodeState> allocatorFixedPoolNodes;
std::atomic<bool> allocatorHeaderCorruptionSeen{};
std::atomic<uint32_t> allocatorHeaderStoreSequence{};
std::atomic<bool> mainUpdateTraceClaimed{};
thread_local bool mainUpdateTraceActive{};
thread_local uint32_t mainUpdateTraceObject{};
thread_local uint32_t mainUpdateTraceSequence{};
constexpr bool kMainUpdatePathTraceEnabled = false;
std::atomic<uint64_t> graphicsPollHelperSequence{};
std::atomic<uint32_t> audioSingletonProbeSequence{};
std::atomic<uint32_t> audioSingletonAllocator{};
std::atomic<uint32_t> audioInitializerIndirectSequence{};
std::atomic<bool> audioInitializerPathActive{};
std::atomic<uint32_t> audioInitializerPathSequence{};
std::mutex postSwapTraceMutex;
bool postSwapTraceStarted{};
bool postSwapTraceComplete{};
uint32_t postSwapTraceThread{};
size_t postSwapTraceEvents{};
std::unordered_map<uint64_t, uint32_t> postSwapFunctionOccurrences;
std::mutex rootUpdateListTraceMutex;
uint32_t rootUpdateListTraceSequence{};
std::mutex titleFrameProbeMutex;
bool titleFrameProbeStarted{};
bool titleFrameProbeComplete{};
uint32_t titleFrameProbeThread{};
uint32_t titleFrameProbeSequence{};
bool titleFrameProbeLogDetails{};
bool titleFrameProbeHasLastState{};
std::array<uint32_t, 15> titleFrameProbeLastState{};

using FrameCadenceClock = std::chrono::steady_clock;
struct FrameCadenceThreadState {
    darkness::diagnostics::PrebuildWaitBracket prebuildWait{};
    FrameCadenceClock::time_point prebuildWaitStart{};
    uint32_t prebuildWaitQueue{};
    bool buildActive{};
    FrameCadenceClock::time_point buildStart{};
    uint32_t buildMain{};
    bool completionAcquireActive{};
    bool completionQueueEmpty{};
    bool completionWaitObserved{};
    FrameCadenceClock::time_point completionAcquireStart{};
    uint32_t completionQueue{};
    uint32_t completionEvent{};
    uint32_t completionWaitStatus{};
    int64_t completionGuestTimeout{};
    int64_t completionWaitDurationUs{};
};
thread_local FrameCadenceThreadState frameCadenceThreadState{};
std::atomic<uint64_t> frameCadenceBuildEntries{};
std::atomic<uint64_t> frameCadenceControllerEntries{};
std::atomic<uint64_t> frameCadencePrebuildWaitEntries{};
std::atomic<uint64_t> frameCadenceEnqueues{};
std::atomic<uint64_t> frameCadencePoolInitialAcquires{};
std::atomic<uint64_t> frameCadencePoolRetryAcquires{};
std::atomic<uint64_t> frameCadenceAcquireEntries{};
std::atomic<uint64_t> frameCadenceEmptyEntries{};
std::atomic<uint64_t> frameCadenceWaits{};
std::atomic<uint64_t> frameCadenceTimeouts{};
std::atomic<uint64_t> frameCadenceSubmits{};
std::atomic<uint64_t> frameCadenceSimulationTicks{};
std::atomic<uint64_t> frameCadenceSimulationSteps{};
std::atomic<uint64_t> frameCadenceSimulationQueuedDrains{};
std::array<std::atomic<uint64_t>, 4> frameCadenceWorldAttempts{};
std::array<std::atomic<uint64_t>, 4> frameCadenceWorldEmpty{};
std::array<std::atomic<uint64_t>, 4> frameCadenceWorldConsumed{};
std::atomic<uint64_t> frameCadenceConsumerReturns{};
thread_local uint64_t frameCadenceThreadConsumerReturns{};
struct FrameCadenceClientSteps {
    uint32_t world{};
    uint64_t epoch{};
    uint64_t returns{};
    uint64_t requested{};
    uint64_t verified{};
    uint64_t mismatches{};
};
thread_local FrameCadenceClientSteps frameCadenceThreadClientSteps{};
std::atomic<bool> frameCadenceLogStarted{};
std::mutex frameCadenceLogMutex;
constexpr const char* kFrameCadenceAttributionPath =
    "logs/pc_frame_cadence_attribution.log";

std::mutex levelGameplayFrontierMutex;
std::atomic<uint32_t> levelGameplayClient{};
std::atomic<uint64_t> levelGameplayFrameEntries{};
std::atomic<uint64_t> levelGameplayViewEntries{};
std::atomic<uint64_t> levelGameplayLookupEntries{};
std::atomic<uint64_t> levelGameplaySpawnEntries{};
std::atomic<uint64_t> levelGameplayRemoveEntries{};
std::atomic<uint64_t> levelGameplayInitEntries{};
std::atomic<uint64_t> levelGameplayQueryEntries{};
std::atomic<uint64_t> levelGameplayComponentIdEntries{};
std::atomic<uint64_t> levelGameplayActivationEntries{};
std::atomic<uint64_t> levelGameplayComponentBindEntries{};
uint32_t levelGameplayFrontierSequence{};
std::atomic<uint32_t> levelGameplayLastCurrent0{UINT32_MAX};
std::atomic<uint32_t> levelGameplayLastCurrent1{UINT32_MAX};
std::atomic<uint32_t> levelGameplayLastClientFlags{UINT32_MAX};
std::atomic<uint32_t> levelGameplayLastManagerCount{UINT32_MAX};
std::atomic<uint32_t> levelGameplayLastRegistryFirst{UINT32_MAX};
std::atomic<uint32_t> levelGameplayLastRegistryFirstChildList{UINT32_MAX};
std::atomic<uint32_t> levelGameplayLastSelectedId{UINT32_MAX};
std::atomic<uint32_t> levelGameplayLastViewObject{UINT32_MAX};
std::mutex levelRegistryUpdateTraceMutex;
std::atomic<uint64_t> levelRegistryTransportPumpEntries{};
std::atomic<uint64_t> levelRegistryMessageEntries{};
std::atomic<uint64_t> levelRegistryListenerDispatchEntries{};
std::atomic<uint32_t> levelRegistryLastDispatchCallee{UINT32_MAX};
std::atomic<uint32_t> levelRegistryLastDispatchListenerVtable{UINT32_MAX};
std::atomic<uint32_t> levelRegistryLastDispatchMessageType{UINT32_MAX};
std::atomic<bool> levelRegistryUpdateTraceComplete{};
uint32_t levelRegistryUpdateTraceSequence{};
thread_local bool levelRegistryUpdateActive{};
thread_local uint64_t levelRegistryActiveMessageOrdinal{};
thread_local uint32_t levelRegistryActiveClient{};

constexpr uint32_t kLevelGameplayClientVtable = 0x820807E0u;
constexpr uint32_t kLevelEntityTableVtable = 0x820634CCu;
constexpr uint32_t kLevelRegistryVtable = 0x82095798u;
constexpr uint32_t kLevelPlayerObjectName = 0x8206E2D8u;
constexpr uint32_t kLevelGameObjectName = 0x8206E2E4u;
constexpr uint32_t kLevelWorldSpawnDescriptor = 0x82A42528u;
constexpr uint32_t kLevelPlayerStartDescriptor = 0x82A42218u;
constexpr uint32_t kLevelCharNpcDescriptor = 0x82A40D58u;
constexpr std::array<uint32_t, 10> kLevelRpgObjectVtables{
    0x82069690u, 0x82069758u, 0x82069820u, 0x82069918u, 0x82069A10u,
    0x82069B08u, 0x82069C00u, 0x8206E310u, 0x8206E7E0u, 0x8206F9C8u,
};
constexpr uint32_t kLevelGameplayTraceLimit = 4096u;
constexpr const char* kLevelGameplayFrontierPath =
    "logs/m9_level_gameplay_frontier_probe.log";
constexpr uint32_t kLevelRegistryUpdateTraceLimit = 2048u;
constexpr const char* kLevelRegistryUpdateTracePath =
    "logs/m9_level_registry_update_probe.log";

constexpr uint32_t kTitleFrameProbeLimit = 1u << 20;
constexpr const char* kTitleFrameProbePath = "logs/m9_level_frame_state_probe.log";

std::atomic<uint32_t> xdfResolutionTraceSequence{};
thread_local std::string activeXdfResolutionPath;
thread_local uint32_t activeXdfResolutionOutput{};
thread_local uint32_t activeGameContextTextStream{};
std::atomic<uint32_t> threadVirtualDispatchTraceSequence{};
std::mutex gameContextWorkerFlagTraceMutex;
std::atomic<uint32_t> gameContextWorkerObject{};
std::atomic<uint32_t> gameContextWorkerLastFlags{};
uint32_t gameContextWorkerFlagTraceSequence{};
std::unordered_map<uint64_t, uint32_t> gameContextWorkerCandidateOccurrences;
std::mutex gameContextManagerCleanupTraceMutex;
uint32_t gameContextManagerCleanupTraceSequence{};
std::mutex gameContextEventTraceMutex;
uint32_t gameContextEventTraceSequence{};
std::mutex gameContextBufferedReadTraceMutex;
uint32_t gameContextBufferedReadTraceSequence{};
uint32_t gameContextBufferedReadLastBuffer{};
std::unordered_map<uint32_t, uint32_t> gameContextBufferedReadOccurrences;
std::mutex gameContextConsumerOwnerTraceMutex;
uint32_t gameContextConsumerOwnerTraceSequence{};
std::unordered_map<uint32_t, uint32_t> gameContextConsumerOwnerOccurrences;
std::unordered_map<uint32_t, uint32_t> gameContextConsumerOwnerType2Occurrences;
std::mutex xdfProviderSelectionTraceMutex;
std::atomic<uint32_t> xdfProviderSelectionTraceSequence{};
thread_local bool xdfProviderDeepTraceActive{};
thread_local uint32_t xdfProviderDeepTraceEvents{};

constexpr const char* kGameContextWorkerFlagTracePath =
    "logs/m6d_gamecontext_worker_flag_trace.log";
constexpr const char* kGameContextManagerCleanupTracePath =
    "logs/m6d_gamecontext_manager_cleanup_trace.log";
constexpr const char* kGameContextEventTracePath =
    "logs/m6d_gamecontext_event_trace.log";
constexpr const char* kGameContextBufferedReadTracePath =
    "logs/m6d_gamecontext_buffered_read_trace.log";
constexpr const char* kGameContextConsumerOwnerTracePath =
    "logs/m6d_gamecontext_consumer_owner_trace.log";
constexpr const char* kXdfProviderSelectionTracePath =
    "logs/m6d_xdf_provider_selection_trace.log";
constexpr const char* kXdfProviderDeepTracePath =
    "logs/m6d_xdf_provider_deep_trace.log";

std::string ReadStarbreezeNarrowString(uint8_t* base, uint32_t object);
std::string ReadGuestNarrowCString(uint8_t* base, uint32_t address);

void AppendXdfProviderDeepTrace(PPCContext& context, uint8_t* base, uint32_t address) {
    if (!xdfProviderDeepTraceActive || xdfProviderDeepTraceEvents >= 256u) return;
    std::ofstream out(kXdfProviderDeepTracePath,
                      xdfProviderDeepTraceEvents ? std::ios::app : std::ios::trunc);
    out << "XDF_PROVIDER_CALL sequence=" << xdfProviderDeepTraceEvents++
        << " thread=" << CurrentGuestThreadId() << " function=0x" << std::hex
        << address << " caller_lr=0x" << context.lr << " r3=0x"
        << context.r3.u32 << " r4=0x" << context.r4.u32 << " r5=0x"
        << context.r5.u32 << " r6=0x" << context.r6.u32 << " r27=0x"
        << context.r27.u32 << " r28=0x" << context.r28.u32 << " r29=0x"
        << context.r29.u32 << " r30=0x" << context.r30.u32 << " r31=0x"
        << context.r31.u32;
    if (address == 0x821F3E50u) {
        out << " lhs=\"" << ReadStarbreezeNarrowString(base, context.r3.u32)
            << "\" rhs=\"" << ReadGuestNarrowCString(base, context.r4.u32)
            << "\"";
    }
    out << std::dec << '\n';
}

void AppendGameContextWorkerFlagTrace(PPCContext& context, uint8_t* base,
                                      uint32_t address) {
    constexpr uint32_t kWorkerEntry = 0x8220DC10u;
    constexpr uint32_t kWorkerVtable = 0x820659A0u;
    constexpr uint32_t kGameContextLastChunk = 0xEEu;
    constexpr uint32_t kTraceLimit = 4096u;

    uint32_t worker = gameContextWorkerObject.load(std::memory_order_relaxed);
    if (address == kWorkerEntry && context.r3.u32 &&
        PPC_LOAD_U32(context.r3.u32) == kWorkerVtable &&
        PPC_LOAD_U32(context.r3.u32 + 0x4Cu) == kGameContextLastChunk) {
        uint32_t expected = 0;
        if (gameContextWorkerObject.compare_exchange_strong(
                expected, context.r3.u32, std::memory_order_relaxed)) {
            worker = context.r3.u32;
            std::lock_guard<std::mutex> lock(gameContextWorkerFlagTraceMutex);
            gameContextWorkerLastFlags = PPC_LOAD_U32(worker + 4u);
            gameContextWorkerFlagTraceSequence = 0;
            gameContextWorkerCandidateOccurrences.clear();
            std::filesystem::create_directories("logs");
            std::ofstream out(kGameContextWorkerFlagTracePath, std::ios::trunc);
            out << "WORKER_FLAG sequence=" << gameContextWorkerFlagTraceSequence++
                << " stage=capture thread=" << CurrentGuestThreadId()
                << " function=0x" << std::hex << address << " caller_lr=0x"
                << context.lr << " object=0x" << worker << " flags=0x"
                << gameContextWorkerLastFlags << " last_chunk=0x"
                << PPC_LOAD_U32(worker + 0x4Cu) << std::dec << '\n';
        } else {
            worker = expected;
        }
    }
    if (!worker) return;

    const uint32_t flags = PPC_LOAD_U32(worker + 4u);
    const bool flagsChanged = flags != gameContextWorkerLastFlags;
    const bool workerArgument =
        context.r3.u32 == worker || context.r3.u32 == worker + 4u ||
        context.r4.u32 == worker || context.r4.u32 == worker + 4u ||
        context.r5.u32 == worker || context.r5.u32 == worker + 4u ||
        context.r6.u32 == worker || context.r6.u32 == worker + 4u;
    const bool knownFlagMethod = workerArgument &&
        (address == 0x820ED4C0u || address == 0x820ED4D0u ||
         address == 0x820ED4E0u || address == 0x820ED4E8u ||
         address == 0x821FB950u || address == 0x821FB970u ||
         address == 0x821FC480u);

    bool logCandidate = false;
    uint32_t occurrence = 0;
    if (workerArgument) {
        const uint64_t key = (uint64_t(address) << 32) | context.lr;
        std::lock_guard<std::mutex> lock(gameContextWorkerFlagTraceMutex);
        occurrence = ++gameContextWorkerCandidateOccurrences[key];
        logCandidate = occurrence <= 2u || (occurrence & (occurrence - 1u)) == 0;
    }
    if (!flagsChanged && !knownFlagMethod && !logCandidate) return;

    std::lock_guard<std::mutex> lock(gameContextWorkerFlagTraceMutex);
    if (gameContextWorkerFlagTraceSequence >= kTraceLimit) return;
    std::filesystem::create_directories("logs");
    std::ofstream out(kGameContextWorkerFlagTracePath, std::ios::app);
    out << "WORKER_FLAG sequence=" << gameContextWorkerFlagTraceSequence++
        << " stage=" << (flagsChanged ? "flags-changed" :
                           knownFlagMethod ? "flag-method" : "object-candidate")
        << " thread=" << CurrentGuestThreadId() << " function=0x" << std::hex
        << address << " caller_lr=0x" << context.lr << " object=0x" << worker
        << " flags_before_observer=0x" << gameContextWorkerLastFlags
        << " flags=0x" << flags << " r1=0x" << context.r1.u32
        << " r3=0x" << context.r3.u32 << " r4=0x" << context.r4.u32
        << " r5=0x" << context.r5.u32 << " r6=0x" << context.r6.u32
        << " r11=0x" << context.r11.u32 << " r30=0x" << context.r30.u32
        << " r31=0x" << context.r31.u32 << std::dec;
    if (occurrence) out << " occurrence=" << occurrence;
    out << '\n';
    gameContextWorkerLastFlags = flags;
}

void AppendGameContextManagerCleanupTrace(PPCContext& context, uint8_t* base,
                                          uint32_t address) {
    if (address != 0x821FF068u ||
        !gameContextWorkerObject.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(gameContextManagerCleanupTraceMutex);
    constexpr uint32_t kTraceLimit = 64u;
    if (gameContextManagerCleanupTraceSequence >= kTraceLimit) return;

    const uint32_t root = PPC_LOAD_U32(0x82A690F8u);
    const uint32_t manager = root ? PPC_LOAD_U32(root + 12u) : 0;
    const uint32_t worker =
        gameContextWorkerObject.load(std::memory_order_relaxed);
    std::filesystem::create_directories("logs");
    std::ofstream out(kGameContextManagerCleanupTracePath,
                      gameContextManagerCleanupTraceSequence ? std::ios::app
                                                             : std::ios::trunc);
    out << "MANAGER_CLEANUP sequence=" << gameContextManagerCleanupTraceSequence++
        << " thread=" << CurrentGuestThreadId() << " function=0x" << std::hex
        << address << " caller_lr=0x" << context.lr << " r1=0x"
        << context.r1.u32 << " r3=0x" << context.r3.u32 << " r4=0x"
        << context.r4.u32 << " r5=0x" << context.r5.u32 << " r30=0x"
        << context.r30.u32 << " r31=0x" << context.r31.u32 << " root=0x"
        << root << " manager=0x" << manager << " manager_thread=0x"
        << (manager ? PPC_LOAD_U32(manager + 156u) : 0)
        << " manager_primary=0x" << (manager ? PPC_LOAD_U32(manager + 160u) : 0)
        << " manager_secondary=0x" << (manager ? PPC_LOAD_U32(manager + 164u) : 0)
        << " worker=0x" << worker << " worker_flags=0x"
        << (worker ? PPC_LOAD_U32(worker + 4u) : 0)
        << " worker_last_chunk=0x" << (worker ? PPC_LOAD_U32(worker + 76u) : 0)
        << " worker_cursor=0x" << (worker ? PPC_LOAD_U32(worker + 88u) : 0)
        << " worker_base_position=0x" << (worker ? PPC_LOAD_U64(worker + 304u) : 0)
        << " worker_active_buffer=0x" << (worker ? PPC_LOAD_U32(worker + 312u) : 0)
        << " queue_d4=0x" << (worker ? PPC_LOAD_U32(worker + 0xD4u) : 0)
        << " queue_d8=0x" << (worker ? PPC_LOAD_U32(worker + 0xD8u) : 0)
        << " queue_dc=0x" << (worker ? PPC_LOAD_U32(worker + 0xDCu) : 0)
        << " queue_e0=0x" << (worker ? PPC_LOAD_U32(worker + 0xE0u) : 0);
    const uint32_t activeBuffer = worker ? PPC_LOAD_U32(worker + 312u) : 0;
    if (activeBuffer) {
        out << " active_data=0x" << PPC_LOAD_U32(activeBuffer)
            << " active_length=0x" << PPC_LOAD_U32(activeBuffer + 4u)
            << " active_consumed=0x" << PPC_LOAD_U32(activeBuffer + 8u);
    }
    out << std::dec << '\n';
}

void AppendGameContextEventTrace(PPCContext& context, uint8_t* base,
                                 uint32_t address) {
    constexpr uint32_t kNtSetEventWrapper = 0x828A9F20u;
    constexpr uint32_t kTraceLimit = 4096u;
    if (address != kNtSetEventWrapper) return;

    const uint32_t worker =
        gameContextWorkerObject.load(std::memory_order_relaxed);
    if (!worker) return;

    const uint32_t handle = context.r3.u32;
    if (handle != 0x35u && handle != 0x36u && handle != 0x3Cu) return;

    std::lock_guard<std::mutex> lock(gameContextEventTraceMutex);
    if (gameContextEventTraceSequence >= kTraceLimit) return;
    std::filesystem::create_directories("logs");
    std::ofstream out(kGameContextEventTracePath,
                      gameContextEventTraceSequence ? std::ios::app
                                                    : std::ios::trunc);
    out << "GAMECONTEXT_EVENT sequence=" << gameContextEventTraceSequence++
        << " thread=" << CurrentGuestThreadId() << " function=0x" << std::hex
        << address << " caller_lr=0x" << context.lr << " handle=0x" << handle
        << " r1=0x" << context.r1.u32 << " worker=0x" << worker
        << " worker_flags=0x" << PPC_LOAD_U32(worker + 4u) << std::dec << '\n';
}

void AppendGameContextBufferedReadTrace(PPCContext& context, uint8_t* base,
                                        uint32_t address) {
    constexpr uint32_t kWorkerVtable = 0x820659A0u;
    constexpr uint32_t kSignalEvent = 0x821FC480u;
    constexpr uint32_t kReadVariable = 0x8220D788u;
    constexpr uint32_t kConsumeBuffer = 0x8220E070u;
    constexpr uint32_t kRead8 = 0x8220EED0u;
    constexpr uint32_t kRead16 = 0x8220F220u;
    constexpr uint32_t kRead48 = 0x8220F680u;
    constexpr uint32_t kTraceLimit = 4096u;

    const uint32_t worker =
        gameContextWorkerObject.load(std::memory_order_relaxed);
    if (!worker || PPC_LOAD_U32(worker) != kWorkerVtable) return;

    if (address == kSignalEvent) {
        if (context.r3.u32 != worker + 0xE4u &&
            context.r3.u32 != worker + 0x108u) {
            return;
        }
        std::lock_guard<std::mutex> lock(gameContextBufferedReadTraceMutex);
        if (gameContextBufferedReadTraceSequence >= kTraceLimit) return;
        std::filesystem::create_directories("logs");
        std::ofstream out(kGameContextBufferedReadTracePath,
                          gameContextBufferedReadTraceSequence ? std::ios::app
                                                               : std::ios::trunc);
        out << "BUFFERED_READ sequence=" << gameContextBufferedReadTraceSequence++
            << " stage=signal thread=" << CurrentGuestThreadId()
            << " function=0x" << std::hex << address << " caller_lr=0x"
            << context.lr << " event_object=0x" << context.r3.u32
            << " event_role="
            << (context.r3.u32 == worker + 0xE4u ? "producer-wake"
                                                 : "reader-wake")
            << " worker_flags=0x" << PPC_LOAD_U32(worker + 4u) << std::dec
            << '\n';
        return;
    }

    if (address == kConsumeBuffer) {
        if (context.r3.u32 != worker) return;
        std::lock_guard<std::mutex> lock(gameContextBufferedReadTraceMutex);
        if (gameContextBufferedReadTraceSequence >= kTraceLimit) return;
        const uint32_t occurrence =
            ++gameContextBufferedReadOccurrences[address];
        const uint32_t activeBuffer = PPC_LOAD_U32(worker + 312u);
        std::filesystem::create_directories("logs");
        std::ofstream out(kGameContextBufferedReadTracePath,
                          gameContextBufferedReadTraceSequence ? std::ios::app
                                                               : std::ios::trunc);
        out << "BUFFERED_READ sequence=" << gameContextBufferedReadTraceSequence++
            << " stage=consume-entry thread=" << CurrentGuestThreadId()
            << " function=0x" << std::hex << address << " caller_lr=0x"
            << context.lr << " occurrence=0x" << occurrence
            << " requested_position=0x" << context.r4.u64 << " output=0x"
            << context.r5.u32 << " bytes=0x" << context.r6.u32
            << " worker_flags=0x" << PPC_LOAD_U32(worker + 4u)
            << " worker_base_position=0x" << PPC_LOAD_U64(worker + 304u)
            << " active_buffer=0x" << activeBuffer << " queue_d4=0x"
            << PPC_LOAD_U32(worker + 0xD4u) << " queue_d8=0x"
            << PPC_LOAD_U32(worker + 0xD8u) << " queue_dc=0x"
            << PPC_LOAD_U32(worker + 0xDCu) << " queue_e0=0x"
            << PPC_LOAD_U32(worker + 0xE0u);
        if (activeBuffer) {
            out << " active_data=0x" << PPC_LOAD_U32(activeBuffer)
                << " active_length=0x" << PPC_LOAD_U32(activeBuffer + 4u)
                << " active_consumed=0x" << PPC_LOAD_U32(activeBuffer + 8u);
        }
        out << std::dec << '\n';
        return;
    }

    if (address != kReadVariable && address != kRead8 &&
        address != kRead16 && address != kRead48) {
        return;
    }

    const uint32_t wrapper = address == kRead8 ? context.r3.u32 : context.r4.u32;
    if (!wrapper || PPC_LOAD_U32(wrapper + 20u) != 2u) return;
    const uint32_t stream = PPC_LOAD_U32(wrapper + 16u);
    if (!stream) return;

    const uint32_t activeBuffer = PPC_LOAD_U32(worker + 312u);
    std::lock_guard<std::mutex> lock(gameContextBufferedReadTraceMutex);
    const uint32_t occurrence = ++gameContextBufferedReadOccurrences[address];
    const bool bufferChanged = activeBuffer != gameContextBufferedReadLastBuffer;
    const bool sample = occurrence <= 64u ||
                        (occurrence & (occurrence - 1u)) == 0u;
    gameContextBufferedReadLastBuffer = activeBuffer;
    if (!bufferChanged && !sample) return;
    if (gameContextBufferedReadTraceSequence >= kTraceLimit) return;

    std::filesystem::create_directories("logs");
    std::ofstream out(kGameContextBufferedReadTracePath,
                      gameContextBufferedReadTraceSequence ? std::ios::app
                                                           : std::ios::trunc);
    out << "BUFFERED_READ sequence=" << gameContextBufferedReadTraceSequence++
        << " stage=entry thread=" << CurrentGuestThreadId() << " function=0x"
        << std::hex << address << " caller_lr=0x" << context.lr
        << " occurrence=0x" << occurrence << " wrapper=0x" << wrapper
        << " stream=0x" << stream << " stream_position=0x"
        << PPC_LOAD_U64(stream + 24u) << " file_index=0x"
        << PPC_LOAD_U32(stream + 40u) << " output_r3=0x" << context.r3.u32
        << " output_r4=0x" << context.r4.u32 << " worker_flags=0x"
        << PPC_LOAD_U32(worker + 4u) << " worker_last_chunk=0x"
        << PPC_LOAD_U32(worker + 76u) << " worker_cursor=0x"
        << PPC_LOAD_U32(worker + 88u) << " worker_base_position=0x"
        << PPC_LOAD_U64(worker + 304u) << " active_buffer=0x" << activeBuffer;
    if (activeBuffer) {
        out << " active_data=0x" << PPC_LOAD_U32(activeBuffer)
            << " active_length=0x" << PPC_LOAD_U32(activeBuffer + 4u)
            << " active_consumed=0x" << PPC_LOAD_U32(activeBuffer + 8u);
    }
    out << std::dec << " buffer_changed=" << (bufferChanged ? 1 : 0) << '\n';
}

void AppendGameContextConsumerOwnerTrace(PPCContext& context, uint8_t* base,
                                         uint32_t address) {
    constexpr uint32_t kWorkerVtable = 0x820659A0u;
    constexpr uint32_t kTraceLimit = 4096u;
    constexpr std::array<uint32_t, 13> kConsumerOwners = {
        0x8278A588u, 0x82143000u, 0x8287A1B0u, 0x827899C0u,
        0x82626588u, 0x8262FAE0u, 0x821F6A40u, 0x82387C68u,
        0x8238DB50u, 0x82740838u, 0x8276D1D8u, 0x827BB648u,
        0x827BD428u,
    };
    if (std::find(kConsumerOwners.begin(), kConsumerOwners.end(), address) ==
        kConsumerOwners.end()) {
        return;
    }

    const uint32_t worker =
        gameContextWorkerObject.load(std::memory_order_relaxed);
    if (!worker || PPC_LOAD_U32(worker) != kWorkerVtable ||
        PPC_LOAD_U32(worker + 4u) != 1u) {
        return;
    }

    std::lock_guard<std::mutex> lock(gameContextConsumerOwnerTraceMutex);
    const uint32_t occurrence = ++gameContextConsumerOwnerOccurrences[address];
    uint32_t wrapper = 0;
    if (address == 0x82143000u || address == 0x8238DB50u) {
        wrapper = context.r3.u32;
    } else if (address == 0x821F6A40u || address == 0x82740838u) {
        wrapper = context.r4.u32;
    } else if (address == 0x8287A1B0u && context.r3.u32) {
        wrapper = PPC_LOAD_U32(context.r3.u32 + 12u);
    }
    const uint32_t wrapperType = wrapper ? PPC_LOAD_U32(wrapper + 20u) : 0;
    const uint32_t type2Occurrence = wrapperType == 2u
                                         ? ++gameContextConsumerOwnerType2Occurrences[address]
                                         : 0u;
    const uint32_t sampleOccurrence = type2Occurrence ? type2Occurrence : occurrence;
    const uint32_t initialSamples = type2Occurrence ? 128u : 4u;
    const bool sample = sampleOccurrence <= initialSamples ||
                        (sampleOccurrence & (sampleOccurrence - 1u)) == 0u;
    if (!sample || gameContextConsumerOwnerTraceSequence >= kTraceLimit) return;

    const uint32_t root = PPC_LOAD_U32(0x82A690F8u);
    const uint32_t manager = root ? PPC_LOAD_U32(root + 12u) : 0;
    std::filesystem::create_directories("logs");
    std::ofstream out(kGameContextConsumerOwnerTracePath,
                      gameContextConsumerOwnerTraceSequence ? std::ios::app
                                                            : std::ios::trunc);
    out << "CONSUMER_OWNER sequence=" << gameContextConsumerOwnerTraceSequence++
        << " thread=" << CurrentGuestThreadId() << " function=0x" << std::hex
        << address << " caller_lr=0x" << context.lr << " occurrence=0x"
        << occurrence << " r3=0x" << context.r3.u64 << " r4=0x"
        << context.r4.u64 << " r5=0x" << context.r5.u64 << " r6=0x"
        << context.r6.u64 << " r7=0x" << context.r7.u64 << " r8=0x"
        << context.r8.u64 << " r9=0x" << context.r9.u64 << " r10=0x"
        << context.r10.u64 << " wrapper=0x" << wrapper << " wrapper_type=0x"
        << wrapperType << " type2_occurrence=0x" << type2Occurrence
        << " stream=0x" << (wrapper ? PPC_LOAD_U32(wrapper + 16u) : 0);
    const uint32_t stream = wrapper ? PPC_LOAD_U32(wrapper + 16u) : 0;
    if (wrapperType == 2u && stream) {
        out << " stream_mode=0x" << PPC_LOAD_U32(stream + 12u)
            << " stream_position=0x" << PPC_LOAD_U64(stream + 24u)
            << " file_index=0x" << PPC_LOAD_U32(stream + 40u);
    }
    out << " worker=0x" << worker << " worker_cursor=0x"
        << PPC_LOAD_U32(worker + 88u) << " worker_base_position=0x"
        << PPC_LOAD_U64(worker + 304u) << " worker_active_buffer=0x"
        << PPC_LOAD_U32(worker + 312u) << " manager=0x" << manager
        << " manager_thread=0x" << (manager ? PPC_LOAD_U32(manager + 156u) : 0)
        << " manager_worker=0x" << (manager ? PPC_LOAD_U32(manager + 164u) : 0)
        << std::dec << '\n';
}

std::string ReadStarbreezeNarrowString(uint8_t* base, uint32_t object) {
    if (!object) return {};
    const uint32_t storage = PPC_LOAD_U32(object + 4u);
    if (!storage) return {};

    std::string value;
    value.reserve(128);
    for (uint32_t offset = 0; offset < 512u; ++offset) {
        const uint8_t character = PPC_LOAD_U8(storage + 2u + offset);
        if (!character) break;
        if (character < 0x20u || character > 0x7Eu) return {};
        value.push_back(static_cast<char>(character));
    }
    return value;
}

std::string ReadGuestNarrowCString(uint8_t* base, uint32_t address) {
    if (!address) return {};
    std::string value;
    value.reserve(128);
    for (uint32_t offset = 0; offset < 512u; ++offset) {
        const uint8_t character = PPC_LOAD_U8(address + offset);
        if (!character) break;
        if (character < 0x20u || character > 0x7Eu) return {};
        value.push_back(static_cast<char>(character));
    }
    return value;
}

bool IsXdfResolutionPath(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
        return character >= 'A' && character <= 'Z'
                   ? static_cast<char>(character - 'A' + 'a')
                   : static_cast<char>(character);
    });
    return lower.find("guiprecache") != std::string::npos ||
           lower.find("fonts\\") != std::string::npos ||
           lower.find("fonts/") != std::string::npos ||
           lower.find("text.xfc") != std::string::npos;
}

void AppendXdfResolutionTrace(PPCContext& context, uint8_t* base, uint32_t address) {
    if (address == 0x8220CC68u || address == 0x8220C770u) {
        const uint32_t root = PPC_LOAD_U32(0x82A690F8u);
        const uint32_t manager = root ? PPC_LOAD_U32(root + 12u) : 0;
        const uint32_t package = manager ? PPC_LOAD_U32(manager + 164u) : 0;
        const bool isGameContextTextRead =
            address == 0x8220CC68u && context.r3.u32 && package &&
            PPC_LOAD_U32(package + 76u) == 0xEEu &&
            PPC_LOAD_U32(context.r3.u32 + 40u) == 0x1Fu;
        const bool isGameContextTextFallback =
            address == 0x8220C770u && context.r3.u32 == activeGameContextTextStream;
        if (isGameContextTextRead) activeGameContextTextStream = context.r3.u32;
        if (isGameContextTextRead || isGameContextTextFallback) {
            const uint32_t sequence =
                xdfResolutionTraceSequence.fetch_add(1, std::memory_order_relaxed);
            if (sequence < 128u) {
                std::filesystem::create_directories("logs");
                std::ofstream out("logs/m6d_xdf_resolution_trace.log", std::ios::app);
                out << "XDF_RESOLUTION sequence=" << sequence << " stage="
                    << (isGameContextTextRead ? "gamecontext-text-read"
                                              : "gamecontext-text-fallback")
                    << " thread=" << CurrentGuestThreadId() << " function=0x"
                    << std::hex << address << " caller=0x" << context.lr << " stream=0x"
                    << context.r3.u32 << " stream_position=0x"
                    << PPC_LOAD_U64(context.r3.u32 + 24u) << " file_index=0x"
                    << PPC_LOAD_U32(context.r3.u32 + 40u) << " buffer=0x"
                    << context.r4.u32 << " bytes=0x" << context.r5.u32
                    << " package=0x" << package << " package_cursor=0x"
                    << (package ? PPC_LOAD_U32(package + 88u) : 0)
                    << " package_last_chunk=0x"
                    << (package ? PPC_LOAD_U32(package + 76u) : 0) << std::dec
                    << " path=fonts\\text.xfc" << '\n';
            }
            return;
        }
    }

    if (address == 0x821F8AD0u && context.lr >= 0x8220CB70u &&
        context.lr <= 0x8220CC60u && !activeXdfResolutionPath.empty()) {
        const char* cleanupStage = "package-path-cleanup";
        if (context.lr == 0x8220CB78u) {
            cleanupStage = "package-entry-accepted";
        } else if (context.lr == 0x8220CB84u) {
            cleanupStage = "package-index-negative";
        } else if (context.lr == 0x8220CC50u) {
            cleanupStage = "package-open-finished";
        } else if (context.lr == 0x8220CC58u) {
            cleanupStage = "package-request-finished";
        }
        const uint32_t sequence =
            xdfResolutionTraceSequence.fetch_add(1, std::memory_order_relaxed);
        if (sequence < 128u) {
            std::filesystem::create_directories("logs");
            std::ofstream out("logs/m6d_xdf_resolution_trace.log", std::ios::app);
            out << "XDF_RESOLUTION sequence=" << sequence
                << " stage=" << cleanupStage << " thread=" << CurrentGuestThreadId()
                << " function=0x" << std::hex << address << " caller=0x" << context.lr
                << " output=0x" << activeXdfResolutionOutput
                << " lookup_result=0x"
                << (activeXdfResolutionOutput
                        ? PPC_LOAD_U32(activeXdfResolutionOutput + 40u)
                        : 0)
                << " stream_position=0x"
                << (activeXdfResolutionOutput
                        ? PPC_LOAD_U64(activeXdfResolutionOutput + 24u)
                        : 0)
                << " backing_object=0x"
                << (activeXdfResolutionOutput
                        ? PPC_LOAD_U32(activeXdfResolutionOutput + 12u)
                        : 0)
                << " backing_vtable=0x"
                << ((activeXdfResolutionOutput &&
                     PPC_LOAD_U32(activeXdfResolutionOutput + 12u))
                        ? PPC_LOAD_U32(PPC_LOAD_U32(activeXdfResolutionOutput + 12u))
                        : 0)
                << std::dec << " path=" << activeXdfResolutionPath << '\n';
        }
        return;
    }

    if ((address == 0x8220CC68u || address == 0x8220C770u) &&
        context.r3.u32 == activeXdfResolutionOutput &&
        !activeXdfResolutionPath.empty()) {
        const uint32_t sequence =
            xdfResolutionTraceSequence.fetch_add(1, std::memory_order_relaxed);
        if (sequence < 128u) {
            const uint32_t root = PPC_LOAD_U32(0x82A690F8u);
            const uint32_t manager = root ? PPC_LOAD_U32(root + 12u) : 0;
            const uint32_t package = manager ? PPC_LOAD_U32(manager + 164u) : 0;
            const uint32_t backing = PPC_LOAD_U32(activeXdfResolutionOutput + 12u);
            std::filesystem::create_directories("logs");
            std::ofstream out("logs/m6d_xdf_resolution_trace.log", std::ios::app);
            out << "XDF_RESOLUTION sequence=" << sequence
                << " stage="
                << (address == 0x8220CC68u ? "package-read-entry"
                                           : "package-read-fallback")
                << " thread=" << CurrentGuestThreadId() << " function=0x" << std::hex
                << address << " caller=0x" << context.lr << " stream=0x"
                << activeXdfResolutionOutput << " stream_position=0x"
                << PPC_LOAD_U64(activeXdfResolutionOutput + 24u)
                << " lookup_result=0x"
                << PPC_LOAD_U32(activeXdfResolutionOutput + 40u) << " buffer=0x"
                << context.r4.u32 << " bytes=0x" << context.r5.u32
                << " backing_object=0x" << backing << " backing_vtable=0x"
                << (backing ? PPC_LOAD_U32(backing) : 0) << " package=0x" << package
                << " package_cursor=0x" << (package ? PPC_LOAD_U32(package + 88u) : 0)
                << " package_last_chunk=0x"
                << (package ? PPC_LOAD_U32(package + 76u) : 0)
                << " package_chunks=0x"
                << (package ? PPC_LOAD_U32(package + 184u) : 0)
                << std::dec << " path=" << activeXdfResolutionPath << '\n';
        }
        return;
    }

    uint32_t pathObject = 0;
    const char* stage = nullptr;
    if (address == 0x8220C8A8u) {
        pathObject = context.r4.u32;
        stage = "package-aware-open";
    } else if (address == 0x82207560u) {
        pathObject = context.r4.u32;
        stage = "resource-open";
    } else {
        return;
    }

    const std::string path = ReadStarbreezeNarrowString(base, pathObject);
    if (!IsXdfResolutionPath(path)) return;
    if (address == 0x8220C8A8u) {
        activeXdfResolutionPath = path;
        activeXdfResolutionOutput = context.r3.u32;
    }

    const uint32_t sequence =
        xdfResolutionTraceSequence.fetch_add(1, std::memory_order_relaxed);
    if (sequence >= 128u) return;

    const uint32_t root = PPC_LOAD_U32(0x82A690F8u);
    const uint32_t manager = root ? PPC_LOAD_U32(root + 12u) : 0;
    const uint32_t package = manager ? PPC_LOAD_U32(manager + 164u) : 0;
    std::filesystem::create_directories("logs");
    std::ofstream out("logs/m6d_xdf_resolution_trace.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "XDF_RESOLUTION sequence=" << sequence << " stage=" << stage
        << " thread=" << CurrentGuestThreadId() << " function=0x" << std::hex << address
        << " caller=0x" << context.lr << " path_object=0x" << pathObject
        << " root=0x" << root << " manager=0x" << manager
        << " manager_generation=0x" << (manager ? PPC_LOAD_U32(manager + 156u) : 0)
        << " package=0x" << package;
    if (package) {
        out << " package_vtable=0x" << PPC_LOAD_U32(package)
            << " base_vtable=0x" << PPC_LOAD_U32(package + 60u)
            << " base_storage=0x" << PPC_LOAD_U32(package + 64u)
            << " file_count=0x" << PPC_LOAD_U32(package + 76u)
            << " cursor=0x" << PPC_LOAD_U32(package + 88u)
            << " names=0x" << PPC_LOAD_U32(package + 180u)
            << " files=0x" << PPC_LOAD_U32(package + 184u)
            << " chunks=0x" << PPC_LOAD_U32(package + 188u)
            << std::dec << " base_path="
            << ReadStarbreezeNarrowString(base, package + 60u);
    }
    out << std::dec << " path=" << path << '\n';
}

void AppendRootUpdateListLifecycle(PPCContext& context, uint8_t* base, uint32_t address) {
    if (address != 0x827798A8u && address != 0x827799B8u) return;

    std::lock_guard<std::mutex> lock(rootUpdateListTraceMutex);
    constexpr uint32_t kTraceLimit = 512;
    if (rootUpdateListTraceSequence >= kTraceLimit) return;

    const uint32_t owner = context.r3.u32;
    const uint32_t item = context.r4.u32;
    const uint32_t ownerVtable = owner ? PPC_LOAD_U32(owner) : 0;
    const uint32_t itemVtable = item ? PPC_LOAD_U32(item) : 0;
    const uint32_t itemUpdate = itemVtable ? PPC_LOAD_U32(itemVtable + 8u) : 0;
    const uint32_t listObject = owner ? PPC_LOAD_U32(owner + 156u) : 0;
    const uint32_t countBefore = listObject ? PPC_LOAD_U32(listObject + 4u) : 0;
    const uint32_t items = listObject ? PPC_LOAD_U32(listObject + 24u) : 0;

    std::filesystem::create_directories("logs");
    std::ofstream out("logs/m6d_root_update_list_lifecycle.log",
                      rootUpdateListTraceSequence ? std::ios::app : std::ios::trunc);
    out << "UPDATE_LIST sequence=" << rootUpdateListTraceSequence++
        << " action=" << (address == 0x827798A8u ? "register" : "remove")
        << " thread=" << CurrentGuestThreadId()
        << " caller_lr=0x" << std::hex << context.lr
        << " owner=0x" << owner << " owner_vtable=0x" << ownerVtable
        << " item=0x" << item << " item_vtable=0x" << itemVtable
        << " item_update=0x" << itemUpdate
        << " list_object=0x" << listObject << " items=0x" << items
        << std::dec << " count_before=" << countBefore << '\n';
}

int64_t FrameCadenceTimestampUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               FrameCadenceClock::now().time_since_epoch())
        .count();
}

void AppendFrameCadenceLine(const char* event, uint64_t ordinal,
                            const std::string& details) {
    if (!darkness::diagnostics::ShouldSampleFrameCadence(ordinal)) return;
    std::lock_guard lock(frameCadenceLogMutex);
    std::filesystem::create_directories("logs");
    const bool first = !frameCadenceLogStarted.exchange(true, std::memory_order_relaxed);
    std::ofstream out(kFrameCadenceAttributionPath,
                      first ? std::ios::trunc : std::ios::app);
    if (first) {
        out << "FRAME_CADENCE_BEGIN behavior=observational_sparse"
               " producer=0x820e2058 enqueue=0x825a45a8"
               " frame_pool=0x825a3fd8 initial_lr=0x820e21a4"
               " retry_lr=0x820e21e0"
               " completion_acquire=0x825a43b0 submit=0x82241a68"
               " simulation_tick=0x824a7080 simulation_step_lr=0x824a7194"
               " simulation_queue_drain_lr=0x824a7254 observer_version=5"
               " controller_entry=0x827a6198 controller_measure=entry_attempts"
               " prebuild_wait=0x825a4278 end_boundary_lr=0x820de1e0"
               " prebuild_duration=entry_to_next_timer_call_not_exact_return"
               " legacy_simulation_fields=entry_attempts_not_updates"
               " work_completion=queue_consumer_return_not_full_simulation"
               " client_steps=packet_count_vs_returned_CA0_delta"
               " client_steps_scope=same_world_same_thread_consumer_only"
               " guest_timeout_seconds=0.05\n";
    }
    out << "FRAME_CADENCE event=" << event << " ordinal=" << ordinal
        << " timestamp_us=" << FrameCadenceTimestampUs()
        << " thread=" << CurrentGuestThreadId() << ' ' << details << '\n';
}

void AppendFrameCadenceFunctionEntry(PPCContext& context, uint8_t* base,
                                     uint32_t address) {
    if (address == 0x820DE198u) {
        // A new driver call invalidates an unfinished/exceptional observation.
        frameCadenceThreadState.prebuildWait.Reset();
        return;
    }
    if (darkness::diagnostics::IsPrebuildWaitEntry(address, context.lr)) {
        auto& stage = frameCadenceThreadState;
        const uint64_t ordinal = frameCadencePrebuildWaitEntries.fetch_add(
            1, std::memory_order_relaxed) + 1;
        const bool sampled = darkness::diagnostics::ShouldSampleFrameCadence(ordinal);
        stage.prebuildWait.Begin(context.r31.u32, ordinal, sampled);
        if (!sampled) return;
        stage.prebuildWaitQueue = context.r3.u32;
        stage.prebuildWaitStart = FrameCadenceClock::now();
        return;
    }
    if (darkness::diagnostics::IsPrebuildWaitEndBoundary(address, context.lr)) {
        auto& stage = frameCadenceThreadState;
        if (!stage.prebuildWait.Finish(context.r31.u32)) return;
        const auto finished = FrameCadenceClock::now();
        std::ostringstream details;
        details << "main=0x" << std::hex << stage.prebuildWait.owner
                << " queue=0x" << stage.prebuildWaitQueue << std::dec
                << " duration_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
                       finished - stage.prebuildWaitStart).count()
                << " boundary=next_timer_entry";
        AppendFrameCadenceLine("prebuild_wait_boundary", stage.prebuildWait.ordinal,
                               details.str());
        return;
    }
    if (darkness::diagnostics::IsFrameControllerEntry(address)) {
        const uint64_t ordinal = frameCadenceControllerEntries.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (!darkness::diagnostics::ShouldSampleFrameCadence(ordinal)) return;
        // Register-only snapshot before the guest prologue. Do not chase a
        // possibly-null requested view or alter busy/availability gates.
        std::ostringstream details;
        details << "controller=0x" << std::hex << context.r3.u32
                << " requested_view=0x" << context.r4.u32
                << " arg5=0x" << context.r5.u32
                << " caller=0x" << context.lr;
        AppendFrameCadenceLine("controller_entry", ordinal, details.str());
        return;
    }
    const bool simulationTick =
        darkness::diagnostics::IsSimulationTickEntry(address);
    const bool simulationStep =
        darkness::diagnostics::IsSimulationStepCall(context.lr);
    const bool simulationQueuedDrain =
        darkness::diagnostics::IsSimulationQueuedDrainCall(context.lr);
    const bool build = address == 0x820E2058u;
    const bool poolInitial =
        darkness::diagnostics::IsFramePoolInitialAcquire(address, context.lr);
    const bool poolRetry =
        darkness::diagnostics::IsFramePoolRetryAcquire(address, context.lr);
    const bool enqueue = address == 0x825A45A8u && context.lr == 0x820E2834u;
    const bool completionAcquire =
        address == 0x825A43B0u && context.lr == 0x820E32A0u;
    const bool submit = address == 0x82241A68u && context.lr == 0x820E3914u;
    if (!simulationTick && !simulationStep && !build && !poolInitial &&
        !poolRetry && !enqueue && !completionAcquire && !submit && !simulationQueuedDrain) {
        return;
    }

    auto& state = frameCadenceThreadState;
    const auto now = FrameCadenceClock::now();

    if (simulationTick) {
        const uint64_t ordinal =
            frameCadenceSimulationTicks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!darkness::diagnostics::ShouldSampleFrameCadence(ordinal)) return;
        const uint32_t world = context.r3.u32;
        std::ostringstream details;
        details << "world=0x" << std::hex << world
                << " caller=0x" << context.lr
                << " world_vtable=0x" << PPC_LOAD_U32(world)
                << " configured_rate_bits=0x" << PPC_LOAD_U32(world + 0x1A0u)
                << " base_step_bits=0x" << PPC_LOAD_U32(world + 0x19Cu)
                << " scaled_step_bits=0x" << PPC_LOAD_U32(world + 0x1A4u)
                << " timescale_bits=0x" << PPC_LOAD_U32(world + 0x1A8u)
                << " accumulator_bits=0x" << PPC_LOAD_U32(world + 0xC58u)
                << " queued_read=0x" << PPC_LOAD_U32(world + 0xC3Cu)
                << " queued_write=0x" << PPC_LOAD_U32(world + 0xC38u)
                << " dependency=0x" << PPC_LOAD_U32(world + 0xC28u)
                << std::dec
                << " total_steps="
                << frameCadenceSimulationSteps.load(std::memory_order_relaxed);
        AppendFrameCadenceLine("simulation_tick", ordinal, details.str());
        return;
    }

    if (simulationStep || simulationQueuedDrain) {
        auto& counter = simulationQueuedDrain ? frameCadenceSimulationQueuedDrains : frameCadenceSimulationSteps;
        const uint64_t ordinal = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!darkness::diagnostics::ShouldSampleFrameCadence(ordinal)) return;
        const uint32_t world = context.r3.u32;
        std::ostringstream details;
        details << "world=0x" << std::hex << world
                << " callee=0x" << address
                << " caller=0x" << context.lr
                << " base_step_bits=0x" << PPC_LOAD_U32(world + 0x19Cu)
                << " timescale_bits=0x" << PPC_LOAD_U32(world + 0x1A8u)
                << " accumulator_bits=0x" << PPC_LOAD_U32(world + 0xC58u)
                << std::dec
                << " total_ticks="
                << frameCadenceSimulationTicks.load(std::memory_order_relaxed);
        AppendFrameCadenceLine(simulationQueuedDrain ? "simulation_queue_drain" : "simulation_step", ordinal, details.str());
        return;
    }

    if (build) {
        state.buildActive = true;
        state.buildStart = now;
        state.buildMain = context.r3.u32;
        const uint64_t ordinal =
            frameCadenceBuildEntries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!darkness::diagnostics::ShouldSampleFrameCadence(ordinal)) return;
        std::ostringstream details;
        details << "main=0x" << std::hex << state.buildMain
                << " caller=0x" << context.lr;
        if (context.lr == 0x827A62D0u) {
            // Raw caller 827A61A4/827A628C retains controller in r29 and
            // passes the selected view in r4. No extra guest-memory reads.
            details << " controller=0x" << context.r29.u32
                    << " selected_view=0x" << context.r4.u32;
        }
        AppendFrameCadenceLine("build_begin", ordinal, details.str());
        return;
    }

    if (poolInitial || poolRetry) {
        auto& counter = poolInitial ? frameCadencePoolInitialAcquires
                                    : frameCadencePoolRetryAcquires;
        const uint64_t ordinal =
            counter.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!darkness::diagnostics::ShouldSampleFrameCadence(ordinal)) return;
        std::ostringstream details;
        details << "main=0x" << std::hex << state.buildMain
                << " queue=0x" << context.r3.u32
                << " timeout_bits=0x" << context.f1.u64 << std::dec
                << " build_active=" << state.buildActive
                << " total_initial="
                << frameCadencePoolInitialAcquires.load(std::memory_order_relaxed)
                << " total_retry="
                << frameCadencePoolRetryAcquires.load(std::memory_order_relaxed);
        AppendFrameCadenceLine(poolInitial ? "frame_pool_initial"
                                           : "frame_pool_retry",
                               ordinal, details.str());
        return;
    }

    if (enqueue) {
        const uint64_t ordinal =
            frameCadenceEnqueues.fetch_add(1, std::memory_order_relaxed) + 1;
        const int64_t durationUs = state.buildActive
            ? std::chrono::duration_cast<std::chrono::microseconds>(now - state.buildStart)
                  .count()
            : -1;
        if (!darkness::diagnostics::ShouldSampleFrameCadence(ordinal)) {
            state.buildActive = false;
            return;
        }
        std::ostringstream details;
        details << "main=0x" << std::hex << state.buildMain
                << " queue=0x" << context.r3.u32
                << " frame=0x" << context.r4.u32 << std::dec
                << " build_duration_us=" << durationUs
                << " totals_pool_initial="
                << frameCadencePoolInitialAcquires.load(std::memory_order_relaxed)
                << " totals_pool_retry="
                << frameCadencePoolRetryAcquires.load(std::memory_order_relaxed);
        AppendFrameCadenceLine("enqueue", ordinal, details.str());
        state.buildActive = false;
        return;
    }

    if (completionAcquire) {
        const uint32_t queue = context.r3.u32;
        const uint32_t sentinel = queue ? PPC_LOAD_U32(queue + 16u) : 0;
        const uint32_t sentinelFlags = sentinel ? PPC_LOAD_U32(sentinel + 4u) : 0;
        const bool emptyKnown = sentinel != 0;
        const bool empty = emptyKnown &&
            darkness::diagnostics::IntrusiveQueueIsEmpty(sentinelFlags);

        state.completionAcquireActive = true;
        state.completionQueueEmpty = empty;
        state.completionWaitObserved = false;
        state.completionAcquireStart = now;
        state.completionQueue = queue;
        state.completionEvent = queue ? PPC_LOAD_U32(queue + 68u) : 0;
        state.completionWaitStatus = 0;
        state.completionGuestTimeout = 0;
        state.completionWaitDurationUs = -1;

        const uint64_t ordinal =
            frameCadenceAcquireEntries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (empty) frameCadenceEmptyEntries.fetch_add(1, std::memory_order_relaxed);
        if (!darkness::diagnostics::ShouldSampleFrameCadence(ordinal)) return;
        std::ostringstream details;
        details << "queue=0x" << std::hex << queue
                << " sentinel=0x" << sentinel
                << " sentinel_flags=0x" << sentinelFlags
                << " event=0x" << state.completionEvent << std::dec
                << " empty_known=" << emptyKnown << " empty=" << empty;
        AppendFrameCadenceLine("completion_acquire", ordinal, details.str());
        return;
    }

    if (submit) {
        const uint64_t ordinal =
            frameCadenceSubmits.fetch_add(1, std::memory_order_relaxed) + 1;
        const int64_t acquireToSubmitUs = state.completionAcquireActive
            ? std::chrono::duration_cast<std::chrono::microseconds>(
                  now - state.completionAcquireStart).count()
            : -1;
        if (!darkness::diagnostics::ShouldSampleFrameCadence(ordinal)) {
            state.completionAcquireActive = false;
            return;
        }
        std::ostringstream details;
        details << "graphics=0x" << std::hex << context.r3.u32
                << " frame=0x" << context.r4.u32
                << " queue=0x" << state.completionQueue << std::dec
                << " acquire_to_submit_us=" << acquireToSubmitUs
                << " queue_empty_entry=" << state.completionQueueEmpty
                << " wait_observed=" << state.completionWaitObserved
                << " wait_status=0x" << std::hex << state.completionWaitStatus
                << std::dec << " guest_timeout_100ns=" << state.completionGuestTimeout
                << " wait_duration_us=" << state.completionWaitDurationUs
                << " totals_build=" << frameCadenceBuildEntries.load(std::memory_order_relaxed)
                << " totals_enqueue=" << frameCadenceEnqueues.load(std::memory_order_relaxed)
                << " totals_pool_initial="
                << frameCadencePoolInitialAcquires.load(std::memory_order_relaxed)
                << " totals_pool_retry="
                << frameCadencePoolRetryAcquires.load(std::memory_order_relaxed)
                << " totals_acquire=" << frameCadenceAcquireEntries.load(std::memory_order_relaxed)
                << " totals_empty=" << frameCadenceEmptyEntries.load(std::memory_order_relaxed)
                << " totals_wait=" << frameCadenceWaits.load(std::memory_order_relaxed)
                << " totals_timeout=" << frameCadenceTimeouts.load(std::memory_order_relaxed)
                << " totals_simulation_ticks="
                << frameCadenceSimulationTicks.load(std::memory_order_relaxed)
                << " totals_simulation_steps="
                << frameCadenceSimulationSteps.load(std::memory_order_relaxed)
                << " totals_simulation_queue_drains="
                << frameCadenceSimulationQueuedDrains.load(std::memory_order_relaxed);
        AppendFrameCadenceLine("submit", ordinal, details.str());
        state.completionAcquireActive = false;
    }
}

void AppendFrameCadenceWait(uint32_t handle, bool hasTimeout, int64_t timeout,
                            uint32_t status) {
    auto& state = frameCadenceThreadState;
    if (!state.completionAcquireActive || handle != state.completionEvent) return;

    state.completionWaitObserved = true;
    state.completionWaitStatus = status;
    state.completionGuestTimeout = hasTimeout ? timeout : INT64_MIN;
    state.completionWaitDurationUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            FrameCadenceClock::now() - state.completionAcquireStart).count();
    const uint64_t ordinal =
        frameCadenceWaits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (darkness::diagnostics::WaitCompletedByTimeout(status)) {
        frameCadenceTimeouts.fetch_add(1, std::memory_order_relaxed);
    }
    if (!darkness::diagnostics::ShouldSampleFrameCadence(ordinal)) return;
    std::ostringstream details;
    details << "queue=0x" << std::hex << state.completionQueue
            << " handle=0x" << handle << " status=0x" << status << std::dec
            << " has_timeout=" << hasTimeout
            << " guest_timeout_100ns=" << state.completionGuestTimeout
            << " wait_duration_us=" << state.completionWaitDurationUs
            << " queue_empty_entry=" << state.completionQueueEmpty
            << " total_timeouts=" << frameCadenceTimeouts.load(std::memory_order_relaxed);
    AppendFrameCadenceLine("completion_wait", ordinal, details.str());
}

void AppendTitleFrameProbe(PPCContext& context, uint8_t* base, uint32_t address) {
    const bool frameBegin = address == 0x827A62E0u && context.lr == 0x820E3158u;
    const bool sceneQuery = address == 0x820F56D0u && context.lr == 0x820E3320u;
    const bool producerAcquire = address == 0x825A3FD8u && context.lr == 0x820E3358u;
    const bool frameSubmit = address == 0x82241A68u && context.lr == 0x820E3914u;
    if (!frameBegin && !sceneQuery && !producerAcquire && !frameSubmit) return;

    std::lock_guard<std::mutex> lock(titleFrameProbeMutex);
    if (titleFrameProbeComplete) return;
    if (!titleFrameProbeStarted) {
        if (!frameBegin) return;
        titleFrameProbeStarted = true;
        titleFrameProbeThread = CurrentGuestThreadId();
        std::filesystem::create_directories("logs");
        std::ofstream out(kTitleFrameProbePath, std::ios::trunc);
        out << "PROBE_BEGIN thread=" << titleFrameProbeThread
            << " limit=" << kTitleFrameProbeLimit
            << " behavior=observational_sparse_state_changes guest_words=big_endian\n";
    }
    if (CurrentGuestThreadId() != titleFrameProbeThread) return;

    if (frameBegin) {
        if (titleFrameProbeSequence == kTitleFrameProbeLimit) {
            titleFrameProbeComplete = true;
            std::ofstream out(kTitleFrameProbePath, std::ios::app);
            out << std::dec << "PROBE_COMPLETE frames=" << titleFrameProbeSequence << '\n';
            return;
        }
        ++titleFrameProbeSequence;
        const uint32_t main = context.r30.u32;
        const uint32_t root = PPC_LOAD_U32(main + 24u);
        const uint32_t producer = PPC_LOAD_U32(main + 108u);
        const uint32_t scene = PPC_LOAD_U32(main + 2788u);
        const std::array<uint32_t, 15> state{
            PPC_LOAD_U32(main + 2720u), PPC_LOAD_U32(main + 2724u),
            PPC_LOAD_U32(main + 2728u), PPC_LOAD_U32(main + 2732u),
            PPC_LOAD_U32(main + 2752u), PPC_LOAD_U32(main + 2764u), scene,
            scene ? PPC_LOAD_U32(scene + 3668u) : 0,
            root ? PPC_LOAD_U32(root + 20u) : 0,
            root ? PPC_LOAD_U32(root + 60u) : 0,
            root ? PPC_LOAD_U32(root + 64u) : 0,
            root ? PPC_LOAD_U32(root + 72u) : 0,
            root ? PPC_LOAD_U32(root + 88u) : 0,
            producer ? PPC_LOAD_U32(producer + 36u) : 0,
            PPC_LOAD_U32(main + 660u),
        };
        const bool powerOfTwo =
            (titleFrameProbeSequence & (titleFrameProbeSequence - 1u)) == 0;
        const bool stateChanged =
            titleFrameProbeHasLastState && state != titleFrameProbeLastState;
        titleFrameProbeLogDetails =
            titleFrameProbeSequence <= 8u || powerOfTwo || stateChanged;
        titleFrameProbeLastState = state;
        titleFrameProbeHasLastState = true;
        if (!titleFrameProbeLogDetails) return;

        std::ofstream out(kTitleFrameProbePath, std::ios::app);
        out << std::hex << std::setfill('0');
        out << std::dec << "FRAME_BEGIN sequence=" << titleFrameProbeSequence
            << std::hex << " main=0x" << main
            << " root=0x" << root
            << " producer=0x" << producer
            << " state2720=0x" << PPC_LOAD_U32(main + 2720u)
            << " state2724=0x" << PPC_LOAD_U32(main + 2724u)
            << " state2728=0x" << PPC_LOAD_U32(main + 2728u)
            << " state2732=0x" << PPC_LOAD_U32(main + 2732u)
            << " service2752=0x" << PPC_LOAD_U32(main + 2752u)
            << " service2764=0x" << PPC_LOAD_U32(main + 2764u)
            << " scene2788=0x" << PPC_LOAD_U32(main + 2788u)
            << " time5448_hi=0x" << PPC_LOAD_U32(main + 5448u)
            << " time5448_lo=0x" << PPC_LOAD_U32(main + 5452u);
        if (root) {
            const uint32_t listObject = PPC_LOAD_U32(root + 156u);
            const uint32_t listCount = listObject ? PPC_LOAD_U32(listObject + 4u) : 0;
            const uint32_t listItems = listObject ? PPC_LOAD_U32(listObject + 24u) : 0;
            out << " root_vtable=0x" << PPC_LOAD_U32(root)
                << " root_flags20=0x" << PPC_LOAD_U32(root + 20u)
                << " root_field60=0x" << PPC_LOAD_U32(root + 60u)
                << " root_field64=0x" << PPC_LOAD_U32(root + 64u)
                << " graphics72=0x" << PPC_LOAD_U32(root + 72u)
                << " root_field88=0x" << PPC_LOAD_U32(root + 88u)
                << " update_list=0x" << listObject
                << " update_items=0x" << listItems
                << std::dec << " update_count=" << listCount << std::hex;
            if (listItems && listCount <= 64u &&
                (titleFrameProbeSequence <= 16u ||
                 (titleFrameProbeSequence & (titleFrameProbeSequence - 1u)) == 0)) {
                const uint32_t loggedCount = std::min(listCount, 16u);
                for (uint32_t index = 0; index < loggedCount; ++index) {
                    const uint32_t item = PPC_LOAD_U32(listItems + index * 4u);
                    const uint32_t vtable = item ? PPC_LOAD_U32(item) : 0;
                    const uint32_t update = vtable ? PPC_LOAD_U32(vtable + 8u) : 0;
                    out << " update" << std::dec << index << std::hex << "=0x" << item
                        << "/0x" << vtable << "/0x" << update;
                }
            }
        }
        out << '\n';
        return;
    }

    if (!titleFrameProbeSequence || !titleFrameProbeLogDetails) return;
    std::ofstream out(kTitleFrameProbePath, std::ios::app);
    out << std::hex << std::setfill('0');
    if (sceneQuery) {
        const uint32_t scene = context.r3.u32;
        const uint32_t list = scene ? PPC_LOAD_U32(scene + 3668u) : 0;
        const uint32_t count = list ? PPC_LOAD_U32(list + 4u) : 0;
        const uint32_t items = list ? PPC_LOAD_U32(list + 24u) : 0;
        const uint32_t first = count && items ? PPC_LOAD_U32(items) : 0;
        out << std::dec << "SCENE_QUERY sequence=" << titleFrameProbeSequence
            << std::hex << " scene=0x" << scene
            << " vtable=0x" << (scene ? PPC_LOAD_U32(scene) : 0)
            << " field3664=0x" << (scene ? PPC_LOAD_U32(scene + 3664u) : 0)
            << " list3668=0x" << list
            << " field3672=0x" << (scene ? PPC_LOAD_U32(scene + 3672u) : 0)
            << " field3676=0x" << (scene ? PPC_LOAD_U32(scene + 3676u) : 0)
            << " count=0x" << count << " items=0x" << items
            << " first=0x" << first << '\n';
        return;
    }
    if (producerAcquire) {
        const uint32_t producer = context.r3.u32;
        out << std::dec << "PRODUCER_ACQUIRE sequence=" << titleFrameProbeSequence
            << std::hex << " producer=0x" << producer;
        if (producer) {
            out << " field08=0x" << PPC_LOAD_U32(producer + 8u)
                << " field20=0x" << PPC_LOAD_U32(producer + 32u)
                << " available24=0x" << PPC_LOAD_U32(producer + 36u)
                << " field28=0x" << PPC_LOAD_U32(producer + 40u)
                << " lock2c=0x" << PPC_LOAD_U32(producer + 44u)
                << " event40=0x" << PPC_LOAD_U32(producer + 64u);
        }
        out << '\n';
        return;
    }

    const uint32_t graphics = context.r3.u32;
    const uint32_t slotIndex = PPC_LOAD_U32(graphics + 4468u);
    const uint32_t slot0 = graphics + 4308u;
    const uint32_t slot1 = graphics + 4388u;
    const uint32_t selectedSlot = graphics + 4308u + slotIndex * 80u;
    const uint32_t selectedHeader = slotIndex <= 1 ? PPC_LOAD_U32(selectedSlot) : 0;
    const uint32_t selectedExternal = slotIndex <= 1 ? PPC_LOAD_U32(selectedSlot + 76u) : 0;
    const uint32_t selectedWords =
        slotIndex <= 1 && selectedExternal ? selectedExternal
        : slotIndex <= 1 && selectedHeader ? selectedSlot + 4u
                                           : 0;
    out << std::dec << "FRAME_SUBMIT sequence=" << titleFrameProbeSequence
        << std::hex << " main=0x" << context.r30.u32
        << " frame=0x" << context.r31.u32
        << " graphics=0x" << graphics
        << " flags308=0x" << PPC_LOAD_U32(graphics + 308u)
        << " resource3924=0x" << PPC_LOAD_U32(graphics + 3924u)
        << " slot_index=0x" << slotIndex
        << " previous_meta4760=0x" << PPC_LOAD_U32(graphics + 4760u)
        << " slot0_header=0x" << PPC_LOAD_U32(slot0)
        << " slot0_external=0x" << PPC_LOAD_U32(slot0 + 76u)
        << " slot1_header=0x" << PPC_LOAD_U32(slot1)
        << " slot1_external=0x" << PPC_LOAD_U32(slot1 + 76u)
        << " selected_slot=0x" << selectedSlot
        << " selected_header=0x" << selectedHeader
        << " selected_external=0x" << selectedExternal
        << " selected_words=0x" << selectedWords;
    if (selectedWords) {
        out << " words=";
        for (uint32_t i = 0; i < 16; ++i) {
            if (i) out << ',';
            out << "0x" << std::setw(8) << PPC_LOAD_U32(selectedWords + i * 4u);
        }
    }
    out << '\n';
}

bool IsSparseLevelGameplayOrdinal(uint64_t ordinal, uint64_t earlyLimit) {
    return ordinal <= earlyLimit || (ordinal & (ordinal - 1u)) == 0;
}

bool IsLevelGameplayGuestObject(uint32_t address) {
    return address >= 0x80000000u && address <= 0xFFFFFFFCu;
}

struct LevelRegistryNodeSnapshot {
    uint32_t object{};
    uint32_t vtable{};
    uint32_t flags{};
    uint32_t parent{};
    uint32_t list{};
    uint32_t count{};
    uint32_t items{};
    uint32_t firstChild{};
    uint32_t value{};
    std::array<uint32_t, 6> payload{};
};

struct PendingLevelRegistryMutation {
    bool active{};
    uint32_t function{};
    uint32_t functionEnd{};
    uint32_t entryStack{};
    uint32_t target{};
    uint32_t client{};
    uint64_t messageOrdinal{};
    const char* kind{};
    LevelRegistryNodeSnapshot before{};
};

thread_local PendingLevelRegistryMutation pendingLevelRegistryMutation{};

LevelRegistryNodeSnapshot SnapshotLevelRegistryNode(uint8_t* base,
                                                    uint32_t object) {
    LevelRegistryNodeSnapshot snapshot{};
    snapshot.object = object;
    if (!IsLevelGameplayGuestObject(object) || object > UINT32_MAX - 48u) {
        return snapshot;
    }
    snapshot.vtable = PPC_LOAD_U32(object);
    snapshot.flags = PPC_LOAD_U32(object + 4u);
    snapshot.parent = PPC_LOAD_U32(object + 8u);
    snapshot.list = PPC_LOAD_U32(object + 16u);
    snapshot.value = PPC_LOAD_U32(object + 20u);
    for (uint32_t index = 0; index < snapshot.payload.size(); ++index) {
        snapshot.payload[index] = PPC_LOAD_U32(object + 24u + index * 4u);
    }
    if (IsLevelGameplayGuestObject(snapshot.list) &&
        snapshot.list <= UINT32_MAX - 28u) {
        snapshot.count = PPC_LOAD_U32(snapshot.list + 4u);
        snapshot.items = PPC_LOAD_U32(snapshot.list + 24u);
        if (snapshot.count && IsLevelGameplayGuestObject(snapshot.items)) {
            snapshot.firstChild = PPC_LOAD_U32(snapshot.items);
        }
    }
    return snapshot;
}

uint32_t HashLevelRegistryBytes(uint8_t* base, uint32_t address,
                                uint32_t byteCount) {
    if (!address || address > UINT32_MAX - byteCount) return 0;
    uint32_t hash = 2166136261u;
    for (uint32_t index = 0; index < byteCount; ++index) {
        hash ^= PPC_LOAD_U8(address + index);
        hash *= 16777619u;
    }
    return hash;
}

uint32_t LevelRegistryMutationFunctionEnd(uint32_t address) {
    switch (address) {
    case 0x824B5E40u: return 0x824B5FE0u;
    case 0x824B5FE0u: return 0x824B6130u;
    case 0x8273C538u: return 0x8273C6A0u;
    case 0x8273C6A0u: return 0x8273C724u;
    case 0x8273DA90u: return 0x8273DB60u;
    case 0x8273DB60u: return 0x8273DC04u;
    case 0x8273E268u: return 0x8273E278u;
    default: return address + 4u;
    }
}

void AppendPendingLevelRegistryMutationAfter(PPCContext& context,
                                             uint8_t* base,
                                             uint32_t address) {
    auto& pending = pendingLevelRegistryMutation;
    if (!pending.active) return;

    // RuntimeFunctionEnter runs before each callee's prologue. A nested helper
    // therefore observes the mutation routine's lower stack frame, while the
    // first call made after the routine returns observes the original entry
    // stack again. This distinguishes a real post-state from helpers called
    // before the store, including the vtable setters used by the text parser.
    if (context.r1.u32 < pending.entryStack) return;
    if (context.lr >= pending.function && context.lr < pending.functionEnd) {
        return;
    }

    const PendingLevelRegistryMutation completed = pending;
    pending = {};
    if (levelRegistryUpdateTraceComplete.load(std::memory_order_relaxed)) return;

    const LevelRegistryNodeSnapshot after =
        SnapshotLevelRegistryNode(base, completed.target);
    const bool validClient = IsLevelGameplayGuestObject(completed.client) &&
                             PPC_LOAD_U32(completed.client) ==
                                 kLevelGameplayClientVtable;
    const bool changed =
        completed.before.vtable != after.vtable ||
        completed.before.flags != after.flags ||
        completed.before.parent != after.parent ||
        completed.before.list != after.list ||
        completed.before.count != after.count ||
        completed.before.items != after.items ||
        completed.before.firstChild != after.firstChild ||
        completed.before.value != after.value ||
        completed.before.payload != after.payload;
    const auto timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(levelRegistryUpdateTraceMutex);
    if (levelRegistryUpdateTraceSequence >= kLevelRegistryUpdateTraceLimit) {
        levelRegistryUpdateTraceComplete.store(true, std::memory_order_relaxed);
        return;
    }
    std::filesystem::create_directories("logs");
    std::ofstream out(kLevelRegistryUpdateTracePath,
                      levelRegistryUpdateTraceSequence ? std::ios::app
                                                       : std::ios::trunc);
    out << "LEVEL_REGISTRY_UPDATE sequence=" << levelRegistryUpdateTraceSequence++
        << " timestamp_us=" << timestampUs
        << " kind=MUTATION_AFTER"
        << " mutation_kind=" << (completed.kind ? completed.kind : "UNKNOWN")
        << " message_ordinal=" << completed.messageOrdinal
        << " thread=" << CurrentGuestThreadId()
        << std::hex
        << " mutation_function=0x" << completed.function
        << " mutation_entry_stack=0x" << completed.entryStack
        << " next_function=0x" << address
        << " next_lr=0x" << context.lr
        << " next_stack=0x" << context.r1.u32
        << " client=0x" << completed.client
        << " current_player_id=0x"
        << (validClient ? PPC_LOAD_U32(completed.client + 536u) : UINT32_MAX)
        << " current_game_id=0x"
        << (validClient ? PPC_LOAD_U32(completed.client + 540u) : UINT32_MAX)
        << " selected_id=0x"
        << (validClient ? PPC_LOAD_U32(completed.client + 3964u) : UINT32_MAX)
        << " view_object=0x"
        << (validClient ? PPC_LOAD_U32(completed.client + 7360u) : 0u)
        << " target=0x" << completed.target
        << std::dec << " changed=" << changed
        << std::hex
        << " before_vtable=0x" << completed.before.vtable
        << " after_vtable=0x" << after.vtable
        << " before_flags=0x" << completed.before.flags
        << " after_flags=0x" << after.flags
        << " before_parent=0x" << completed.before.parent
        << " after_parent=0x" << after.parent
        << " before_list=0x" << completed.before.list
        << " after_list=0x" << after.list
        << std::dec
        << " before_count=" << completed.before.count
        << " after_count=" << after.count
        << std::hex
        << " before_items=0x" << completed.before.items
        << " after_items=0x" << after.items
        << " before_first_child=0x" << completed.before.firstChild
        << " after_first_child=0x" << after.firstChild
        << " before_value=0x" << completed.before.value
        << " after_value=0x" << after.value
        << " before_payload_words=";
    for (uint32_t index = 0; index < completed.before.payload.size(); ++index) {
        if (index) out << ',';
        out << "0x" << completed.before.payload[index];
    }
    out << " after_payload_words=";
    for (uint32_t index = 0; index < after.payload.size(); ++index) {
        if (index) out << ',';
        out << "0x" << after.payload[index];
    }
    out << std::dec << '\n';
    if (levelRegistryUpdateTraceSequence >= kLevelRegistryUpdateTraceLimit) {
        levelRegistryUpdateTraceComplete.store(true, std::memory_order_relaxed);
    }
}

void AppendLevelRegistryUpdateProbe(PPCContext& context, uint8_t* base,
                                    uint32_t address) {
    constexpr uint32_t kTransportPump = 0x82483AD0u;
    constexpr uint32_t kClientMessageUpdate = 0x823F7E10u;
    constexpr uint32_t kRegistryParser = 0x824B7018u;
    constexpr uint32_t kRegistryTextValue = 0x824B5E40u;
    constexpr uint32_t kRegistryWideTextValue = 0x824B5FE0u;
    constexpr uint32_t kRegistryResize = 0x8273C538u;
    constexpr uint32_t kRegistryChild = 0x8273C6A0u;
    constexpr uint32_t kRegistryInteger = 0x8273DA90u;
    constexpr uint32_t kRegistryFloat = 0x8273DB60u;
    constexpr uint32_t kRegistryUpdateFlags = 0x8273E268u;

    const bool transportEntry = address == kTransportPump;
    const bool messageEntry = address == kClientMessageUpdate;
    const bool parserEntry = address == kRegistryParser;
    // 0x82745A28 is the registry-node implementation of vtable +0x210
    // (0x82095798 + 0x210).  It serializes/deserializes the node through r4
    // and recursively invokes each child node's +0x210 method at 0x82746ED0.
    // The other three call sites exercise the same virtual contract on other
    // object families.  Record them without assuming that every dispatch is
    // a network receive or that every target is a client listener.
    const bool listenerDispatch =
        !messageEntry &&
        (context.lr == 0x82746ED0u || context.lr == 0x820F6888u ||
         context.lr == 0x824A8068u || context.lr == 0x824844C8u);
    const bool mutationEntry =
        (address == kRegistryTextValue &&
         (context.lr == 0x824B707Cu || context.lr == 0x824B7108u)) ||
        (address == kRegistryWideTextValue && context.lr == 0x824B70F8u) ||
        (address == kRegistryResize && context.lr == 0x824B71D8u) ||
        (address == kRegistryChild && context.lr == 0x824B7248u) ||
        (address == kRegistryInteger && context.lr == 0x824B7150u) ||
        (address == kRegistryFloat && context.lr == 0x824B71A0u) ||
        (address == kRegistryUpdateFlags && context.lr == 0x824B705Cu);
    if (!transportEntry && !messageEntry && !listenerDispatch &&
        !parserEntry && !mutationEntry) {
        return;
    }
    if (levelRegistryUpdateTraceComplete.load(std::memory_order_relaxed)) return;

    uint32_t client = levelGameplayClient.load(std::memory_order_relaxed);
    uint32_t message = 0;
    uint32_t messageType = UINT32_MAX;
    uint32_t messageSource = 0;
    uint32_t payload = 0;
    uint32_t stream = 0;
    uint32_t transportManager = 0;
    uint32_t transportListeners = 0;
    uint32_t transportListenerCount = 0;
    uint32_t transportListenerItems = 0;
    uint32_t transportConnections = 0;
    uint32_t transportConnectionCount = 0;
    uint32_t transportConnectionItems = 0;
    uint32_t transportIndex = UINT32_MAX;
    uint32_t transportListener = 0;
    uint32_t transportConnection = 0;
    uint32_t transportCallback = 0;
    uint32_t transportCallbackSlot210 = 0;
    uint32_t dispatchListener = 0;
    uint32_t dispatchListenerVtable = 0;
    uint32_t dispatchCallee = 0;
    uint32_t dispatchListenerList = 0;
    uint32_t dispatchListenerCount = 0;
    uint32_t dispatchListenerItems = 0;
    uint64_t messageOrdinal = levelRegistryActiveMessageOrdinal;
    const char* kind = nullptr;

    if (transportEntry) {
        const uint64_t pumpOrdinal = levelRegistryTransportPumpEntries.fetch_add(
                                         1, std::memory_order_relaxed) + 1;
        if (!IsSparseLevelGameplayOrdinal(pumpOrdinal, 64u)) return;
        transportManager = context.r3.u32;
        if (!transportManager || transportManager > UINT32_MAX - 1932u) return;
        transportListeners = PPC_LOAD_U32(transportManager + 1924u);
        transportConnections = PPC_LOAD_U32(transportManager + 1932u);
        if (IsLevelGameplayGuestObject(transportListeners) &&
            transportListeners <= UINT32_MAX - 28u) {
            transportListenerCount = PPC_LOAD_U32(transportListeners + 4u);
            transportListenerItems = PPC_LOAD_U32(transportListeners + 24u);
        }
        if (IsLevelGameplayGuestObject(transportConnections) &&
            transportConnections <= UINT32_MAX - 28u) {
            transportConnectionCount = PPC_LOAD_U32(transportConnections + 4u);
            transportConnectionItems = PPC_LOAD_U32(transportConnections + 24u);
        }
        const uint32_t scanCount = std::min({transportListenerCount,
                                             transportConnectionCount, 64u});
        if (IsLevelGameplayGuestObject(transportListenerItems) &&
            IsLevelGameplayGuestObject(transportConnectionItems)) {
            for (uint32_t index = 0; index < scanCount; ++index) {
                const uint32_t listener =
                    PPC_LOAD_U32(transportListenerItems + index * 4u);
                const uint32_t connection =
                    PPC_LOAD_U32(transportConnectionItems + index * 4u);
                uint32_t callback = listener;
                if (IsLevelGameplayGuestObject(connection) &&
                    connection <= UINT32_MAX - 540u) {
                    const uint32_t overrideCallback =
                        PPC_LOAD_U32(connection + 536u);
                    if (overrideCallback) callback = overrideCallback;
                }
                if (IsLevelGameplayGuestObject(callback) &&
                    PPC_LOAD_U32(callback) == kLevelGameplayClientVtable) {
                    transportIndex = index;
                    transportListener = listener;
                    transportConnection = connection;
                    transportCallback = callback;
                    transportCallbackSlot210 =
                        PPC_LOAD_U32(kLevelGameplayClientVtable + 528u);
                    uint32_t expected = 0;
                    levelGameplayClient.compare_exchange_strong(
                        expected, callback, std::memory_order_relaxed);
                    client = levelGameplayClient.load(std::memory_order_relaxed);
                    break;
                }
            }
        }
        kind = "TRANSPORT_PUMP";
        messageOrdinal = pumpOrdinal;
    } else if (messageEntry) {
        if (!IsLevelGameplayGuestObject(context.r3.u32) ||
            PPC_LOAD_U32(context.r3.u32) != kLevelGameplayClientVtable) {
            return;
        }
        // Trace every valid client instance.  The front end may create an
        // earlier object with the same vtable; pinning the first instance
        // would silently hide the level client's real update stream.
        client = context.r3.u32;
        uint32_t expected = 0;
        levelGameplayClient.compare_exchange_strong(
            expected, client, std::memory_order_relaxed);

        message = context.r4.u32;
        if (message && message <= UINT32_MAX - 24u) {
            messageType = PPC_LOAD_U32(message + 20u);
            messageSource = PPC_LOAD_U32(message + 12u);
            if (messageSource && messageSource <= UINT32_MAX - 24u) {
                payload = PPC_LOAD_U32(messageSource + 24u);
                if (payload && payload <= UINT32_MAX - 11u) {
                    stream = payload + 11u;
                }
            }
        }
        messageOrdinal = levelRegistryMessageEntries.fetch_add(
                             1, std::memory_order_relaxed) + 1;
        levelRegistryActiveMessageOrdinal = messageOrdinal;
        levelRegistryActiveClient = client;
        levelRegistryUpdateActive = messageType != 11u && stream != 0;
        kind = messageType == 11u ? "MESSAGE_NO_REGISTRY_UPDATE"
                                  : "MESSAGE_REGISTRY_UPDATE";
    } else if (listenerDispatch) {
        dispatchListener = context.r3.u32;
        dispatchCallee = address;
        if (IsLevelGameplayGuestObject(dispatchListener)) {
            dispatchListenerVtable = PPC_LOAD_U32(dispatchListener);
            if (dispatchListener <= UINT32_MAX - 20u) {
                dispatchListenerList = PPC_LOAD_U32(dispatchListener + 16u);
            }
        }
        if (IsLevelGameplayGuestObject(dispatchListenerList) &&
            dispatchListenerList <= UINT32_MAX - 28u) {
            dispatchListenerCount = PPC_LOAD_U32(dispatchListenerList + 4u);
            dispatchListenerItems = PPC_LOAD_U32(dispatchListenerList + 24u);
        }
        message = context.r4.u32;
        if (message && message <= UINT32_MAX - 24u) {
            messageType = PPC_LOAD_U32(message + 20u);
            messageSource = PPC_LOAD_U32(message + 12u);
            if (messageSource && messageSource <= UINT32_MAX - 24u) {
                payload = PPC_LOAD_U32(messageSource + 24u);
                if (payload && payload <= UINT32_MAX - 11u) {
                    stream = payload + 11u;
                }
            }
        }
        messageOrdinal = levelRegistryListenerDispatchEntries.fetch_add(
                             1, std::memory_order_relaxed) + 1;
        const bool changed =
            levelRegistryLastDispatchCallee.exchange(
                dispatchCallee, std::memory_order_relaxed) != dispatchCallee ||
            levelRegistryLastDispatchListenerVtable.exchange(
                dispatchListenerVtable, std::memory_order_relaxed) !=
                dispatchListenerVtable ||
            levelRegistryLastDispatchMessageType.exchange(
                messageType, std::memory_order_relaxed) != messageType;
        if (!changed &&
            !IsSparseLevelGameplayOrdinal(messageOrdinal, 128u)) {
            return;
        }
        switch (context.lr) {
        case 0x82746ED0u: kind = "REGISTRY_CHILD_DISPATCH"; break;
        case 0x820F6888u: kind = "LOCAL_CALLBACK_DISPATCH"; break;
        case 0x824A8068u: kind = "CONNECTION_CALLBACK_DISPATCH"; break;
        default: kind = "PUMP_CALLBACK_DISPATCH"; break;
        }
    } else {
        client = levelRegistryActiveClient;
        if (!IsLevelGameplayGuestObject(client) ||
            PPC_LOAD_U32(client) != kLevelGameplayClientVtable) {
            levelRegistryUpdateActive = false;
            return;
        }
        const uint32_t registry = PPC_LOAD_U32(client + 656u);
        if (parserEntry && context.lr == 0x823F7E48u) {
            levelRegistryUpdateActive = context.r3.u32 == registry;
        }
        if (!levelRegistryUpdateActive) return;

        if (parserEntry) {
            stream = context.r4.u32;
            kind = context.lr == 0x824B725Cu ? "PARSER_CHILD"
                                             : "PARSER_ROOT";
        } else if (address == kRegistryTextValue) {
            kind = "SET_TEXT";
            stream = context.r4.u32 ? PPC_LOAD_U32(context.r4.u32) : 0;
        } else if (address == kRegistryWideTextValue) {
            kind = "SET_WIDE_TEXT";
            stream = context.r4.u32 ? PPC_LOAD_U32(context.r4.u32) : 0;
        } else if (address == kRegistryResize) {
            kind = "RESIZE_CHILDREN";
        } else if (address == kRegistryChild) {
            kind = "SELECT_CHILD";
        } else if (address == kRegistryInteger) {
            kind = "SET_INTEGER";
        } else if (address == kRegistryFloat) {
            kind = "SET_FLOAT";
        } else {
            kind = "SET_UPDATE_FLAGS";
        }
    }

    if (messageEntry &&
        !IsSparseLevelGameplayOrdinal(messageOrdinal, 128u)) return;

    const bool validClient = IsLevelGameplayGuestObject(client) &&
                             PPC_LOAD_U32(client) == kLevelGameplayClientVtable;
    const uint32_t target = (transportEntry || messageEntry)
                                ? (validClient ? PPC_LOAD_U32(client + 656u) : 0u)
                                : context.r3.u32;
    const LevelRegistryNodeSnapshot node =
        SnapshotLevelRegistryNode(base, target);
    uint32_t selectedBefore = 0;
    if (address == kRegistryChild && node.count &&
        context.r4.u32 < node.count && IsLevelGameplayGuestObject(node.items) &&
        context.r4.u32 <= (UINT32_MAX - node.items) / 12u) {
        selectedBefore = PPC_LOAD_U32(node.items + context.r4.u32 * 12u);
    }
    const uint32_t streamHash = HashLevelRegistryBytes(base, stream, 32u);
    const auto timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(levelRegistryUpdateTraceMutex);
    if (levelRegistryUpdateTraceSequence >= kLevelRegistryUpdateTraceLimit) {
        levelRegistryUpdateTraceComplete.store(true, std::memory_order_relaxed);
        return;
    }
    std::filesystem::create_directories("logs");
    std::ofstream out(kLevelRegistryUpdateTracePath,
                      levelRegistryUpdateTraceSequence ? std::ios::app
                                                       : std::ios::trunc);
    out << "LEVEL_REGISTRY_UPDATE sequence=" << levelRegistryUpdateTraceSequence++
        << " timestamp_us=" << timestampUs
        << " kind=" << kind
        << " message_ordinal=" << messageOrdinal
        << " thread=" << CurrentGuestThreadId()
        << std::hex
        << " function=0x" << address
        << " lr=0x" << context.lr
        << " client=0x" << client
        << " current_player_id=0x"
        << (validClient ? PPC_LOAD_U32(client + 536u) : UINT32_MAX)
        << " current_game_id=0x"
        << (validClient ? PPC_LOAD_U32(client + 540u) : UINT32_MAX)
        << " selected_id=0x"
        << (validClient ? PPC_LOAD_U32(client + 3964u) : UINT32_MAX)
        << " view_object=0x"
        << (validClient ? PPC_LOAD_U32(client + 7360u) : 0u)
        << " message=0x" << message
        << " message_type=0x" << messageType
        << " message_source=0x" << messageSource
        << " payload=0x" << payload
        << " stream=0x" << stream
        << " stream_hash32=0x" << streamHash
        << " target=0x" << node.object
        << " target_vtable=0x" << node.vtable
        << " target_flags=0x" << node.flags
        << " target_parent=0x" << node.parent
        << " target_list=0x" << node.list
        << std::dec << " target_count=" << node.count
        << std::hex << " target_items=0x" << node.items
        << " target_first_child=0x" << node.firstChild
        << " target_value=0x" << node.value
        << " r4=0x" << context.r4.u32
        << " r5=0x" << context.r5.u32
        << " r6=0x" << context.r6.u32
        << " selected_before=0x" << selectedBefore
        << " transport_manager=0x" << transportManager
        << " transport_listeners=0x" << transportListeners
        << std::dec << " transport_listener_count=" << transportListenerCount
        << std::hex << " transport_listener_items=0x" << transportListenerItems
        << " transport_connections=0x" << transportConnections
        << std::dec << " transport_connection_count="
        << transportConnectionCount
        << std::hex << " transport_connection_items=0x"
        << transportConnectionItems
        << " transport_index=0x" << transportIndex
        << " transport_listener=0x" << transportListener
        << " transport_connection=0x" << transportConnection
        << " transport_callback=0x" << transportCallback
        << " transport_callback_slot210=0x" << transportCallbackSlot210
        << " dispatch_listener=0x" << dispatchListener
        << " dispatch_listener_vtable=0x" << dispatchListenerVtable
        << " dispatch_callee=0x" << dispatchCallee
        << " dispatch_listener_list=0x" << dispatchListenerList
        << std::dec << " dispatch_listener_count=" << dispatchListenerCount
        << std::hex << " dispatch_listener_items=0x" << dispatchListenerItems
        << " payload_words=";
    for (uint32_t index = 0; index < node.payload.size(); ++index) {
        if (index) out << ',';
        out << "0x" << node.payload[index];
    }
    out << " stream_bytes=";
    if (stream && stream <= UINT32_MAX - 32u) {
        out << std::setfill('0');
        for (uint32_t index = 0; index < 32u; ++index) {
            if (index) out << ',';
            out << std::setw(2)
                << static_cast<uint32_t>(PPC_LOAD_U8(stream + index));
        }
        out << std::setfill(' ');
    } else {
        out << "unavailable";
    }
    out << std::dec << '\n';
    if (mutationEntry) {
        pendingLevelRegistryMutation = {
            true,
            address,
            LevelRegistryMutationFunctionEnd(address),
            context.r1.u32,
            node.object,
            client,
            messageOrdinal,
            kind,
            node,
        };
    }
    if (levelRegistryUpdateTraceSequence >= kLevelRegistryUpdateTraceLimit) {
        levelRegistryUpdateTraceComplete.store(true, std::memory_order_relaxed);
    }
}

bool IsLevelRpgObjectVtable(uint32_t vtable) {
    return std::find(kLevelRpgObjectVtables.begin(),
                     kLevelRpgObjectVtables.end(),
                     vtable) != kLevelRpgObjectVtables.end();
}

bool IsLevelRpgSlotLifecycleAddress(uint32_t address) {
    switch (address) {
    case 0x82304158u: // add a slot object and assign its producer/entity ID
    case 0x82304230u: // clear one slot
    case 0x823042D0u: // clear every slot
    case 0x82304328u: // deactivate one slot through virtual activate(-1)
    case 0x823043B8u: // replace one slot and activate the new entity ID
    case 0x823044B0u: // query/validate the slot's producer-side mapping
    case 0x82304548u: // activate one slot and bind PLAYEROBJ/GAMEOBJ
        return true;
    default:
        return false;
    }
}

struct LevelRpgSlotSnapshot {
    uint32_t object{};
    uint32_t objectVtable{};
    uint32_t objectName0{};
    uint32_t objectName1{};
    uint32_t objectName2{};
    uint32_t owner{};
    uint32_t ownerVtable{};
    uint32_t list{};
    uint32_t listVtable{};
    uint32_t listCapacity{};
    uint32_t listItems{};
    uint32_t slotIndex{UINT32_MAX};
    uint32_t slotEntry{};
    uint32_t slotVtable{};
    uint32_t slotFlagsWord{};
    uint32_t slotEntityId{UINT32_MAX};
    uint32_t slotOwnerId{UINT32_MAX};
    uint32_t slotStoredIndex{UINT32_MAX};
    uint32_t slotField130{UINT32_MAX};
};

LevelRpgSlotSnapshot SnapshotLevelRpgSlot(PPCContext& context,
                                         uint8_t* base,
                                         uint32_t address) {
    LevelRpgSlotSnapshot snapshot{};
    snapshot.object = context.r3.u32;
    if (!IsLevelGameplayGuestObject(snapshot.object) ||
        snapshot.object > UINT32_MAX - 656u) {
        return snapshot;
    }

    snapshot.objectVtable = PPC_LOAD_U32(snapshot.object);
    if (!IsLevelRpgObjectVtable(snapshot.objectVtable)) return snapshot;
    snapshot.objectName0 = PPC_LOAD_U32(snapshot.object + 8u);
    snapshot.objectName1 = PPC_LOAD_U32(snapshot.object + 12u);
    snapshot.objectName2 = PPC_LOAD_U32(snapshot.object + 16u);
    snapshot.owner = PPC_LOAD_U32(snapshot.object + 636u);
    if (IsLevelGameplayGuestObject(snapshot.owner)) {
        snapshot.ownerVtable = PPC_LOAD_U32(snapshot.owner);
    }
    snapshot.list = PPC_LOAD_U32(snapshot.object + 652u);
    if (!IsLevelGameplayGuestObject(snapshot.list) ||
        snapshot.list > UINT32_MAX - 28u) {
        return snapshot;
    }
    snapshot.listVtable = PPC_LOAD_U32(snapshot.list);
    snapshot.listCapacity = PPC_LOAD_U32(snapshot.list + 4u);
    snapshot.listItems = PPC_LOAD_U32(snapshot.list + 24u);

    // Slot-add receives a pointer-like owner whose first word is the new slot
    // object. The remaining family receives a numeric slot index in r4.
    if (address == 0x82304158u) {
        if (IsLevelGameplayGuestObject(context.r4.u32)) {
            snapshot.slotEntry = PPC_LOAD_U32(context.r4.u32);
        }
    } else if (address != 0x823042D0u) {
        snapshot.slotIndex = context.r4.u32;
        if (snapshot.listVtable == kLevelEntityTableVtable &&
            snapshot.slotIndex < snapshot.listCapacity &&
            snapshot.listCapacity <= 65536u &&
            IsLevelGameplayGuestObject(snapshot.listItems) &&
            snapshot.listItems <= UINT32_MAX - snapshot.listCapacity * 4u) {
            snapshot.slotEntry =
                PPC_LOAD_U32(snapshot.listItems + snapshot.slotIndex * 4u);
        }
    }

    if (!IsLevelGameplayGuestObject(snapshot.slotEntry) ||
        snapshot.slotEntry > UINT32_MAX - 308u) {
        return snapshot;
    }
    snapshot.slotVtable = PPC_LOAD_U32(snapshot.slotEntry);
    snapshot.slotFlagsWord = PPC_LOAD_U32(snapshot.slotEntry + 8u);
    snapshot.slotEntityId = PPC_LOAD_U32(snapshot.slotEntry + 292u);
    snapshot.slotOwnerId = PPC_LOAD_U32(snapshot.slotEntry + 296u);
    snapshot.slotStoredIndex = PPC_LOAD_U32(snapshot.slotEntry + 300u);
    snapshot.slotField130 = PPC_LOAD_U32(snapshot.slotEntry + 304u);
    return snapshot;
}

struct LevelGameplayTableSnapshot {
    uint32_t object{};
    uint32_t vtable{};
    uint32_t capacity{};
    uint32_t items{};
    uint32_t nonnull{};
    uint32_t firstIndex{UINT32_MAX};
    uint32_t firstItem{};
    uint32_t worldSpawnCount{};
    uint32_t playerStartCount{};
    uint32_t charNpcCount{};
};

LevelGameplayTableSnapshot SnapshotLevelGameplayTable(uint8_t* base,
                                                      uint32_t object) {
    LevelGameplayTableSnapshot snapshot{};
    snapshot.object = object;
    if (!IsLevelGameplayGuestObject(object)) return snapshot;

    snapshot.vtable = PPC_LOAD_U32(object);
    if (snapshot.vtable != kLevelEntityTableVtable) return snapshot;
    snapshot.capacity = PPC_LOAD_U32(object + 4u);
    snapshot.items = PPC_LOAD_U32(object + 24u);
    if (!snapshot.items || snapshot.capacity > 65536u ||
        snapshot.items > UINT32_MAX - snapshot.capacity * 4u) {
        return snapshot;
    }

    for (uint32_t index = 0; index < snapshot.capacity; ++index) {
        const uint32_t item = PPC_LOAD_U32(snapshot.items + index * 4u);
        if (!item) continue;
        if (!snapshot.nonnull) {
            snapshot.firstIndex = index;
            snapshot.firstItem = item;
        }
        ++snapshot.nonnull;
        if (!IsLevelGameplayGuestObject(item) || item > UINT32_MAX - 364u) continue;
        switch (PPC_LOAD_U32(item + 360u)) {
        case kLevelWorldSpawnDescriptor:
            ++snapshot.worldSpawnCount;
            break;
        case kLevelPlayerStartDescriptor:
            ++snapshot.playerStartCount;
            break;
        case kLevelCharNpcDescriptor:
            ++snapshot.charNpcCount;
            break;
        default:
            break;
        }
    }
    return snapshot;
}

void AppendLevelGameplayFrontierProbe(PPCContext& context, uint8_t* base,
                                      uint32_t address) {
    const bool initialize = address == 0x823F72B0u || address == 0x823ED958u;
    const bool frame = address == 0x823FC818u;
    const bool view = address == 0x823F73E0u || address == 0x823F7F70u ||
                      address == 0x823F8050u || address == 0x823F8AF0u;
    const bool lookup = address == 0x8249D618u;
    const bool spawn = address == 0x8249D938u;
    const bool remove = address == 0x8249DC18u;
    const bool rpgSlotLifecycle = IsLevelRpgSlotLifecycleAddress(address);
    const bool componentBind =
        address == 0x8275D498u &&
        (context.r4.u32 == kLevelPlayerObjectName ||
         context.r4.u32 == kLevelGameObjectName);
    const bool componentQuery =
        address == 0x8275CD80u &&
        (context.lr == 0x823ED9C0u || context.lr == 0x823ED9FCu);
    const bool componentId =
        context.lr == 0x823ED9D8u || context.lr == 0x823EDA14u;
    if (!initialize && !frame && !view && !lookup && !spawn && !remove &&
        !rpgSlotLifecycle && !componentBind && !componentQuery &&
        !componentId) {
        return;
    }

    uint32_t client = levelGameplayClient.load(std::memory_order_relaxed);
    if (IsLevelGameplayGuestObject(context.r3.u32) &&
        PPC_LOAD_U32(context.r3.u32) == kLevelGameplayClientVtable) {
        client = context.r3.u32;
        uint32_t expected = 0;
        levelGameplayClient.compare_exchange_strong(
            expected, client, std::memory_order_relaxed);
    }

    uint64_t ordinal = 0;
    bool shouldLog = false;
    const char* kind = nullptr;
    if (initialize) {
        ordinal = levelGameplayInitEntries.fetch_add(1, std::memory_order_relaxed) + 1;
        shouldLog = ordinal <= 64u;
        kind = address == 0x823ED958u ? "CURRENT_ID_INIT" : "CLIENT_REFRESH";
    } else if (frame) {
        ordinal = levelGameplayFrameEntries.fetch_add(1, std::memory_order_relaxed) + 1;
        shouldLog = IsSparseLevelGameplayOrdinal(ordinal, 8u);
        kind = "FRAME";
    } else if (view) {
        ordinal = levelGameplayViewEntries.fetch_add(1, std::memory_order_relaxed) + 1;
        shouldLog = IsSparseLevelGameplayOrdinal(ordinal, 64u);
        switch (address) {
        case 0x823F73E0u: kind = "VIEW_REFRESH"; break;
        case 0x823F7F70u: kind = "VIEW_SELECT"; break;
        case 0x823F8050u: kind = "VIEW_CREATE"; break;
        default: kind = "VIEW_PREP"; break;
        }
    } else if (lookup) {
        ordinal = levelGameplayLookupEntries.fetch_add(1, std::memory_order_relaxed) + 1;
        shouldLog = IsSparseLevelGameplayOrdinal(ordinal, 8u) ||
                    context.r4.u32 == UINT32_MAX || context.r5.u32 == UINT32_MAX ||
                    context.r4.u32 >= 0xA00u || context.r5.u32 >= 0xA00u;
        kind = "ENTITY_LOOKUP";
    } else if (spawn) {
        ordinal = levelGameplaySpawnEntries.fetch_add(1, std::memory_order_relaxed) + 1;
        shouldLog = IsSparseLevelGameplayOrdinal(ordinal, 64u);
        kind = "ENTITY_SPAWN";
    } else if (remove) {
        ordinal = levelGameplayRemoveEntries.fetch_add(1, std::memory_order_relaxed) + 1;
        shouldLog = IsSparseLevelGameplayOrdinal(ordinal, 64u);
        kind = "ENTITY_REMOVE";
    } else if (rpgSlotLifecycle) {
        ordinal = levelGameplayActivationEntries.fetch_add(
                      1, std::memory_order_relaxed) + 1;
        shouldLog = IsSparseLevelGameplayOrdinal(ordinal, 128u);
        switch (address) {
        case 0x82304158u: kind = "RPG_SLOT_ADD"; break;
        case 0x82304230u: kind = "RPG_SLOT_CLEAR"; break;
        case 0x823042D0u: kind = "RPG_SLOT_CLEAR_ALL"; break;
        case 0x82304328u: kind = "RPG_SLOT_DEACTIVATE"; break;
        case 0x823043B8u: kind = "RPG_SLOT_REPLACE"; break;
        case 0x823044B0u: kind = "RPG_SLOT_VALIDATE"; break;
        default: kind = "RPG_SLOT_ACTIVATE"; break;
        }
    } else if (componentBind) {
        ordinal = levelGameplayComponentBindEntries.fetch_add(
                      1, std::memory_order_relaxed) + 1;
        shouldLog = ordinal <= 128u;
        kind = context.r4.u32 == kLevelPlayerObjectName
                   ? "PLAYEROBJ_BIND"
                   : "GAMEOBJ_BIND";
    } else if (componentQuery) {
        ordinal = levelGameplayQueryEntries.fetch_add(1, std::memory_order_relaxed) + 1;
        shouldLog = ordinal <= 64u;
        kind = context.lr == 0x823ED9C0u ? "COMPONENT_QUERY_0" : "COMPONENT_QUERY_1";
    } else {
        ordinal = levelGameplayComponentIdEntries.fetch_add(
                      1, std::memory_order_relaxed) + 1;
        shouldLog = ordinal <= 64u;
        kind = context.lr == 0x823ED9D8u ? "COMPONENT_ID_0" : "COMPONENT_ID_1";
    }

    uint32_t current0 = UINT32_MAX;
    uint32_t current1 = UINT32_MAX;
    uint32_t clientFlags = 0;
    uint32_t manager = 0;
    uint32_t managerList = 0;
    uint32_t managerCount = 0;
    uint32_t managerItems = 0;
    uint32_t managerFirstObject = 0;
    uint32_t managerFirstNestedList = 0;
    uint32_t selectedId = UINT32_MAX;
    uint32_t viewObject = 0;
    if (IsLevelGameplayGuestObject(client)) {
        current0 = PPC_LOAD_U32(client + 536u);
        current1 = PPC_LOAD_U32(client + 540u);
        clientFlags = PPC_LOAD_U32(client + 516u);
        selectedId = PPC_LOAD_U32(client + 3964u);
        viewObject = PPC_LOAD_U32(client + 7360u);
        manager = PPC_LOAD_U32(client + 656u);
        if (IsLevelGameplayGuestObject(manager) &&
            PPC_LOAD_U32(manager) == kLevelRegistryVtable) {
            managerList = PPC_LOAD_U32(manager + 16u);
            if (IsLevelGameplayGuestObject(managerList)) {
                managerCount = PPC_LOAD_U32(managerList + 4u);
                managerItems = PPC_LOAD_U32(managerList + 24u);
                if (managerCount && IsLevelGameplayGuestObject(managerItems)) {
                    managerFirstObject = PPC_LOAD_U32(managerItems);
                    if (IsLevelGameplayGuestObject(managerFirstObject)) {
                        managerFirstNestedList =
                            PPC_LOAD_U32(managerFirstObject + 16u);
                    }
                }
            }
        }
        if (frame) {
            const uint32_t old0 = levelGameplayLastCurrent0.exchange(
                current0, std::memory_order_relaxed);
            const uint32_t old1 = levelGameplayLastCurrent1.exchange(
                current1, std::memory_order_relaxed);
            const uint32_t oldFlags = levelGameplayLastClientFlags.exchange(
                clientFlags, std::memory_order_relaxed);
            const uint32_t oldManagerCount = levelGameplayLastManagerCount.exchange(
                managerCount, std::memory_order_relaxed);
            const uint32_t oldRegistryFirst = levelGameplayLastRegistryFirst.exchange(
                managerFirstObject, std::memory_order_relaxed);
            const uint32_t oldRegistryFirstChildList =
                levelGameplayLastRegistryFirstChildList.exchange(
                    managerFirstNestedList, std::memory_order_relaxed);
            const uint32_t oldSelectedId = levelGameplayLastSelectedId.exchange(
                selectedId, std::memory_order_relaxed);
            const uint32_t oldViewObject = levelGameplayLastViewObject.exchange(
                viewObject, std::memory_order_relaxed);
            shouldLog |= current0 != old0 || current1 != old1 ||
                         clientFlags != oldFlags || managerCount != oldManagerCount ||
                         managerFirstObject != oldRegistryFirst ||
                         managerFirstNestedList != oldRegistryFirstChildList ||
                         selectedId != oldSelectedId || viewObject != oldViewObject;
        }
    }
    if (!shouldLog) return;

    LevelRpgSlotSnapshot rpgSlot{};
    if (rpgSlotLifecycle) {
        rpgSlot = SnapshotLevelRpgSlot(context, base, address);
    }

    LevelGameplayTableSnapshot low{};
    LevelGameplayTableSnapshot high{};
    uint32_t highIdBase = 0;
    if (client && (frame || initialize || spawn || remove)) {
        low = SnapshotLevelGameplayTable(base, PPC_LOAD_U32(client + 3940u));
        high = SnapshotLevelGameplayTable(base, PPC_LOAD_U32(client + 3952u));
        highIdBase = PPC_LOAD_U32(client + 3956u);
    }

    const auto timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(levelGameplayFrontierMutex);
    if (levelGameplayFrontierSequence >= kLevelGameplayTraceLimit) return;
    std::filesystem::create_directories("logs");
    std::ofstream out(kLevelGameplayFrontierPath,
                      levelGameplayFrontierSequence ? std::ios::app : std::ios::trunc);
    out << "LEVEL_FRONTIER sequence=" << levelGameplayFrontierSequence++
        << " timestamp_us=" << timestampUs
        << " kind=" << kind
        << " ordinal=" << ordinal
        << " thread=" << CurrentGuestThreadId()
        << std::hex << " function=0x" << address
        << " lr=0x" << context.lr
        << " client=0x" << client
        << " client_vtable=0x" <<
               (IsLevelGameplayGuestObject(client) ? PPC_LOAD_U32(client) : 0)
        << " current0=0x" << current0
        << " current1=0x" << current1
        << " r3=0x" << context.r3.u32
        << " r4=0x" << context.r4.u32
        << " r5=0x" << context.r5.u32
        << " r6=0x" << context.r6.u32
        << " r7=0x" << context.r7.u32
        << " r8=0x" << context.r8.u32;
    if (client) {
        out << " client_flags204=0x" << PPC_LOAD_U32(client + 516u)
            << " client_mode208=0x" << PPC_LOAD_U32(client + 520u)
            << " client_owner1b0=0x" << PPC_LOAD_U32(client + 432u)
            << " client_state1b6c=0x" << PPC_LOAD_U32(client + 7020u)
            << " view_pending1ec4=0x"
            << static_cast<uint32_t>(PPC_LOAD_U8(client + 7876u))
            << " client_state2270=0x"
            << static_cast<uint32_t>(PPC_LOAD_U8(client + 8816u))
            << " selected_id=0x" << selectedId
            << " view_object=0x" << viewObject
            << " client_registry290=0x" << manager
            << " registry_flags=0x"
            << (IsLevelGameplayGuestObject(manager)
                    ? PPC_LOAD_U32(manager + 4u) : 0)
            << " registry_parent=0x"
            << (IsLevelGameplayGuestObject(manager)
                    ? PPC_LOAD_U32(manager + 8u) : 0)
            << " registry_payload_words=0x"
            << (IsLevelGameplayGuestObject(manager)
                    ? PPC_LOAD_U32(manager + 24u) : 0)
            << ",0x"
            << (IsLevelGameplayGuestObject(manager)
                    ? PPC_LOAD_U32(manager + 28u) : 0)
            << ",0x"
            << (IsLevelGameplayGuestObject(manager)
                    ? PPC_LOAD_U32(manager + 32u) : 0)
            << " registry_list=0x" << managerList
            << std::dec << " registry_count=" << managerCount
            << std::hex << " registry_items=0x" << managerItems
            << " registry_first_object=0x" << managerFirstObject
            << " registry_first_flags=0x"
            << (IsLevelGameplayGuestObject(managerFirstObject)
                    ? PPC_LOAD_U32(managerFirstObject + 4u) : 0)
            << " registry_first_parent=0x"
            << (IsLevelGameplayGuestObject(managerFirstObject)
                    ? PPC_LOAD_U32(managerFirstObject + 8u) : 0)
            << " registry_first_child_list=0x" << managerFirstNestedList
            << " registry_first_value=0x"
            << (IsLevelGameplayGuestObject(managerFirstObject)
                    ? PPC_LOAD_U32(managerFirstObject + 20u) : 0)
            << " registry_first_payload_words=0x"
            << (IsLevelGameplayGuestObject(managerFirstObject)
                    ? PPC_LOAD_U32(managerFirstObject + 24u) : 0)
            << ",0x"
            << (IsLevelGameplayGuestObject(managerFirstObject)
                    ? PPC_LOAD_U32(managerFirstObject + 28u) : 0)
            << ",0x"
            << (IsLevelGameplayGuestObject(managerFirstObject)
                    ? PPC_LOAD_U32(managerFirstObject + 32u) : 0)
            << ",0x"
            << (IsLevelGameplayGuestObject(managerFirstObject)
                    ? PPC_LOAD_U32(managerFirstObject + 36u) : 0)
            << ",0x"
            << (IsLevelGameplayGuestObject(managerFirstObject)
                    ? PPC_LOAD_U32(managerFirstObject + 40u) : 0)
            << ",0x"
            << (IsLevelGameplayGuestObject(managerFirstObject)
                    ? PPC_LOAD_U32(managerFirstObject + 44u) : 0)
            << " low_table=0x" << low.object
            << " low_vtable=0x" << low.vtable
            << std::dec << " low_capacity=" << low.capacity
            << " low_nonnull=" << low.nonnull
            << " low_worldspawn=" << low.worldSpawnCount
            << " low_playerstart=" << low.playerStartCount
            << " low_charnpc=" << low.charNpcCount
            << std::hex << " low_items=0x" << low.items
            << " low_first_index=0x" << low.firstIndex
            << " low_first_item=0x" << low.firstItem
            << " high_table=0x" << high.object
            << " high_vtable=0x" << high.vtable
            << std::dec << " high_capacity=" << high.capacity
            << " high_nonnull=" << high.nonnull
            << " high_worldspawn=" << high.worldSpawnCount
            << " high_playerstart=" << high.playerStartCount
            << " high_charnpc=" << high.charNpcCount
            << std::hex << " high_items=0x" << high.items
            << " high_id_base=0x" << highIdBase
            << " high_first_index=0x" << high.firstIndex
            << " high_first_item=0x" << high.firstItem
            << " level_name_words=0x" << PPC_LOAD_U32(client + 3972u)
            << ",0x" << PPC_LOAD_U32(client + 3976u)
            << ",0x" << PPC_LOAD_U32(client + 3980u);
    }
    if (rpgSlotLifecycle) {
        out << " rpg_object=0x" << rpgSlot.object
            << " rpg_object_vtable=0x" << rpgSlot.objectVtable
            << " rpg_object_name_words=0x" << rpgSlot.objectName0
            << ",0x" << rpgSlot.objectName1
            << ",0x" << rpgSlot.objectName2
            << " rpg_owner=0x" << rpgSlot.owner
            << " rpg_owner_vtable=0x" << rpgSlot.ownerVtable
            << " rpg_slot_list=0x" << rpgSlot.list
            << " rpg_slot_list_vtable=0x" << rpgSlot.listVtable
            << std::dec << " rpg_slot_capacity=" << rpgSlot.listCapacity
            << std::hex << " rpg_slot_items=0x" << rpgSlot.listItems
            << " rpg_slot_index=0x" << rpgSlot.slotIndex
            << " rpg_slot_entry=0x" << rpgSlot.slotEntry
            << " rpg_slot_vtable=0x" << rpgSlot.slotVtable
            << " rpg_slot_flags_word=0x" << rpgSlot.slotFlagsWord
            << " rpg_slot_entity_id=0x" << rpgSlot.slotEntityId
            << " rpg_slot_owner_id=0x" << rpgSlot.slotOwnerId
            << " rpg_slot_stored_index=0x" << rpgSlot.slotStoredIndex
            << " rpg_slot_field130=0x" << rpgSlot.slotField130;
    }
    if (componentQuery && IsLevelGameplayGuestObject(context.r4.u32)) {
        out << " query_type_word0=0x" << PPC_LOAD_U32(context.r4.u32)
            << " query_type_word1=0x" << PPC_LOAD_U32(context.r4.u32 + 4u);
    }
    if (componentId && IsLevelGameplayGuestObject(context.r3.u32)) {
        out << " component_vtable=0x" << PPC_LOAD_U32(context.r3.u32);
    }
    out << std::dec << '\n';
}

void BeginDeepTrace(PPCContext& context) {
    std::lock_guard<std::mutex> lock(deepTraceMutex);
    if (deepTraceStarted) return;
    deepTraceStarted = true;
    deepTraceThread = CurrentGuestThreadId();
    std::filesystem::create_directories("logs");
    std::ofstream out("logs/m5_state_machine_8259ddd0_trace.log", std::ios::trunc);
    out << "TRACE_BEGIN thread=" << deepTraceThread << " object=0x" << std::hex
        << context.r3.u32 << " value=0x" << context.r4.u32 << " lr=0x" << context.lr
        << " r1=0x" << context.r1.u32 << std::dec << " limit=" << kDeepTraceLimit << '\n';
}

void AppendDeepTrace(const char* kind, uint32_t address, PPCContext& context,
                     const char* detail = nullptr) {
    std::lock_guard<std::mutex> lock(deepTraceMutex);
    if (!deepTraceStarted || deepTraceComplete || CurrentGuestThreadId() != deepTraceThread) return;
    std::ofstream out("logs/m5_state_machine_8259ddd0_trace.log", std::ios::app);
    out << "EVENT index=" << deepTraceEvents << " kind=" << kind << " address=0x" << std::hex
        << address << " lr=0x" << context.lr << " ctr=0x" << context.ctr.u32
        << " r1=0x" << context.r1.u32 << " r3=0x" << context.r3.u32
        << " r4=0x" << context.r4.u32 << std::dec;
    if (detail) out << " detail=" << detail;
    out << '\n';
    if (++deepTraceEvents == kDeepTraceLimit) {
        deepTraceComplete = true;
        out << "TRACE_COMPLETE events=" << deepTraceEvents << '\n';
    }
}

void AppendGraphicsTrace(const char* kind, uint32_t address, PPCContext& context,
                         const char* detail = nullptr) {
    std::lock_guard<std::mutex> lock(graphicsTraceMutex);
    if (!graphicsTraceStarted || graphicsTraceComplete ||
        CurrentGuestThreadId() != graphicsTraceThread)
        return;
    std::ofstream out("logs/m6a_graphics_frontier_trace.log", std::ios::app);
    out << "EVENT index=" << graphicsTraceEvents << " thread=" << CurrentGuestThreadId()
        << " kind=" << kind << " address=0x" << std::hex << address
        << " lr=0x" << context.lr << " ctr=0x" << context.ctr.u32
        << " r1=0x" << context.r1.u32 << " r3=0x" << context.r3.u32
        << " r4=0x" << context.r4.u32 << " r5=0x" << context.r5.u32
        << " r6=0x" << context.r6.u32 << " r7=0x" << context.r7.u32
        << " r8=0x" << context.r8.u32 << " r9=0x" << context.r9.u32 << std::dec;
    if (detail) out << " detail=" << detail;
    out << '\n';
    if (++graphicsTraceEvents == kDeepTraceLimit) {
        graphicsTraceComplete = true;
        out << "TRACE_COMPLETE events=" << graphicsTraceEvents << '\n';
    }
}

void AppendPostSwapTrace(const char* kind, uint32_t address, PPCContext& context,
                         const char* detail = nullptr) {
    std::lock_guard<std::mutex> lock(postSwapTraceMutex);
    if (!postSwapTraceStarted || postSwapTraceComplete ||
        CurrentGuestThreadId() != postSwapTraceThread)
        return;
    uint32_t occurrence = 0;
    if (kind[0] == 'F') {
        const uint64_t callSite = (uint64_t(address) << 32) | context.lr;
        occurrence = ++postSwapFunctionOccurrences[callSite];
        // Gamma-ramp generation and other title loops invoke the same leaf
        // routines thousands of times. Keep the first two observations and
        // powers of two so their progress remains visible without consuming
        // the bounded trace before the enclosing state machine advances.
        if (occurrence > 2 && (occurrence & (occurrence - 1)) != 0) return;
    }
    std::ofstream out("logs/m6d_post_swap_guest_trace_v2.log", std::ios::app);
    out << "EVENT index=" << postSwapTraceEvents << " thread=" << CurrentGuestThreadId()
        << " kind=" << kind << " address=0x" << std::hex << address
        << " lr=0x" << context.lr << " ctr=0x" << context.ctr.u32
        << " r1=0x" << context.r1.u32 << " r3=0x" << context.r3.u32
        << " r4=0x" << context.r4.u32 << " r5=0x" << context.r5.u32
        << " r6=0x" << context.r6.u32 << " r7=0x" << context.r7.u32
        << " r8=0x" << context.r8.u32 << " r9=0x" << context.r9.u32 << std::dec;
    if (occurrence) out << " occurrence=" << occurrence;
    if (detail) out << " detail=" << detail;
    out << '\n';
    if (++postSwapTraceEvents == kPostSwapTraceLimit) {
        postSwapTraceComplete = true;
        out << "TRACE_COMPLETE events=" << postSwapTraceEvents << '\n';
    }
}

std::shared_ptr<TraceThread> CurrentTrace() {
    if (currentTrace) return currentTrace;
    currentTrace = std::make_shared<TraceThread>();
    currentTrace->id.store(CurrentGuestThreadId(), std::memory_order_relaxed);
    currentTrace->guestObject.store(CurrentGuestThreadObject(), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(traceMutex);
    traceThreads.push_back(currentTrace);
    return currentTrace;
}

void AppendFunctionHistory(std::ofstream& out, uint32_t limit = 64u) {
    const auto trace = CurrentTrace();
    const uint32_t write = trace->functionWrite.load(std::memory_order_relaxed);
    const uint32_t historyCount = std::min<uint32_t>(write, limit);
    for (uint32_t i = historyCount; i; --i) {
        const uint32_t slot = (write - i) % kFunctionHistory;
        out << (i == historyCount ? "0x" : ",0x") << std::hex
            << trace->functions[slot].load(std::memory_order_relaxed);
    }
}

void AppendAllocatorReferenceProvider(PPCContext& context, uint8_t* base,
                                      uint32_t address) {
    constexpr uint32_t kProviderCallerConstruct = 0x8273C09Cu;
    constexpr uint32_t kProviderCallerRefresh = 0x8273D0DCu;
    constexpr uint32_t kCopyAssignment = 0x821FCD58u;
    constexpr uint32_t kAssignmentCallerConstruct = 0x8273C0ACu;
    constexpr uint32_t kAssignmentCallerRefresh = 0x8273D0ECu;
    const bool providerEntry = context.lr == kProviderCallerConstruct ||
                               context.lr == kProviderCallerRefresh;
    const bool providerAssignment = address == kCopyAssignment &&
        (context.lr == kAssignmentCallerConstruct ||
         context.lr == kAssignmentCallerRefresh);
    if (!providerEntry && !providerAssignment) return;

    const uint32_t sequence = allocatorReferenceProviderSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence >= 4096u) return;

    // These ranges cover the mapped title image, guest heap, and guest thread
    // stacks used by the two verified call sites. Avoid speculative reads from
    // scalar arguments or invalid pointers while retaining the full object and
    // returned-buffer evidence needed to identify the dynamic provider.
    const auto mappedObject = [](uint32_t pointer, uint32_t bytes) {
        return pointer >= 0x60000000u && pointer < 0xC0000000u &&
               bytes <= 0xC0000000u - pointer;
    };
    const auto loadMapped = [&](uint32_t pointer, uint32_t offset) {
        return mappedObject(pointer, offset + 4u)
            ? PPC_LOAD_U32(pointer + offset) : 0u;
    };

    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::lock_guard<std::mutex> lock(allocatorReferenceProviderLogMutex);
    std::ofstream out("logs/m6d_allocator_reference_provider.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "ALLOCATOR_REFERENCE_PROVIDER sequence=" << sequence
        << " event=" << (providerEntry ? "provider-enter" :
                                      "provider-result-assignment")
        << " thread=" << CurrentGuestThreadId()
        << " function=0x" << std::hex << address
        << " caller_lr=0x" << context.lr;
    if (providerEntry) {
        const uint32_t destination = context.r3.u32;
        const uint32_t source = context.r4.u32;
        out << " destination=0x" << destination
            << " source=0x" << source
            << " source_vtable=0x" << loadMapped(source, 0u);
        for (uint32_t offset = 0; offset <= 0x3Cu; offset += 4u) {
            out << " source" << std::setw(2) << std::setfill('0') << offset
                << "=0x" << loadMapped(source, offset);
        }
        out << std::setfill(' ')
            << " r5=0x" << context.r5.u32
            << " r6=0x" << context.r6.u32;
    } else {
        const uint32_t destinationSlot = context.r3.u32;
        const uint32_t sourceSlot = context.r4.u32;
        const uint32_t returnedPointer = loadMapped(sourceSlot, 0u);
        const uint32_t item = destinationSlot >= 12u
            ? destinationSlot - 12u : 0u;
        out << " destination_slot=0x" << destinationSlot
            << " source_slot=0x" << sourceSlot
            << " destination_before=0x" << loadMapped(destinationSlot, 0u)
            << " returned_pointer=0x" << returnedPointer
            << " item=0x" << item
            << " item_vtable=0x" << loadMapped(item, 0u)
            << " item_parent=0x" << loadMapped(item, 8u);
        for (uint32_t offset = 0; offset <= 0x1Cu; offset += 4u) {
            out << " returned" << std::setw(2) << std::setfill('0') << offset
                << "=0x" << loadMapped(returnedPointer, offset);
        }
        out << std::setfill(' ');
    }
    out << " stack=0x" << context.r1.u32 << " history=";
    AppendFunctionHistory(out, 48u);
    out << std::dec << '\n';
}

void AppendAllocatorRegistryProducer(PPCContext& context, uint8_t* base,
                                     uint32_t address) {
    constexpr uint32_t kProducer = 0x820C2B68u;
    constexpr uint32_t kParseCallReturn = 0x820C2C78u;
    constexpr uint32_t kParseResultObservation = 0x820C2CB4u;
    constexpr uint32_t kRecordCallReturn = 0x820C2CECu;
    constexpr uint32_t kRecordResultObservation = 0x820C2CFCu;
    constexpr uint32_t kSelector = 0x820C1338u;
    constexpr uint32_t kSelectorCallReturn = 0x820C2D18u;
    constexpr uint32_t kCloneCallReturn = 0x820C2D3Cu;
    constexpr uint32_t kVectorCleanup = 0x8276E808u;
    constexpr uint32_t kVectorCleanupCaller = 0x820C2D90u;
    constexpr uint32_t kClone = 0x8273D060u;
    constexpr uint32_t kDeepCopy = 0x8273B468u;
    constexpr uint32_t kDeepCopyCaller = 0x8273D0C4u;
    constexpr uint32_t kExists = 0x821FFB90u;
    constexpr uint32_t kCandidateExistsCaller = 0x820C1414u;
    constexpr uint32_t kOriginalExistsCaller = 0x820C1474u;
    constexpr uint32_t kRootVtable = 0x82095798u;
    constexpr uint32_t kProxyVtable = 0x820960F8u;

    const bool producerEntry = address == kProducer;
    const bool parseEntry = context.lr == kParseCallReturn;
    const bool parseResult = context.lr == kParseResultObservation;
    const bool recordEntry = context.lr == kRecordCallReturn;
    const bool recordResult = context.lr == kRecordResultObservation;
    const bool selectorEntry = address == kSelector &&
                               context.lr == kSelectorCallReturn;
    const bool cloneEntry = context.lr == kCloneCallReturn;
    const bool cleanupEntry = address == kVectorCleanup &&
                              context.lr == kVectorCleanupCaller;
    const bool anyCloneEntry = address == kClone;
    const bool deepCopyEntry = address == kDeepCopy &&
                               context.lr == kDeepCopyCaller;
    const bool existsEntry = address == kExists &&
        (context.lr == kCandidateExistsCaller ||
         context.lr == kOriginalExistsCaller);

    const auto mappedObject = [](uint32_t pointer, uint32_t bytes) {
        return pointer >= 0x60000000u && pointer < 0xC0000000u &&
               bytes <= 0xC0000000u - pointer;
    };
    const auto loadMapped = [&](uint32_t pointer, uint32_t offset) {
        return mappedObject(pointer, offset + 4u)
            ? PPC_LOAD_U32(pointer + offset) : 0u;
    };

    if (selectorEntry) {
        allocatorRegistryCloneStats = {};
        allocatorRegistryCloneStats.active = true;
        allocatorRegistryCloneStats.path =
            ReadStarbreezeNarrowString(base, context.r4.u32);
    }
    if (allocatorRegistryCloneStats.active && anyCloneEntry) {
        ++allocatorRegistryCloneStats.cloneCalls;
        const uint32_t sourceVtable = loadMapped(context.r4.u32, 0u);
        if (sourceVtable == kRootVtable) {
            ++allocatorRegistryCloneStats.rootSources;
        } else if (sourceVtable == kProxyVtable) {
            ++allocatorRegistryCloneStats.proxySources;
        } else {
            ++allocatorRegistryCloneStats.otherSources;
        }
    }
    if (allocatorRegistryCloneStats.active && deepCopyEntry) {
        ++allocatorRegistryCloneStats.deepCopyCalls;
        const uint32_t sourceVtable = loadMapped(context.r4.u32, 0u);
        if (sourceVtable == kRootVtable) {
            ++allocatorRegistryCloneStats.deepRootSources;
        } else if (sourceVtable == kProxyVtable) {
            ++allocatorRegistryCloneStats.deepProxySources;
        } else {
            ++allocatorRegistryCloneStats.deepOtherSources;
        }
    }
    if (!producerEntry && !parseEntry && !parseResult && !recordEntry &&
        !recordResult && !selectorEntry && !cloneEntry && !cleanupEntry &&
        !existsEntry) {
        return;
    }

    const uint32_t sequence = allocatorRegistryProducerSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence >= 4096u) return;

    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::lock_guard<std::mutex> lock(allocatorRegistryProducerLogMutex);
    std::ofstream out("logs/m6d_allocator_registry_producer.log",
                      sequence ? std::ios::app : std::ios::trunc);
    const char* event = producerEntry ? "producer-enter" :
        parseEntry ? "parse-enter" :
        parseResult ? "parse-result" :
        recordEntry ? "record-enter" :
        recordResult ? "record-result" :
        selectorEntry ? "selector-enter" :
        existsEntry ? (context.lr == kCandidateExistsCaller
                           ? "candidate-exists-enter"
                           : "original-exists-enter") :
        cloneEntry ? "clone-enter" : "vector-cleanup";
    out << "ALLOCATOR_REGISTRY_PRODUCER sequence=" << sequence
        << " event=" << event
        << " thread=" << CurrentGuestThreadId()
        << " function=0x" << std::hex << address
        << " caller_lr=0x" << context.lr
        << " stack=0x" << context.r1.u32
        << " r3=0x" << context.r3.u32
        << " r4=0x" << context.r4.u32
        << " r5=0x" << context.r5.u32
        << " r6=0x" << context.r6.u32
        << " r20=0x" << context.r20.u32
        << " r21=0x" << context.r21.u32
        << " r22=0x" << context.r22.u32
        << " r23=0x" << context.r23.u32
        << " r29=0x" << context.r29.u32
        << std::dec;
    if (selectorEntry) {
        out << " path=\"" << ReadStarbreezeNarrowString(base, context.r4.u32)
            << "\"";
    } else if (existsEntry) {
        out << " path=\"" << ReadStarbreezeNarrowString(base, context.r3.u32)
            << "\"";
    }
    out << '\n';

    const auto dumpWords = [&](const char* label, uint32_t pointer,
                               uint32_t words) {
        out << "  " << label << "=0x" << std::hex << pointer;
        if (!mappedObject(pointer, words * 4u)) {
            out << " unmapped" << std::dec << '\n';
            return;
        }
        for (uint32_t i = 0; i < words; ++i) {
            out << " +" << std::setw(2) << std::setfill('0') << (i * 4u)
                << "=0x" << loadMapped(pointer, i * 4u);
        }
        out << std::setfill(' ') << std::dec << '\n';
    };

    dumpWords("r3-object", context.r3.u32, 16u);
    dumpWords("r4-object", context.r4.u32, 24u);
    dumpWords("r5-object", context.r5.u32, 8u);
    if (mappedObject(context.r4.u32, 4u)) {
        dumpWords("r4-vtable", loadMapped(context.r4.u32, 0u), 24u);
    }

    if (parseResult || recordResult || selectorEntry || cloneEntry ||
        cleanupEntry) {
        // Function-entry hooks run before the generated callee prologue, so
        // r1 is still sub_820C2B68's frame at every selected call boundary.
        const uint32_t callerStack = context.r1.u32;
        dumpWords("producer-stack", callerStack + 64u, 28u);
        const uint32_t parseRecord = loadMapped(callerStack, 100u);
        dumpWords("parse-record", parseRecord, 12u);
        dumpWords("parse-backing", loadMapped(parseRecord, 24u), 12u);
        const uint32_t vectorObject = callerStack + 128u;
        const uint32_t vectorOwner = loadMapped(vectorObject, 8u);
        const uint32_t vectorBacking = loadMapped(vectorOwner, 0u);
        dumpWords("result-vector", vectorObject, 4u);
        dumpWords("result-vector-owner", vectorOwner, 4u);
        dumpWords("result-vector-backing", vectorBacking, 20u);
    }

    if (cleanupEntry && allocatorRegistryCloneStats.active) {
        out << "  clone-summary path=\"" << allocatorRegistryCloneStats.path
            << "\" clones=" << allocatorRegistryCloneStats.cloneCalls
            << " root_sources=" << allocatorRegistryCloneStats.rootSources
            << " proxy_sources=" << allocatorRegistryCloneStats.proxySources
            << " other_sources=" << allocatorRegistryCloneStats.otherSources
            << " deep_copies=" << allocatorRegistryCloneStats.deepCopyCalls
            << " deep_root_sources="
            << allocatorRegistryCloneStats.deepRootSources
            << " deep_proxy_sources="
            << allocatorRegistryCloneStats.deepProxySources
            << " deep_other_sources="
            << allocatorRegistryCloneStats.deepOtherSources << '\n';
    }

    out << "  history=";
    AppendFunctionHistory(out, 64u);
    out << std::dec << '\n';
    if (cleanupEntry) allocatorRegistryCloneStats = {};
}

void RecordAllocatorSharedControlEvent(PPCContext& context, uint8_t* base,
                                       uint32_t address) {
    constexpr uint32_t kSharedCopy = 0x8224EBA8u;
    constexpr uint32_t kSharedRelease = 0x8257A4C8u;
    constexpr uint32_t kCopyReleaseCallsite = 0x8224EC10u;
    if (address != kSharedCopy && address != kSharedRelease) return;

    const auto heapPointer = [](uint32_t pointer, uint32_t bytes) {
        return pointer >= 0xA0000000u && pointer < 0xC0000000u &&
               bytes <= 0xC0000000u - pointer;
    };
    AllocatorSharedControlEvent event{};
    event.thread = CurrentGuestThreadId();
    event.function = address;
    event.lr = context.lr;
    if (address == kSharedCopy) {
        event.kind = 1u; // copy-entry, before the source strong-count increment.
        event.destination = context.r3.u32;
        event.source = context.r4.u32;
        if (heapPointer(event.source, 8u)) {
            event.control = PPC_LOAD_U32(event.source + 4u);
        }
    } else {
        event.outer = context.r3.u32;
        if (heapPointer(event.outer, 8u)) {
            event.control = PPC_LOAD_U32(event.outer + 4u);
        }
        if (context.lr == kCopyReleaseCallsite) {
            // sub_8224EBA8 has already incremented the source control when it
            // calls the destination release. Saved nonvolatile registers keep
            // both wrapper addresses available at this exact boundary.
            event.kind = 2u;
            event.source = context.r31.u32;
            event.destination = context.r30.u32;
            if (heapPointer(event.source, 8u)) {
                event.control = PPC_LOAD_U32(event.source + 4u);
            }
        } else {
            event.kind = 3u; // release-entry, before the decrement.
        }
    }
    if (!heapPointer(event.control, 28u)) return;
    event.count = PPC_LOAD_U32(event.control + 20u);
    event.backing = PPC_LOAD_U32(event.control + 24u);
    event.size = PPC_LOAD_U32(event.control + 8u);
    const auto trace = CurrentTrace();
    const uint32_t write = trace->functionWrite.load(std::memory_order_relaxed);
    event.historyCount = std::min<uint32_t>(write, 24u);
    for (uint32_t i = event.historyCount; i; --i) {
        const uint32_t slot = (write - i) % kFunctionHistory;
        event.history[event.historyCount - i] =
            trace->functions[slot].load(std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock(allocatorSharedControlMutex);
    event.ordinal = allocatorSharedControlOrdinal++;
    // This history is diagnostic-only and is discarded after process exit.
    // The cap bounds memory even during unusually long content-streaming runs.
    if (allocatorSharedControlEvents.size() < 131072u) {
        allocatorSharedControlEvents.push_back(event);
    }
}

void DumpAllocatorSharedControlHistory(uint32_t control, uint32_t backing,
                                       uint32_t item) {
    if (!control) return;
    std::vector<AllocatorSharedControlEvent> matches;
    {
        std::lock_guard<std::mutex> lock(allocatorSharedControlMutex);
        for (const auto& event : allocatorSharedControlEvents) {
            if (event.control == control) matches.push_back(event);
        }
    }
    const uint32_t sequence = allocatorSharedControlDumpSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence >= 256u) return;
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::lock_guard<std::mutex> lock(allocatorSharedControlDumpLogMutex);
    std::ofstream out("logs/m6d_allocator_shared_control_history.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "ALLOCATOR_SHARED_CONTROL_HISTORY sequence=" << sequence
        << " control=0x" << std::hex << control
        << " backing=0x" << backing << " item=0x" << item << std::dec
        << " events=" << matches.size() << '\n';
    for (const auto& event : matches) {
        const char* kind = event.kind == 1u ? "copy-entry" :
            event.kind == 2u ? "copy-after-increment" : "release-entry";
        out << "  ordinal=" << event.ordinal << " kind=" << kind
            << " thread=" << event.thread
            << " function=0x" << std::hex << event.function
            << " caller_lr=0x" << event.lr
            << " outer=0x" << event.outer
            << " source=0x" << event.source
            << " destination=0x" << event.destination
            << " count=0x" << event.count
            << " size=0x" << event.size
            << " backing=0x" << event.backing << " history=";
        for (uint32_t i = 0; i < event.historyCount; ++i) {
            out << (i ? ",0x" : "0x") << event.history[i];
        }
        out << std::dec << '\n';
    }
}

void AppendAllocatorTargetReferenceLifecycle(PPCContext& context, uint8_t* base,
                                             uint32_t address) {
    constexpr uint32_t kIncrement = 0x821F7200u;
    constexpr uint32_t kDecrement = 0x821F7260u;
    constexpr uint32_t kGeneralFree = 0x82216270u;
    if (address != kIncrement && address != kDecrement &&
        address != kGeneralFree) {
        return;
    }

    const uint32_t reference = address == kGeneralFree
        ? context.r4.u32 : context.r3.u32;
    uint32_t item = 0;
    {
        std::lock_guard<std::mutex> lock(allocatorItemPointerAssignmentMutex);
        const auto it = allocatorTargetReferenceItems.find(reference);
        if (it == allocatorTargetReferenceItems.end()) return;
        item = it->second;
    }

    const uint32_t sequence = allocatorTargetReferenceLifecycleSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence >= 16384u) return;
    const uint32_t word = PPC_LOAD_U32(reference);
    const uint32_t count = (word >> 16) & 0x1FFFu;
    const char* operation = address == kIncrement ? "increment" :
        address == kDecrement ? "decrement" : "free";
    const int32_t expectedCount = address == kIncrement
        ? static_cast<int32_t>(count) + 1
        : address == kDecrement ? static_cast<int32_t>(count) - 1
                                : static_cast<int32_t>(count);

    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::lock_guard<std::mutex> lock(allocatorTargetReferenceLifecycleLogMutex);
    std::ofstream out("logs/m6d_allocator_target_reference_lifecycle.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "ALLOCATOR_TARGET_REFERENCE_LIFECYCLE sequence=" << sequence
        << " operation=" << operation
        << " thread=" << CurrentGuestThreadId()
        << " function=0x" << std::hex << address
        << " caller_lr=0x" << context.lr
        << " reference=0x" << reference
        << " item=0x" << item
        << " word=0x" << word
        << std::dec << " count_before=" << count
        << " expected_count_after=" << expectedCount
        << " stack=0x" << std::hex << context.r1.u32
        << " r28=0x" << context.r28.u32
        << " r29=0x" << context.r29.u32
        << " r30=0x" << context.r30.u32
        << " r31=0x" << context.r31.u32
        << " history=";
    AppendFunctionHistory(out, 32u);
    out << std::dec << '\n';
}

void AppendAllocatorOwnerConsumerOrder(PPCContext& context, uint8_t* base,
                                       uint32_t address) {
    constexpr uint32_t kBackingOwnerRelease = 0x8257A4C8u;
    constexpr uint32_t kProducerVectorRelease = 0x8276E808u;
    constexpr uint32_t kConsumerDispatch = 0x82368C90u;
    constexpr uint32_t kContainerConsumer = 0x8236A7F0u;
    if (address != kBackingOwnerRelease &&
        address != kProducerVectorRelease &&
        address != kConsumerDispatch && address != kContainerConsumer) {
        return;
    }

    uint32_t outer = 0;
    uint32_t control = 0;
    uint32_t backing = 0;
    uint32_t controlCount = 0;
    uint32_t backingWord = 0;
    uint32_t vector = 0;
    uint32_t vectorCount = 0;
    uint32_t targetVectorReferences = 0;
    AllocatorGeneralPointerState controlAllocation{};
    bool targetBacking = false;
    if (address == kBackingOwnerRelease) {
        outer = context.r3.u32;
        if (outer >= 0xA0000000u && outer < 0xC0000000u) {
            control = PPC_LOAD_U32(outer + 4u);
        }
        if (control >= 0xA0000000u && control < 0xC0000000u) {
            controlCount = PPC_LOAD_U32(control + 20u);
            backing = PPC_LOAD_U32(control + 24u);
            std::lock_guard<std::mutex> lock(allocatorGeneralLifecycleMutex);
            const auto allocation = allocatorGeneralPointers.find(control);
            if (allocation != allocatorGeneralPointers.end()) {
                controlAllocation = allocation->second;
            }
        }
        if (backing >= 0xA0000000u && backing < 0xC0000000u) {
            backingWord = PPC_LOAD_U32(backing);
            std::lock_guard<std::mutex> lock(allocatorItemPointerAssignmentMutex);
            targetBacking = allocatorTargetReferenceItems.find(backing) !=
                            allocatorTargetReferenceItems.end();
        }
        if (!targetBacking) return;
    } else if (address == kProducerVectorRelease) {
        if (context.r3.u32 >= 0xA0000000u && context.r3.u32 < 0xC0000000u) {
            vector = PPC_LOAD_U32(context.r3.u32);
        }
        if (vector >= 0xA0000000u && vector < 0xC0000000u) {
            vectorCount = PPC_LOAD_U32(vector);
        }
        if (!vectorCount || vectorCount > 4096u) return;
        std::lock_guard<std::mutex> lock(allocatorItemPointerAssignmentMutex);
        for (uint32_t i = 0; i < vectorCount; ++i) {
            const uint32_t record = vector + 4u + i * 16u;
            const uint32_t recordControl = PPC_LOAD_U32(record + 8u);
            if (recordControl < 0xA0000000u || recordControl >= 0xC0000000u) {
                continue;
            }
            const uint32_t recordBacking = PPC_LOAD_U32(recordControl + 24u);
            if (allocatorTargetReferenceItems.find(recordBacking) !=
                allocatorTargetReferenceItems.end()) {
                ++targetVectorReferences;
            }
        }
        if (!targetVectorReferences) return;
    }

    const uint32_t sequence = allocatorOwnerConsumerOrderSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence >= 1024u) return;
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::lock_guard<std::mutex> lock(allocatorOwnerConsumerOrderLogMutex);
    std::ofstream out("logs/m6d_allocator_owner_consumer_order.log",
                      sequence ? std::ios::app : std::ios::trunc);
    const char* event = address == kBackingOwnerRelease
        ? "backing-owner-release"
        : address == kProducerVectorRelease
            ? "producer-vector-release"
            : address == kConsumerDispatch
                ? "consumer-dispatch-entry"
                : "container-consumer";
    out << "ALLOCATOR_OWNER_CONSUMER_ORDER sequence=" << sequence
        << " event=" << event
        << " thread=" << CurrentGuestThreadId()
        << " function=0x" << std::hex << address
        << " caller_lr=0x" << context.lr
        << " r3=0x" << context.r3.u32
        << " r4=0x" << context.r4.u32
        << " r5=0x" << context.r5.u32
        << " stack=0x" << context.r1.u32;
    if (address == kBackingOwnerRelease) {
        out << " outer=0x" << outer
            << " control=0x" << control
            << " control_count=0x" << controlCount
            << " control00=0x" << (control ? PPC_LOAD_U32(control) : 0u)
            << " control08=0x" << (control ? PPC_LOAD_U32(control + 8u) : 0u)
            << " control10=0x" << (control ? PPC_LOAD_U32(control + 16u) : 0u)
            << " backing=0x" << backing
            << " backing_word=0x" << backingWord
            << std::dec
            << " control_allocation_known=" << controlAllocation.known
            << " control_allocation_live=" << controlAllocation.allocated
            << " control_allocation_size=" << controlAllocation.allocationSize
            << " control_allocation_ordinal=" << controlAllocation.allocationOrdinal
            << " control_allocation_thread=" << controlAllocation.allocationThread
            << " control_allocation_caller=0x" << std::hex
            << controlAllocation.allocationCaller;
    } else if (address == kProducerVectorRelease) {
        out << " vector=0x" << vector
            << std::dec << " vector_count=" << vectorCount
            << " target_vector_references=" << targetVectorReferences
            << std::hex;
    }
    out << " history=";
    AppendFunctionHistory(out, 48u);
    out << std::dec << '\n';
}

void AppendAllocatorStaleSmartPointer(PPCContext& context, uint8_t* base,
                                      uint32_t address) {
    constexpr uint32_t kSmartPointerDestructor = 0x821F8AD0u;
    if (address != kSmartPointerDestructor || context.r3.u32 > 0xFFFFFFFBu) {
        return;
    }

    const uint32_t object = context.r3.u32;
    const uint32_t pointer = PPC_LOAD_U32(object + 4u);
    if (!pointer) return;

    AllocatorGeneralPointerState state{};
    {
        std::lock_guard<std::mutex> lock(allocatorGeneralLifecycleMutex);
        const auto it = allocatorGeneralPointers.find(pointer);
        if (it == allocatorGeneralPointers.end() || !it->second.known ||
            it->second.allocated) {
            return;
        }
        state = it->second;
    }

    const uint32_t sequence = allocatorSmartPointerStaleSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence >= 64u) return;

    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::ofstream out("logs/m6d_allocator_stale_smart_pointer_watch.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "ALLOCATOR_STALE_SMART_POINTER sequence=" << sequence
        << " thread=" << CurrentGuestThreadId()
        << " caller=0x" << std::hex << context.lr
        << " object=0x" << object
        << " object_vtable=0x" << PPC_LOAD_U32(object)
        << " pointer=0x" << pointer
        << " pointer00=0x" << PPC_LOAD_U32(pointer)
        << " pointer04=0x" << PPC_LOAD_U32(pointer + 4u)
        << " pointer08=0x" << PPC_LOAD_U32(pointer + 8u)
        << " stack=0x" << context.r1.u32
        << " r20=0x" << context.r20.u32
        << " r21=0x" << context.r21.u32
        << " r22=0x" << context.r22.u32
        << " r23=0x" << context.r23.u32
        << " r24=0x" << context.r24.u32
        << " r25=0x" << context.r25.u32
        << " r26=0x" << context.r26.u32
        << " r27=0x" << context.r27.u32
        << " r28=0x" << context.r28.u32
        << " r29=0x" << context.r29.u32
        << " r30=0x" << context.r30.u32
        << " r31=0x" << context.r31.u32
        << std::dec << " allocation_ordinal=" << state.allocationOrdinal
        << " allocation_thread=" << state.allocationThread
        << " allocation_caller=0x" << std::hex << state.allocationCaller
        << " allocation_size=0x" << state.allocationSize
        << " allocation_alignment=0x" << state.allocationAlignment
        << std::dec << " free_ordinal=" << state.freeOrdinal
        << " free_thread=" << state.freeThread
        << " free_caller=0x" << std::hex << state.freeCaller
        << " free_stack=0x" << state.freeStack
        << " free_r3=0x" << state.freeR3
        << " free_r28=0x" << state.freeR28
        << " free_r30=0x" << state.freeR30
        << " free_r31=0x" << state.freeR31
        << " history=";
    AppendFunctionHistory(out);
    out << std::dec << '\n';
}

void AppendAllocatorSmartPointerSource(PPCContext& context, uint8_t* base,
                                       uint32_t address) {
    // sub_8236A7F0 invokes this virtual constructor at 0x8236AA58. It copies
    // item+0x0C into the temporary later destroyed at 0x8236AA84, so this is
    // the earliest point at which the observed stale reference enters the
    // worker's local state.
    constexpr uint32_t kItemSmartPointerConstructor = 0x8273E288u;
    constexpr uint32_t kCaller = 0x8236AA58u;
    if (address != kItemSmartPointerConstructor || context.lr != kCaller) return;

    const uint32_t destination = context.r3.u32;
    const uint32_t item = context.r4.u32;
    if (!item) return;
    const uint32_t pointer = PPC_LOAD_U32(item + 12u);
    const uint32_t sequence = allocatorSmartPointerSourceSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence >= 256u) return;

    AllocatorGeneralPointerState state{};
    bool tracked = false;
    if (pointer) {
        std::lock_guard<std::mutex> lock(allocatorGeneralLifecycleMutex);
        const auto it = allocatorGeneralPointers.find(pointer);
        if (it != allocatorGeneralPointers.end()) {
            state = it->second;
            tracked = state.known;
        }
    }

    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::ofstream out("logs/m6d_allocator_smart_pointer_source_watch.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "ALLOCATOR_SMART_POINTER_SOURCE sequence=" << sequence
        << " thread=" << CurrentGuestThreadId()
        << " caller=0x" << std::hex << context.lr
        << " destination=0x" << destination
        << " destination_vtable=0x" << PPC_LOAD_U32(destination)
        << " destination_pointer=0x" << PPC_LOAD_U32(destination + 4u)
        << " item=0x" << item
        << " item00=0x" << PPC_LOAD_U32(item)
        << " item04=0x" << PPC_LOAD_U32(item + 4u)
        << " item08=0x" << PPC_LOAD_U32(item + 8u)
        << " item0c=0x" << pointer
        << " item10=0x" << PPC_LOAD_U32(item + 16u)
        << " item14=0x" << PPC_LOAD_U32(item + 20u)
        << " pointer00=0x" << (pointer ? PPC_LOAD_U32(pointer) : 0u)
        << " pointer04=0x" << (pointer ? PPC_LOAD_U32(pointer + 4u) : 0u)
        << " block04=0x" << (pointer >= 16u ? PPC_LOAD_U32(pointer - 12u) : 0u)
        << " owner=0x" << context.r24.u32
        << " count=0x" << context.r25.u32
        << " container=0x" << context.r28.u32
        << " index=0x" << context.r29.u32
        << " argument=0x" << context.r30.u32
        << " selected=0x" << context.r31.u32
        << " stack=0x" << context.r1.u32
        << std::dec << " tracked=" << tracked
        << " tracked_allocated=" << state.allocated
        << " allocation_ordinal=" << state.allocationOrdinal
        << " allocation_caller=0x" << std::hex << state.allocationCaller
        << std::dec << " free_ordinal=" << state.freeOrdinal
        << " free_thread=" << state.freeThread
        << " free_caller=0x" << std::hex << state.freeCaller
        << " history=";
    AppendFunctionHistory(out);
    out << std::dec << '\n';
}

void AppendAllocatorGeneralLifecycle(PPCContext& context, uint8_t* base,
                                     uint32_t address) {
    constexpr uint32_t kAllocate = 0x82215978u;
    constexpr uint32_t kFree = 0x82216270u;

    if (address == kAllocate) {
        allocatorGeneralAllocationRequests.push_back({
            context.r3.u32, context.r4.u32, context.r5.u32,
            static_cast<uint32_t>(context.lr), context.r1.u32});
        return;
    }
    if (address != kFree || context.r4.u32 < 16u) return;

    const uint32_t pointer = context.r4.u32;
    const uint32_t block = pointer - 16u;
    const uint32_t blockFlags = PPC_LOAD_U32(block + 4u);
    const uint64_t ordinal = allocatorGeneralLifecycleOrdinal.fetch_add(
                                 1, std::memory_order_relaxed) + 1u;
    AllocatorGeneralPointerState previous{};
    bool lifecycleDuplicate = false;
    {
        std::lock_guard<std::mutex> lock(allocatorGeneralLifecycleMutex);
        auto& state = allocatorGeneralPointers[pointer];
        previous = state;
        lifecycleDuplicate = state.known && !state.allocated;
        state.known = true;
        state.allocated = false;
        state.freeOrdinal = ordinal;
        state.freeThread = CurrentGuestThreadId();
        state.freeCaller = static_cast<uint32_t>(context.lr);
        state.freeStack = context.r1.u32;
        state.freeR3 = context.r3.u32;
        state.freeR28 = context.r28.u32;
        state.freeR30 = context.r30.u32;
        state.freeR31 = context.r31.u32;
    }
    const bool headerDuplicate = (blockFlags & 1u) != 0;
    if (!headerDuplicate && !lifecycleDuplicate) return;

    const uint32_t sequence = allocatorGeneralDuplicateSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence >= 64u) return;
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::lock_guard<std::mutex> logLock(allocatorGeneralLifecycleMutex);
    std::ofstream out("logs/m6d_allocator_general_double_free_watch.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "ALLOCATOR_GENERAL_DUPLICATE sequence=" << sequence
        << " lifecycle_ordinal=" << ordinal
        << " thread=" << CurrentGuestThreadId()
        << " caller=0x" << std::hex << context.lr
        << " allocator=0x" << context.r3.u32
        << " pointer=0x" << pointer << " block=0x" << block
        << " block00=0x" << PPC_LOAD_U32(block)
        << " block04=0x" << blockFlags
        << " block08=0x" << PPC_LOAD_U32(block + 8u)
        << " block0c=0x" << PPC_LOAD_U32(block + 12u)
        << " stack=0x" << context.r1.u32
        << std::dec << " header_duplicate=" << headerDuplicate
        << " lifecycle_duplicate=" << lifecycleDuplicate
        << " previous_known=" << previous.known
        << " previous_allocated=" << previous.allocated
        << " allocation_ordinal=" << previous.allocationOrdinal
        << " allocation_thread=" << previous.allocationThread
        << " allocation_caller=0x" << std::hex << previous.allocationCaller
        << " allocation_size=0x" << previous.allocationSize
        << " allocation_alignment=0x" << previous.allocationAlignment
        << " allocation_stack=0x" << previous.allocationStack
        << std::dec << " previous_free_ordinal=" << previous.freeOrdinal
        << " previous_free_thread=" << previous.freeThread
        << " previous_free_caller=0x" << std::hex << previous.freeCaller
        << " previous_free_stack=0x" << previous.freeStack
        << " previous_free_r3=0x" << previous.freeR3
        << " previous_free_r28=0x" << previous.freeR28
        << " previous_free_r30=0x" << previous.freeR30
        << " previous_free_r31=0x" << previous.freeR31
        << " history=";
    AppendFunctionHistory(out);
    out << std::dec << '\n';
}

void AppendAllocatorGeneralAllocationResult(PPCContext& context, uint8_t* base,
                                            uint32_t address) {
    constexpr uint32_t kAllocationFailureAfterUnlock = 0x82215A18u;
    constexpr uint32_t kAllocationSuccessAfterUnlock = 0x82216068u;
    if (address != kAllocationFailureAfterUnlock &&
        address != kAllocationSuccessAfterUnlock) {
        return;
    }

    const uint32_t allocator = context.r24.u32;
    auto request = AllocatorGeneralAllocationRequest{};
    bool foundRequest = false;
    for (size_t i = allocatorGeneralAllocationRequests.size(); i; --i) {
        if (allocatorGeneralAllocationRequests[i - 1u].allocator != allocator) continue;
        request = allocatorGeneralAllocationRequests[i - 1u];
        allocatorGeneralAllocationRequests.erase(
            allocatorGeneralAllocationRequests.begin() + (i - 1u));
        foundRequest = true;
        break;
    }
    if (address == kAllocationFailureAfterUnlock) return;

    const uint32_t pointer = context.r30.u32;
    if (!pointer) return;
    const uint64_t ordinal = allocatorGeneralLifecycleOrdinal.fetch_add(
                                 1, std::memory_order_relaxed) + 1u;
    AllocatorGeneralPointerState previous{};
    bool reallocation = false;
    {
        std::lock_guard<std::mutex> lock(allocatorGeneralLifecycleMutex);
        auto& state = allocatorGeneralPointers[pointer];
        previous = state;
        reallocation = state.known && state.allocated;
        state.known = true;
        state.allocated = true;
        state.allocationOrdinal = ordinal;
        state.allocationThread = CurrentGuestThreadId();
        state.allocationCaller = request.caller;
        state.allocationSize = request.size;
        state.allocationAlignment = request.alignment;
        state.allocationStack = request.stack;
    }
    if (!reallocation) return;

    const uint32_t sequence = allocatorGeneralReallocationSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence >= 64u) return;
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::lock_guard<std::mutex> logLock(allocatorGeneralLifecycleMutex);
    std::ofstream out("logs/m6d_allocator_general_reallocation_watch.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "ALLOCATOR_GENERAL_REALLOCATION sequence=" << sequence
        << " lifecycle_ordinal=" << ordinal
        << " thread=" << CurrentGuestThreadId()
        << " allocator=0x" << std::hex << allocator
        << " pointer=0x" << pointer
        << " caller=0x" << request.caller
        << " size=0x" << request.size
        << " alignment=0x" << request.alignment
        << " stack=0x" << request.stack
        << std::dec << " request_found=" << foundRequest
        << " previous_allocation_ordinal=" << previous.allocationOrdinal
        << " previous_allocation_thread=" << previous.allocationThread
        << " previous_allocation_caller=0x" << std::hex
        << previous.allocationCaller << " history=";
    AppendFunctionHistory(out);
    out << std::dec << '\n';
}

void AppendAllocatorFixedPoolLifecycle(PPCContext& context, uint8_t* base,
                                       uint32_t address) {
    constexpr uint32_t kGeneralFree = 0x82216270u;
    constexpr uint32_t kFixedPoolFree = 0x82216B98u;
    constexpr uint32_t kFixedPoolAllocate = 0x82216D40u;
    constexpr uint32_t kDescriptor = 0xBF0000A4u;
    constexpr uint32_t kFirstPage = 0xBF000580u;
    constexpr uint32_t kFirstRaw = 0xBF000590u;
    constexpr uint32_t kRawStride = 0x24u;
    constexpr uint32_t kFirstPageLimit = 0xBF00E000u;

    if (address == kGeneralFree) {
        auto& record = allocatorGeneralFreeHistory[
            allocatorGeneralFreeHistoryWrite++ % allocatorGeneralFreeHistory.size()];
        record.ordinal = allocatorGeneralFreeOrdinal.fetch_add(
                             1, std::memory_order_relaxed) + 1u;
        record.stack = context.r1.u32;
        record.pointer = context.r4.u32;
        record.caller = static_cast<uint32_t>(context.lr);
        return;
    }

    if (address == kFixedPoolAllocate && context.r3.u32 == kDescriptor) {
        // The exact raw node is not known until the generated routine pops its
        // selected page. Preserve the caller here; the raw[0] = page store is
        // observed below and records the actual allocation transition.
        allocatorFixedPoolAllocationCaller = static_cast<uint32_t>(context.lr);
        return;
    }

    if (address != kFixedPoolFree || context.r3.u32 != kDescriptor ||
        context.r4.u32 < 4u) {
        return;
    }
    const uint32_t raw = context.r4.u32 - 4u;
    if (raw < kFirstRaw || raw >= kFirstPageLimit ||
        ((raw - kFirstRaw) % kRawStride) != 0u) {
        return;
    }

    AllocatorGeneralFreeContext currentGeneralFree{};
    for (uint32_t i = 0; i < allocatorGeneralFreeHistory.size(); ++i) {
        const uint32_t index = (allocatorGeneralFreeHistoryWrite - 1u - i) %
                               allocatorGeneralFreeHistory.size();
        const auto& candidate = allocatorGeneralFreeHistory[index];
        if (!candidate.ordinal || candidate.stack < context.r1.u32 ||
            candidate.stack - context.r1.u32 >= 0x4000u) {
            continue;
        }
        currentGeneralFree = candidate;
        break;
    }

    const uint64_t ordinal = allocatorFixedPoolLifecycleOrdinal.fetch_add(
                                 1, std::memory_order_relaxed) + 1u;
    const uint32_t rawWord = PPC_LOAD_U32(raw);
    AllocatorFixedPoolNodeState previous{};
    bool duplicate = false;
    {
        std::lock_guard<std::mutex> lock(allocatorFixedPoolLifecycleMutex);
        auto& state = allocatorFixedPoolNodes[raw];
        previous = state;
        duplicate = rawWord != kFirstPage;
        state.known = true;
        state.allocated = false;
        state.freeOrdinal = ordinal;
        state.freeThread = CurrentGuestThreadId();
        state.freeCaller = static_cast<uint32_t>(context.lr);
        state.freeGeneralPointer = currentGeneralFree.pointer;
        state.freeGeneralCaller = currentGeneralFree.caller;
        state.freeStack = context.r1.u32;
    }
    if (!duplicate) return;

    std::lock_guard<std::mutex> logLock(allocatorFixedPoolLifecycleMutex);
    const uint32_t sequence = allocatorFixedPoolDuplicateSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence >= 64u) return;
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::ofstream out("logs/m6d_allocator_fixed_pool_lifecycle.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "ALLOCATOR_FIXED_POOL_DUPLICATE sequence=" << sequence
        << " lifecycle_ordinal=" << ordinal << " raw=0x" << std::hex << raw
        << " raw_word=0x" << rawWord
        << " current_thread=" << std::dec << CurrentGuestThreadId()
        << " current_free_caller=0x" << std::hex << context.lr
        << " current_general_pointer=0x" << currentGeneralFree.pointer
        << " current_general_caller=0x" << currentGeneralFree.caller
        << " current_general_ordinal=" << std::dec << currentGeneralFree.ordinal
        << " current_stack=0x" << std::hex << context.r1.u32
        << " previous_free_ordinal=" << std::dec << previous.freeOrdinal
        << " previous_thread=" << previous.freeThread
        << " previous_free_caller=0x" << std::hex << previous.freeCaller
        << " previous_general_pointer=0x" << previous.freeGeneralPointer
        << " previous_general_caller=0x" << previous.freeGeneralCaller
        << " previous_stack=0x" << previous.freeStack
        << " last_allocation_ordinal=" << std::dec << previous.allocationOrdinal
        << " last_allocation_thread=" << previous.allocationThread
        << " last_allocation_caller=0x" << std::hex << previous.allocationCaller
        << " history=";
    const auto trace = CurrentTrace();
    const uint32_t write = trace->functionWrite.load(std::memory_order_relaxed);
    const uint32_t historyCount = std::min<uint32_t>(write, 64u);
    for (uint32_t i = historyCount; i; --i) {
        const uint32_t slot = (write - i) % kFunctionHistory;
        out << (i == historyCount ? "0x" : ",0x") << std::hex
            << trace->functions[slot].load(std::memory_order_relaxed);
    }
    out << std::dec << '\n';
}

void AppendAllocatorFixedPoolFreeProbe(PPCContext& context, uint8_t* base,
                                       uint32_t address) {
    constexpr uint32_t kFixedPoolFree = 0x82216B98u;
    constexpr uint32_t kDescriptor = 0xBF0000A4u;
    constexpr uint32_t kFirstPage = 0xBF000580u;
    constexpr uint32_t kFirstRaw = 0xBF000590u;
    constexpr uint32_t kRawStride = 0x24u;
    constexpr uint32_t kFirstPageLimit = 0xBF00E000u;
    constexpr uint32_t kTraversalLimit = 2048u;
    if (address != kFixedPoolFree || context.r3.u32 != kDescriptor ||
        context.r4.u32 < 4u) {
        return;
    }

    const uint32_t raw = context.r4.u32 - 4u;
    const bool rawInFirstPage =
        raw >= kFirstRaw && raw < kFirstPageLimit &&
        ((raw - kFirstRaw) % kRawStride) == 0u;
    if (!rawInFirstPage) return;

    const uint32_t rawWord = PPC_LOAD_U32(raw);
    uint32_t cursor = PPC_LOAD_U32(kFirstPage + 8u);
    uint32_t depth = 0;
    bool foundInFreeList = false;
    bool malformedList = false;
    while (cursor && depth < kTraversalLimit) {
        if (cursor == raw) {
            foundInFreeList = true;
            break;
        }
        if (cursor < kFirstRaw || cursor >= kFirstPageLimit ||
            ((cursor - kFirstRaw) % kRawStride) != 0u) {
            malformedList = true;
            break;
        }
        cursor = PPC_LOAD_U32(cursor);
        ++depth;
    }
    if (depth == kTraversalLimit) malformedList = true;

    // An allocated first-page node always carries its page pointer in raw[0].
    // A different value, or presence in the page's free list, proves that the
    // title is attempting to return a node that is no longer allocated.
    const bool suspicious = rawWord != kFirstPage || foundInFreeList;
    if (!suspicious) return;

    const uint32_t sequence =
        allocatorFixedPoolFreeSequence.fetch_add(1, std::memory_order_relaxed);
    if (sequence >= 128u) return;

    std::lock_guard<std::mutex> lock(allocatorFixedPoolFreeMutex);
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::ofstream out("logs/m6d_allocator_double_free_watch.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "ALLOCATOR_FIXED_POOL_FREE sequence=" << sequence
        << " thread=" << CurrentGuestThreadId() << " function=0x" << std::hex
        << address << " caller_lr=0x" << context.lr << " descriptor=0x"
        << context.r3.u32 << " payload=0x" << context.r4.u32 << " raw=0x"
        << raw << " raw_word=0x" << rawWord << " page=0x" << kFirstPage
        << " page_head=0x" << PPC_LOAD_U32(kFirstPage + 8u)
        << " page_count=0x" << PPC_LOAD_U32(kFirstPage + 12u)
        << " traversal_cursor=0x" << cursor << std::dec
        << " traversal_depth=" << depth
        << " found_in_free_list=" << foundInFreeList
        << " malformed_list=" << malformedList << " history=";

    const auto trace = CurrentTrace();
    const uint32_t write = trace->functionWrite.load(std::memory_order_relaxed);
    const uint32_t historyCount = std::min<uint32_t>(write, 64u);
    for (uint32_t i = historyCount; i; --i) {
        const uint32_t slot = (write - i) % kFunctionHistory;
        out << (i == historyCount ? "0x" : ",0x") << std::hex
            << trace->functions[slot].load(std::memory_order_relaxed);
    }
    out << std::dec << '\n';
}

const char* ThreadStateName(GuestThreadState state) {
    switch (state) {
    case GuestThreadState::Running: return "RUNNING";
    case GuestThreadState::Suspended: return "SUSPENDED";
    case GuestThreadState::Blocked: return "BLOCKED";
    case GuestThreadState::Waiting: return "WAITING";
    case GuestThreadState::Terminated: return "TERMINATED";
    }
    return "UNKNOWN";
}
}

void RuntimeObserveItemSmartPointerStore(uint8_t* base, uint32_t address,
                                         uint32_t value, void* caller) {
    // Initial zeroing and destructor clearing are already classified by the
    // function trace and vastly outnumber ownership-bearing assignments. Keep
    // this watch focused on writes that install an actual shared reference.
    constexpr uint32_t kItemVtable = 0x82095798u;
    constexpr uint32_t kTargetContainer = 0xAE1C49F0u;
    const bool parentLink = value == kTargetContainer &&
        __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(
            base + address - 8u)) == kItemVtable;
    const uint32_t item = address - (parentLink ? 8u : 12u);
    const uint32_t parent = __builtin_bswap32(
        *reinterpret_cast<const volatile uint32_t*>(base + item + 8u));
    const uint32_t oldValue = __builtin_bswap32(
        *reinterpret_cast<const volatile uint32_t*>(base + address));

    AllocatorItemPointerAssignment assignment{};
    bool assignmentFound = false;
    if (!parentLink) {
        std::lock_guard<std::mutex> lock(allocatorItemPointerAssignmentMutex);
        if (oldValue) {
            const auto target = allocatorTargetReferenceItems.find(oldValue);
            if (target != allocatorTargetReferenceItems.end() &&
                target->second == item) {
                allocatorTargetReferenceItems.erase(target);
                allocatorTargetReferenceControls.erase(oldValue);
            }
        }
        if (!value) {
            allocatorItemPointerAssignments.erase(item);
            return;
        }
        assignment.thread = CurrentGuestThreadId();
        assignment.function = RuntimeActiveGuestFunction();
        assignment.lr = RuntimeActiveGuestLr();
        assignment.oldValue = oldValue;
        assignment.value = value;
        assignment.hostCaller = caller;
        const auto trace = CurrentTrace();
        const uint32_t write = trace->functionWrite.load(std::memory_order_relaxed);
        assignment.historyCount = std::min<uint32_t>(write, 24u);
        for (uint32_t i = assignment.historyCount; i; --i) {
            const uint32_t slot = (write - i) % kFunctionHistory;
            assignment.history[assignment.historyCount - i] =
                trace->functions[slot].load(std::memory_order_relaxed);
        }
        allocatorItemPointerAssignments[item] = assignment;
    } else {
        std::lock_guard<std::mutex> lock(allocatorItemPointerAssignmentMutex);
        const auto it = allocatorItemPointerAssignments.find(item);
        if (it != allocatorItemPointerAssignments.end()) {
            assignment = it->second;
            assignmentFound = true;
        }
    }
    // Two independent failing captures identify this exact title-owned
    // 38-element container as the source of the dangling references. This is
    // a diagnostic address filter only; it does not alter guest state.
    if (!parentLink && parent != kTargetContainer) return;
    const uint32_t sequence = allocatorItemPointerStoreSequence.fetch_add(
        1, std::memory_order_relaxed);
    if (sequence >= 1024u) return;

    const uint32_t reference = parentLink
        ? __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(
              base + item + 12u))
        : value;
    const uint32_t referenceHeader =
        reference >= 0xA0000000u && reference < 0xC0000000u
        ? __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(
              base + reference))
        : 0u;
    uint32_t referenceControl = 0;
    uint32_t referenceControlMatches = 0;
    AllocatorGeneralPointerState referenceControlAllocation{};
    if (parentLink && reference) {
        // The parse-result vector owns each source through a separate control
        // block. At the moment the source is linked into the long-lived item
        // tree, find that still-live control block from tracked allocations.
        // This is a one-time, diagnostic-only reverse lookup over allocations
        // no larger than a control block; it does not modify guest state.
        std::lock_guard<std::mutex> lock(allocatorGeneralLifecycleMutex);
        for (const auto& [pointer, state] : allocatorGeneralPointers) {
            if (!state.known || !state.allocated || state.allocationSize < 28u ||
                state.allocationSize > 64u || pointer < 0xA0000000u ||
                pointer >= 0xC0000000u || PPC_LOAD_U32(pointer + 24u) != reference) {
                continue;
            }
            ++referenceControlMatches;
            if (!referenceControl) {
                referenceControl = pointer;
                referenceControlAllocation = state;
            }
        }
    }
    if (reference) {
        std::lock_guard<std::mutex> lock(allocatorItemPointerAssignmentMutex);
        allocatorTargetReferenceItems[reference] = item;
        if (referenceControl) {
            allocatorTargetReferenceControls[reference] = referenceControl;
        }
    }
    if (parentLink && referenceControl) {
        DumpAllocatorSharedControlHistory(referenceControl, reference, item);
    }
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::ofstream out("logs/m6d_allocator_item_pointer_store_watch.log",
                      sequence ? std::ios::app : std::ios::trunc);
    out << "ALLOCATOR_ITEM_POINTER_STORE sequence=" << sequence
        << " kind=" << (parentLink ? "parent-link" : "pointer")
        << " thread=" << CurrentGuestThreadId()
        << " function=0x" << std::hex << RuntimeActiveGuestFunction()
        << " lr=0x" << RuntimeActiveGuestLr()
        << " item=0x" << item
        << " item04=0x" << __builtin_bswap32(
               *reinterpret_cast<const volatile uint32_t*>(base + item + 4u))
        << " item08=0x" << __builtin_bswap32(
               *reinterpret_cast<const volatile uint32_t*>(base + item + 8u))
        << " address=0x" << address
        << " old=0x" << oldValue
        << " value=0x" << value
        << " reference=0x" << reference
        << " reference00=0x" << referenceHeader
        << " reference_control=0x" << referenceControl;
    if (referenceControl) {
        out << " reference_control00=0x" << PPC_LOAD_U32(referenceControl)
            << " reference_control08=0x" << PPC_LOAD_U32(referenceControl + 8u)
            << " reference_control10=0x" << PPC_LOAD_U32(referenceControl + 16u)
            << " reference_control14=0x" << PPC_LOAD_U32(referenceControl + 20u)
            << " reference_control18=0x" << PPC_LOAD_U32(referenceControl + 24u)
            << std::dec
            << " reference_control_allocation_size="
            << referenceControlAllocation.allocationSize
            << " reference_control_allocation_ordinal="
            << referenceControlAllocation.allocationOrdinal
            << " reference_control_allocation_thread="
            << referenceControlAllocation.allocationThread
            << " reference_control_allocation_caller=0x" << std::hex
            << referenceControlAllocation.allocationCaller;
    }
    out << std::dec << " reference_control_matches=" << referenceControlMatches
        << " assignment_found=" << assignmentFound;
    if (parentLink && assignmentFound) {
        out << " assignment_thread=" << assignment.thread
            << " assignment_function=0x" << std::hex << assignment.function
            << " assignment_lr=0x" << assignment.lr
            << " assignment_old=0x" << assignment.oldValue
            << " assignment_value=0x" << assignment.value
            << " assignment_host_caller=" << assignment.hostCaller
            << " assignment_history=";
        for (uint32_t i = 0; i < assignment.historyCount; ++i) {
            out << (i ? ",0x" : "0x") << assignment.history[i];
        }
    }
    out << std::hex
        << " host_caller=" << caller << " history=";
    AppendFunctionHistory(out, 24u);
    out << std::dec << '\n';
}

void RuntimeObserveAllocatorHeaderStore(uint8_t* base, uint32_t address, uint32_t width,
                                        uint64_t value, void* caller) {
    if (width == 4u && value == 0xBF000580u &&
        address >= 0xBF000590u && address < 0xBF00E000u &&
        ((address - 0xBF000590u) % 0x24u) == 0u) {
        const uint64_t ordinal = allocatorFixedPoolLifecycleOrdinal.fetch_add(
                                     1, std::memory_order_relaxed) + 1u;
        std::lock_guard<std::mutex> lock(allocatorFixedPoolLifecycleMutex);
        auto& state = allocatorFixedPoolNodes[address];
        state.known = true;
        state.allocated = true;
        state.allocationOrdinal = ordinal;
        state.allocationThread = CurrentGuestThreadId();
        state.allocationCaller = allocatorFixedPoolAllocationCaller;
        return;
    }
    const uint32_t sequence =
        allocatorHeaderStoreSequence.fetch_add(1, std::memory_order_relaxed);
    const bool alignedWord = width == 4u && (address & 3u) == 0;
    bool suspicious = !alignedWord;
    if (alignedWord) {
        switch (address) {
        case 0xBF000080u:
            suspicious = value != 0u && value != 0x82065B24u &&
                         value != 0x8206573Cu;
            break;
        case 0xBF000084u:
            // This field is initialized with an ordinary store and otherwise
            // changed only by the title's lwarx/stwcx critical-section code.
            suspicious = value != 0u;
            break;
        case 0xBF000088u:
            suspicious = value > 2u;
            break;
        case 0xBF00008Cu:
            suspicious = value > 64u;
            break;
        case 0xBF000090u:
            suspicious = value > 64u;
            break;
        case 0xBF000094u:
            suspicious = value > 0x10000u;
            break;
        case 0xBF000588u:
            suspicious = value != 0u &&
                         !(value >= 0xBF000590u && value < 0xBF00E000u &&
                           ((value - 0xBF000590u) % 0x24u) == 0u);
            break;
        default:
            suspicious = true;
            break;
        }
    }
    if (!suspicious && sequence >= 128u && (sequence & 0xFFFu) != 0) return;
    const auto loadWord = [base](uint32_t guestAddress) {
        return __builtin_bswap32(
            *reinterpret_cast<const volatile uint32_t*>(base + guestAddress));
    };
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::ofstream out("logs/m6d_allocator_header_store_watch.log",
                      sequence ? std::ios::app : std::ios::trunc);
    const PPCContext* activeContext = RuntimeActivePpcContext();
    out << "ALLOCATOR_HEADER_STORE sequence=" << sequence
        << " thread=" << CurrentGuestThreadId()
        << " function=0x" << std::hex << RuntimeActiveGuestFunction()
        << " lr=0x" << RuntimeActiveGuestLr()
        << " address=0x" << address << " width=0x" << width
        << " value=0x" << value << " host_caller=" << caller
        << " before00=0x" << loadWord(0xBF000080u)
        << " before04=0x" << loadWord(0xBF000084u)
        << " before08=0x" << loadWord(0xBF000088u)
        << " before0c=0x" << loadWord(0xBF00008Cu)
        << " before10=0x" << loadWord(0xBF000090u)
        << " before14=0x" << loadWord(0xBF000094u)
        << " pool00=0x" << loadWord(0xBF000580u)
        << " pool04=0x" << loadWord(0xBF000584u)
        << " pool08=0x" << loadWord(0xBF000588u)
        << " pool0c=0x" << loadWord(0xBF00058Cu)
        << std::dec << " suspicious=" << suspicious;
    if (activeContext) {
        out << " r3=0x" << std::hex << activeContext->r3.u32
            << " r4=0x" << activeContext->r4.u32
            << " r8=0x" << activeContext->r8.u32
            << " r9=0x" << activeContext->r9.u32
            << " r10=0x" << activeContext->r10.u32
            << " r11=0x" << activeContext->r11.u32
            << " r29=0x" << activeContext->r29.u32
            << " r30=0x" << activeContext->r30.u32
            << " r31=0x" << activeContext->r31.u32 << std::dec;
    }
    out << '\n';
}

std::atomic<uint32_t> g_runtime_entry_slow_mask{};

void RuntimeSetEntrySlowBit(uint32_t bit, bool enabled) noexcept {
    if (enabled) {
        g_runtime_entry_slow_mask.fetch_or(bit, std::memory_order_release);
    } else {
        g_runtime_entry_slow_mask.fetch_and(~bit, std::memory_order_release);
    }
}

void ConfigureRuntimeFrameCadenceDiagnostics(bool enabled) noexcept {
    frameCadenceAttributionEnabled.store(enabled, std::memory_order_relaxed);
    RuntimeSetEntrySlowBit(kRuntimeEntrySlowCadence, enabled);
}

namespace {
// One byte per possible function start (4-byte aligned) in the title's code.
constexpr uint32_t kCoverageBase = 0x82000000u;
constexpr uint32_t kCoverageEnd = 0x83000000u;
std::atomic<uint8_t>* coverageMap = nullptr;
std::wstring coveragePath;
}

void ConfigureRuntimeFunctionCoverage(const wchar_t* path) noexcept {
    if (!path || !*path || coverageMap) return;
    try {
        coverageMap = new std::atomic<uint8_t>[(kCoverageEnd - kCoverageBase) / 4]();
        coveragePath = path;
    } catch (...) {
        coverageMap = nullptr;
        return;
    }
    RuntimeSetEntrySlowBit(kRuntimeEntrySlowCoverage, true);
}

void WriteRuntimeFunctionCoverage() noexcept {
    if (!coverageMap) return;
    try {
        std::ofstream out(std::filesystem::path(coveragePath), std::ios::trunc);
        size_t functions = 0;
        char line[16];
        for (uint32_t index = 0; index < (kCoverageEnd - kCoverageBase) / 4; ++index) {
            if (coverageMap[index].load(std::memory_order_relaxed)) {
                std::snprintf(line, sizeof(line), "%08X\n", kCoverageBase + index * 4);
                out << line;
                ++functions;
            }
        }
        std::cerr << "RUNTIME_FUNCTION_COVERAGE functions=" << functions << '\n';
    } catch (...) {
    }
}

namespace {
std::atomic<bool> oneVblankExperimentEnabled{};
std::atomic<bool> immediateDeadlineExperimentEnabled{};
std::atomic<uint32_t> oneVblankExperimentReports{};
}

void ConfigureRuntimeOneVblankExperiment(bool enabled, bool immediate) noexcept {
    oneVblankExperimentEnabled.store(enabled, std::memory_order_relaxed);
    immediateDeadlineExperimentEnabled.store(immediate, std::memory_order_relaxed);
}

void ConfigureRuntimeHitchDiagnostics(bool enabled) noexcept {
    runtimeHitchDiagnosticsEnabled.store(enabled, std::memory_order_relaxed);
}

bool RuntimeHitchDiagnosticsEnabled() noexcept {
    return runtimeHitchDiagnosticsEnabled.load(std::memory_order_relaxed);
}

void RuntimeFunctionEnter(PPCContext& context, uint8_t* base, uint32_t address) {
    if (GuestRuntimeStopRequested()) throw GuestRuntimeStop{};
    if (coverageMap && address >= kCoverageBase && address < kCoverageEnd) {
        coverageMap[(address - kCoverageBase) >> 2].store(1, std::memory_order_relaxed);
    }
    if (address == kRuntimeFrameStart) {
        // The title's frame driver: the frame starts before its frame-pool
        // wait and input processing.
        RuntimeGraphicsGuestFrameStart();
    }
    if (address == kRuntimeInputSample) {
        // The title reads its input for the frame.
        RuntimeGraphicsGuestInputSample();
    }
    if (address == kRuntimeFrameLimiterBoundary) {
        // The title's per-frame scene builder on its producer thread: the
        // guest frame boundary where production is paced (frame limiter).
        RuntimeGraphicsGuestFrameBoundary();
    }
    RuntimeCameraFunctionEnter(context, base, address);
    if (address == 0x8286FD68u &&
        (oneVblankExperimentEnabled.load(std::memory_order_relaxed) ||
         immediateDeadlineExperimentEnabled.load(std::memory_order_relaxed)) &&
        darkness::experiments::IsTitlePresentationCreation(address,
            static_cast<uint32_t>(context.lr), context.r4.u32, context.r1.u32)) {
        const uint32_t slot = context.r4.u32 + 0x34u;
        const uint32_t original = PPC_LOAD_U32(slot);
        const bool immediate = immediateDeadlineExperimentEnabled.load(std::memory_order_relaxed);
        const uint32_t selected = immediate
            ? darkness::experiments::ExperimentalImmediateInterval(true, original)
            : darkness::experiments::ExperimentalPresentInterval(true, original);
        if (selected != original) PPC_STORE_U32(slot, selected);
        if (oneVblankExperimentReports.fetch_add(1, std::memory_order_relaxed) < 8u) {
            std::cout << (immediate ? "EXPERIMENT_IMMEDIATE_DEADLINE" : "EXPERIMENT_ONE_VBLANK")
                      << " title_creation=1 original="
                      << original << " selected=" << selected
                      << " clocks_changed=0 simulation_changed=0\n";
        }
    }
    if (frameCadenceAttributionEnabled.load(std::memory_order_relaxed)) {
        AppendFrameCadenceFunctionEntry(context, base, address);
    }
    if constexpr (kCompletedPromptNanTraceEnabled) {
        if (address < 0x828999D0u || address > 0x82899A7Cu) {
            activeNonHelperGuestFunction = address;
        }
    }
    RuntimeSetActivePpcContext(&context, base, address);
    if constexpr (kLevelGameplayFrontierProbeEnabled) {
        AppendPendingLevelRegistryMutationAfter(context, base, address);
    }
    if (RuntimeInterruptDeliveryAllowedAtFunction(address) &&
        DeliverRuntimeGraphicsInterrupts(context, base)) {
        // The interrupt callback is guest code and updates the diagnostic TLS;
        // restore the interrupted location after its register state is restored.
        RuntimeSetActivePpcContext(&context, base, address);
    }
    if constexpr (kCompletedPromptNanTraceEnabled) {
    // 0x828714A0 copies selected CPU-side Xenos constant-cache vectors into
    // the transient PM4 stream. Record only invocations whose source already
    // contains the canonical NaNs proven to zero the PRESS START background
    // draw, before the retired broad function tracer returns.
    if (address == 0x828714A0u && context.r5.u32 == 0x4000u && context.r6.u32) {
        RuntimeWatchGpuFloatConstantSource(context.r6.u32);
    }
    if (address == 0x82248A78u) {
        constexpr uint32_t kGpuState = 0x82A69B00u;
        const uint32_t transformPool = PPC_LOAD_U32(kGpuState + 8224u);
        if (transformPool) {
            for (uint32_t index = 0; index < 4; ++index) {
                RuntimeWatchGpuTransformSource(
                    transformPool + index * 656u + 16u);
            }
        }
    }
    // 0x82763D50 is the verified transform-pool copy/setter reached by the
    // prompt draw. Capture the caller-owned 64-byte input before the helper
    // prologue overwrites LR, then watch it for subsequent producer writes.
    if (address == 0x82763D50u && context.r4.u32 &&
        context.r4.u32 <= 0xFFFFFFC0u) {
        uint32_t sourceWords[16]{};
        uint32_t sourceHash = 2166136261u;
        bool hasCanonicalQuietNan = false;
        for (uint32_t index = 0; index < 16; ++index) {
            const uint32_t value = PPC_LOAD_U32(context.r4.u32 + index * 4u);
            sourceWords[index] = value;
            sourceHash ^= value;
            sourceHash *= 16777619u;
            hasCanonicalQuietNan |=
                (value & 0x7FFFFFFFu) == 0x7FC00000u;
        }
        if (hasCanonicalQuietNan) {
            RuntimeWatchGpuTransformInputSource(context.r4.u32);
            const uint32_t sequence = nanTransformInputSequence.fetch_add(
                                          1, std::memory_order_relaxed) + 1;
            if (sequence <= 16u) {
                const auto timestampUs = std::chrono::duration_cast<
                    std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::lock_guard lock(nanTransformInputMutex);
                std::filesystem::create_directories("logs");
                std::ofstream out("logs/m7_prompt_nan_transform_input_probe48.log",
                                  std::ios::app);
                out << "GPU_TRANSFORM_INPUT sequence=" << sequence
                    << " timestamp_us=" << timestampUs
                    << " thread=" << CurrentGuestThreadId()
                    << " caller=0x" << std::hex << context.lr
                    << " state=0x" << context.r3.u32
                    << " source=0x" << context.r4.u32
                    << " auxiliary=0x" << context.r5.u32
                    << " owner=0x" << context.r31.u32
                    << " hash=0x" << sourceHash << " words=";
                for (uint32_t index = 0; index < 16; ++index) {
                    if (index) out << ',';
                    out << "0x" << sourceWords[index];
                }
                out << std::dec << '\n';
            }
        }
    }
    // sub_82354DE0 copies the caller-owned matrix at r4+16 into the rotating
    // render allocation later consumed by 0x82763D50. Observe that source at
    // entry so a following frame identifies the actual upstream store site.
    if (address == 0x82354DE0u && context.r4.u32 &&
        context.r4.u32 <= 0xFFFFFFB0u) {
        const uint32_t source = context.r4.u32 + 16u;
        uint32_t sourceWords[16]{};
        uint32_t sourceHash = 2166136261u;
        bool hasCanonicalQuietNan = false;
        for (uint32_t index = 0; index < 16; ++index) {
            const uint32_t value = PPC_LOAD_U32(source + index * 4u);
            sourceWords[index] = value;
            sourceHash ^= value;
            sourceHash *= 16777619u;
            hasCanonicalQuietNan |=
                (value & 0x7FFFFFFFu) == 0x7FC00000u;
        }
        if (hasCanonicalQuietNan) {
            RuntimeWatchGpuTransformInputSource(source);
            const uint32_t sequence = nanTransformUpstreamSequence.fetch_add(
                                          1, std::memory_order_relaxed) + 1;
            if (sequence <= 16u) {
                const auto timestampUs = std::chrono::duration_cast<
                    std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::lock_guard lock(nanTransformUpstreamMutex);
                std::filesystem::create_directories("logs");
                std::ofstream out("logs/m7_prompt_nan_transform_upstream_probe48.log",
                                  std::ios::app);
                out << "GPU_TRANSFORM_UPSTREAM sequence=" << sequence
                    << " timestamp_us=" << timestampUs
                    << " thread=" << CurrentGuestThreadId()
                    << " caller=0x" << std::hex << context.lr
                    << " destination_context=0x" << context.r3.u32
                    << " object=0x" << context.r4.u32
                    << " source=0x" << source
                    << " option=0x" << context.r5.u32
                    << " hash=0x" << sourceHash << " words=";
                for (uint32_t index = 0; index < 16; ++index) {
                    if (index) out << ',';
                    out << "0x" << sourceWords[index];
                }
                out << std::dec << '\n';
            }
        }
    }
    if (address == 0x828714A0u && context.r6.u32 &&
        context.r6.u32 <= 0xFFFFFF3Fu) {
        constexpr uint32_t kSourceDwords = 48;
        bool hasCanonicalQuietNan = false;
        uint32_t sourceWords[kSourceDwords]{};
        uint32_t sourceHash = 2166136261u;
        for (uint32_t index = 0; index < kSourceDwords; ++index) {
            const uint32_t value = PPC_LOAD_U32(context.r6.u32 + index * 4u);
            sourceWords[index] = value;
            sourceHash ^= value;
            sourceHash *= 16777619u;
            hasCanonicalQuietNan |=
                (value & 0x7FFFFFFFu) == 0x7FC00000u;
        }
        if (hasCanonicalQuietNan) {
            const uint32_t sequence =
                nanConstantCopySourceSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            if (sequence <= 64u) {
                const auto timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::lock_guard lock(nanConstantCopySourceMutex);
                std::filesystem::create_directories("logs");
                std::ofstream out("logs/m7_prompt_nan_constant_source_probe48.log",
                                  std::ios::app);
                out << "GPU_NAN_CONSTANT_COPY_SOURCE sequence=" << sequence
                    << " timestamp_us=" << timestampUs
                    << " thread=" << CurrentGuestThreadId()
                    << " caller=0x" << std::hex << context.lr
                    << " command_builder=0x" << context.r3.u32
                    << " dirty_mask=0x" << context.r4.u64
                    << " register_base=0x" << context.r5.u32
                    << " source=0x" << context.r6.u32
                    << " hash=0x" << sourceHash << " words=";
                for (uint32_t index = 0; index < kSourceDwords; ++index) {
                    if (index) out << ',';
                    out << "0x" << sourceWords[index];
                }
                out << std::dec << '\n';
            }
        }
    }
    }
    if constexpr (kTitleFrameStateProbeEnabled) {
        AppendTitleFrameProbe(context, base, address);
    }
    if constexpr (kLevelGameplayFrontierProbeEnabled) {
        AppendLevelRegistryUpdateProbe(context, base, address);
        AppendLevelGameplayFrontierProbe(context, base, address);
    }
    if constexpr (!kLegacyFunctionTracingEnabled) return;
    AppendRootUpdateListLifecycle(context, base, address);
    if constexpr (kCompletedXdfTraceEnabled) {
        AppendXdfResolutionTrace(context, base, address);
        AppendGameContextWorkerFlagTrace(context, base, address);
        AppendGameContextManagerCleanupTrace(context, base, address);
        AppendGameContextEventTrace(context, base, address);
        AppendGameContextBufferedReadTrace(context, base, address);
        AppendGameContextConsumerOwnerTrace(context, base, address);
        AppendXdfProviderDeepTrace(context, base, address);
    }
    AppendAllocatorFixedPoolLifecycle(context, base, address);
    RecordAllocatorSharedControlEvent(context, base, address);
    AppendAllocatorReferenceProvider(context, base, address);
    AppendAllocatorRegistryProducer(context, base, address);
    AppendAllocatorOwnerConsumerOrder(context, base, address);
    AppendAllocatorTargetReferenceLifecycle(context, base, address);
    AppendAllocatorGeneralLifecycle(context, base, address);
    AppendAllocatorSmartPointerSource(context, base, address);
    AppendAllocatorStaleSmartPointer(context, base, address);
    AppendAllocatorFixedPoolFreeProbe(context, base, address);
    if constexpr (kMainUpdatePathTraceEnabled) if (
        address == 0x820DE198u && context.lr == 0x820DE168u &&
        !mainUpdateTraceClaimed.exchange(true, std::memory_order_relaxed)) {
        mainUpdateTraceActive = true;
        mainUpdateTraceObject = context.r3.u32;
        mainUpdateTraceSequence = 0;
        std::filesystem::create_directories("logs");
        std::ofstream out("logs/m6d_main_update_path.log", std::ios::trunc);
        out << "MAIN_UPDATE_BEGIN thread=" << CurrentGuestThreadId()
            << " object=0x" << std::hex << mainUpdateTraceObject
            << " graphics=0x" << context.r4.u32
            << " state2720=0x" << PPC_LOAD_U32(mainUpdateTraceObject + 2720u)
            << " state2724=0x" << PPC_LOAD_U32(mainUpdateTraceObject + 2724u)
            << " state2728=0x" << PPC_LOAD_U32(mainUpdateTraceObject + 2728u)
            << " state2732=0x" << PPC_LOAD_U32(mainUpdateTraceObject + 2732u)
            << std::dec << '\n';
    }
    if constexpr (kMainUpdatePathTraceEnabled) if (
        mainUpdateTraceActive && mainUpdateTraceSequence < 8192u) {
        std::ofstream out("logs/m6d_main_update_path.log", std::ios::app);
        out << "MAIN_UPDATE_FUNCTION sequence=" << mainUpdateTraceSequence++
            << " thread=" << CurrentGuestThreadId()
            << " function=0x" << std::hex << address << " lr=0x" << context.lr
            << " r3=0x" << context.r3.u32 << " r4=0x" << context.r4.u32
            << " r5=0x" << context.r5.u32 << " r6=0x" << context.r6.u32
            << std::dec << '\n';
        if (mainUpdateTraceSequence == 8192u) {
            out << "MAIN_UPDATE_TRACE_LIMIT object=0x" << std::hex
                << mainUpdateTraceObject << " state2732=0x"
                << PPC_LOAD_U32(mainUpdateTraceObject + 2732u) << std::dec << '\n';
        }
    }
    if (address == 0x82215110u) {
        const uint32_t sequence =
            allocatorLifecycleProbeSequence.fetch_add(1, std::memory_order_relaxed);
        if (sequence < 16u) {
            const uint32_t allocator = context.r3.u32;
            std::filesystem::create_directories("logs");
            std::ofstream out("logs/m6d_allocator_lifecycle_probe.log",
                              sequence ? std::ios::app : std::ios::trunc);
            out << "ALLOCATOR_CONSTRUCTOR_ENTER sequence=" << sequence
                << " thread=" << CurrentGuestThreadId() << " allocator=0x" << std::hex
                << allocator << " caller=0x" << context.lr
                << " word00=0x" << PPC_LOAD_U32(allocator)
                << " word04=0x" << PPC_LOAD_U32(allocator + 4u)
                << " word08=0x" << PPC_LOAD_U32(allocator + 8u)
                << " word0c=0x" << PPC_LOAD_U32(allocator + 12u)
                << " word10=0x" << PPC_LOAD_U32(allocator + 16u)
                << " word14=0x" << PPC_LOAD_U32(allocator + 20u) << std::dec << '\n';
        }
    }
    if (address == 0x82216270u) {
        const uint32_t allocator = context.r3.u32;
        const uint32_t pointer = context.r4.u32;
        const uint32_t block = pointer - 16u;
        const uint32_t lockCount = PPC_LOAD_U32(allocator + 4u);
        const bool pointsIntoAllocator =
            pointer >= allocator && uint64_t(pointer) < uint64_t(allocator) + 232u;
        const bool blockOverlapsAllocator =
            uint64_t(block) < uint64_t(allocator) + 232u &&
            uint64_t(block) + 32u > uint64_t(allocator);
        const bool invalidLockHeader = lockCount > 0x10000u;
        if (pointsIntoAllocator || blockOverlapsAllocator || invalidLockHeader) {
            const uint32_t sequence =
                allocatorSuspiciousFreeSequence.fetch_add(1, std::memory_order_relaxed);
            if (sequence < 1024u) {
                std::filesystem::create_directories("logs");
                std::ofstream out("logs/m6d_allocator_suspicious_free_probe.log",
                                  sequence ? std::ios::app : std::ios::trunc);
                out << "ALLOCATOR_FREE_ANOMALY sequence=" << sequence
                    << " thread=" << CurrentGuestThreadId() << " caller=0x" << std::hex
                    << context.lr << " allocator=0x" << allocator << " pointer=0x" << pointer
                    << " block=0x" << block << " lock_count=0x" << lockCount
                    << " waiter=0x" << PPC_LOAD_U32(allocator + 8u)
                    << " owner=0x" << PPC_LOAD_U32(allocator + 12u)
                    << " recursion=0x" << PPC_LOAD_U32(allocator + 16u)
                    << " event=0x" << PPC_LOAD_U32(allocator + 20u)
                    << " block00=0x" << PPC_LOAD_U32(block)
                    << " block04=0x" << PPC_LOAD_U32(block + 4u)
                    << " block08=0x" << PPC_LOAD_U32(block + 8u)
                    << std::dec << " points_into_allocator=" << pointsIntoAllocator
                    << " block_overlaps_allocator=" << blockOverlapsAllocator
                    << " invalid_lock_header=" << invalidLockHeader << '\n';
            }
        }
    }
    if (address == 0x8285DEB8u && RuntimeGraphicsIsActive()) {
        const uint64_t sequence =
            graphicsPollHelperSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if (sequence <= 16 || (sequence & (sequence - 1)) == 0) {
            const uint32_t descriptor = context.r3.u32;
            const uint32_t owner = descriptor ? PPC_LOAD_U32(descriptor) : 0;
            std::cout << "GPU_TITLE_POLL sequence=" << sequence
                      << " thread=" << CurrentGuestThreadId()
                      << " caller=0x" << std::hex << context.lr
                      << " descriptor=0x" << descriptor
                      << " owner=0x" << owner;
            if (descriptor) {
                std::cout << " kind=0x" << PPC_LOAD_U32(descriptor + 4)
                          << " observed=0x" << PPC_LOAD_U32(descriptor + 8)
                          << " tick=0x" << PPC_LOAD_U32(descriptor + 12);
            }
            if (owner) {
                std::cout << " submitted=0x" << PPC_LOAD_U32(owner + 0x4098u)
                          << " completed=0x" << PPC_LOAD_U32(owner + 0x40A0u)
                          << " pending_read=0x" << PPC_LOAD_U32(owner + 0x4124u)
                          << " pending_write=0x" << PPC_LOAD_U32(owner + 0x4128u);
            }
            std::cout << std::dec << '\n';
        }
    }
    auto trace = CurrentTrace();
    const uint32_t slot = trace->functionWrite.fetch_add(1, std::memory_order_relaxed) % kFunctionHistory;
    trace->functions[slot].store(address, std::memory_order_relaxed);
    trace->lastFunction.store(address, std::memory_order_relaxed);
    trace->lastLr.store(context.lr, std::memory_order_relaxed);
    trace->lastR1.store(context.r1.u32, std::memory_order_relaxed);
    trace->lastR13.store(context.r13.u32, std::memory_order_relaxed);
    trace->functionCount.fetch_add(1, std::memory_order_relaxed);

    if (address == 0x828B5590u && CurrentGuestThreadId() == 1) {
        audioInitializerPathActive.store(true, std::memory_order_relaxed);
    }
    if (audioInitializerPathActive.load(std::memory_order_relaxed) &&
        CurrentGuestThreadId() == 1) {
        const uint32_t sequence =
            audioInitializerPathSequence.fetch_add(1, std::memory_order_relaxed);
        if (sequence < 512) {
            std::cout << "AUDIO_INIT_PATH sequence=" << sequence << " function=0x" << std::hex
                      << address << " lr=0x" << context.lr << " r3=0x" << context.r3.u32
                      << " r4=0x" << context.r4.u32 << " r5=0x" << context.r5.u32
                      << " r6=0x" << context.r6.u32 << std::dec << '\n';
        }
        if (address == 0x828B4D38u) {
            audioInitializerPathActive.store(false, std::memory_order_relaxed);
        }
    }

    // The title's audio singleton is installed at 0x82A49B34 by 0x828B5490,
    // populated by 0x828B5590, and cleared only by 0x828B4D38.  Keep this
    // probe bounded and observational: a later interface call reached the
    // singleton with a null +0x3C member, so the lifecycle must be established
    // before deciding whether the producer or the consumer is incorrect.
    if (address == 0x828B0198u || address == 0x828B1F08u ||
        address == 0x828B4D38u || address == 0x828B5490u ||
        address == 0x828B5560u ||
        address == 0x828B5590u ||
        address == 0x828B5930u || address == 0x828B6078u ||
        address == 0x828B5A30u || address == 0x828B5A90u ||
        address == 0x828BACC8u ||
        address == 0x828C1028u ||
        address == 0x828AFF18u) {
        constexpr uint32_t kSingletonAddress = 0x82A49B34u;
        const uint32_t singleton = PPC_LOAD_U32(kSingletonAddress);
        const uint32_t interfacePointer = singleton ? PPC_LOAD_U32(singleton + 0x3Cu) : 0;
        const uint32_t interfaceObject = interfacePointer ? interfacePointer - 8u : 0;
        if (address == 0x828B5490u && context.r4.u32) {
            audioSingletonAllocator.store(context.r4.u32, std::memory_order_relaxed);
        }
        const uint32_t allocator = singleton ? PPC_LOAD_U32(singleton + 8u)
                                             : audioSingletonAllocator.load(
                                                   std::memory_order_relaxed);
        const bool relevantRelease =
            (address != 0x828AFF18u && address != 0x828B5A90u &&
             address != 0x828B1F08u && address != 0x828BACC8u) ||
            context.r3.u32 == singleton || context.r3.u32 == interfaceObject ||
            context.r3.u32 == allocator;
        if (relevantRelease) {
            const uint32_t sequence =
                audioSingletonProbeSequence.fetch_add(1, std::memory_order_relaxed);
            if (sequence < 256) {
                std::cout << "AUDIO_SINGLETON_TRACE sequence=" << sequence
                      << " thread=" << CurrentGuestThreadId()
                      << " function=0x" << std::hex << address
                      << " lr=0x" << context.lr
                      << " global=0x" << singleton
                      << " interface=0x" << interfacePointer
                      << " allocator=0x" << allocator
                      << " r1=0x" << context.r1.u32
                      << " r3=0x" << context.r3.u32
                      << " r4=0x" << context.r4.u32
                      << " r5=0x" << context.r5.u32
                      << " r6=0x" << context.r6.u32;
                if (context.r3.u32) {
                    std::cout << " object_vtable=0x" << PPC_LOAD_U32(context.r3.u32)
                              << " object_refcount=0x" << PPC_LOAD_U32(context.r3.u32 + 4u);
                }
                if (interfaceObject) {
                    std::cout << " interface_object=0x" << interfaceObject
                              << " interface_vtable=0x" << PPC_LOAD_U32(interfaceObject)
                              << " interface_refcount=0x" << PPC_LOAD_U32(interfaceObject + 4u);
                }
                if (allocator) {
                    std::cout << " allocator_vtable=0x" << PPC_LOAD_U32(allocator)
                              << " allocator_interface_vtable=0x"
                              << PPC_LOAD_U32(allocator + 4u)
                              << " allocator_refcount=0x"
                              << PPC_LOAD_U32(allocator + 8u);
                } else if (address == 0x828B5490u && context.r4.u32) {
                    std::cout << " constructor_allocator_vtable=0x"
                              << PPC_LOAD_U32(context.r4.u32)
                              << " constructor_allocator_refcount=0x"
                              << PPC_LOAD_U32(context.r4.u32 + 4u);
                }
                std::cout << std::dec << '\n';
            }
        }
    }

    if (address == 0x8259DDD0u) {
        BeginDeepTrace(context);
        const uint32_t object = context.r3.u32;
        const uint32_t wrapper = object + 0x540u;
        const uint32_t sequence = tlsTransitionCount.fetch_add(1, std::memory_order_relaxed);
        if (sequence < 128) {
            std::filesystem::create_directories("logs");
            std::ofstream out("logs/m5_tls_state_transitions.log", std::ios::app);
            out << "TLS_SET_ENTER sequence=" << sequence << " thread=" << CurrentGuestThreadId()
                << " function=0x8259ddd0 object=0x" << std::hex << object
                << " wrapper=0x" << wrapper << " wrapper_vtable=0x" << PPC_LOAD_U32(wrapper)
                << " tls_index=0x" << PPC_LOAD_U32(wrapper + 4) << " value=0x" << context.r4.u32
                << " lr=0x" << context.lr << std::dec << '\n';
        }
    }
    AppendDeepTrace("FUNCTION", address, context);
    AppendGraphicsTrace("FUNCTION", address, context);
    AppendPostSwapTrace("FUNCTION", address, context);
}

bool RuntimeInterruptDeliveryAllowedAtFunction(uint32_t address) noexcept {
    const auto isHelperEntry = [address](uint32_t first, uint32_t last, uint32_t stride) {
        return address >= first && address <= last && ((address - first) % stride) == 0;
    };
    // These are the eight verified compiler-helper families from the title's
    // own XEX.  A restore helper enters after its caller has moved r1 back to
    // the caller frame but before it reloads the saved registers. Delivering
    // a guest interrupt there reuses the same guest stack and overwrites the
    // pending spills. Save helpers are also internal prologue machinery, not
    // architectural scheduling boundaries, so defer across the full set.
    // Almost all title functions are below the first helper family. Avoid
    // testing every sparse family on that ordinary path while preserving the
    // exact exclusions (including the gaps between them).
    if (address < 0x828999F0u || address > 0x829B9194u) return true;
    if (address <= 0x82899A84u) {
        return !(isHelperEntry(0x828999F0u, 0x82899A34u, 4u) ||
                 isHelperEntry(0x82899A40u, 0x82899A84u, 4u));
    }
    if (address < 0x8289A190u) return true;
    if (address <= 0x8289A220u) {
        return !(isHelperEntry(0x8289A190u, 0x8289A1D4u, 4u) ||
                 isHelperEntry(0x8289A1DCu, 0x8289A220u, 4u));
    }
    if (address < 0x829B8C70u) return true;
    return !(isHelperEntry(0x829B8C70u, 0x829B8CF8u, 8u) ||
             isHelperEntry(0x829B8D04u, 0x829B8EFCu, 8u) ||
             isHelperEntry(0x829B8F08u, 0x829B8F90u, 8u) ||
             isHelperEntry(0x829B8F9Cu, 0x829B9194u, 8u));
}

void RuntimePcProbe(PPCContext& context, uint8_t* base, uint32_t address) {
    RuntimeSetActivePpcContext(&context, base, address);
    if (address == 0x82358198u || address == 0x8235819Cu ||
        address == 0x823582B4u || address == 0x823582B8u) {
        const uint32_t sequence =
            frsqrteProbeSequence.fetch_add(1, std::memory_order_relaxed);
        if (sequence < 64u) {
            const auto timestampUs = std::chrono::duration_cast<
                std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::lock_guard lock(frsqrteProbeMutex);
            std::filesystem::create_directories("logs");
            std::ofstream out("logs/m7_frsqrte_probe48.log", std::ios::app);
            out << "FRSQRTE_STATE sequence=" << sequence
                << " timestamp_us=" << timestampUs
                << " thread=" << CurrentGuestThreadId()
                << " pc=0x" << std::hex << address
                << " lr=0x" << context.lr
                << " r1=0x" << context.r1.u32
                << " f3=0x" << context.f3.u64
                << " f4=0x" << context.f4.u64
                << " f5=0x" << context.f5.u64
                << " f11=0x" << context.f11.u64
                << " f12=0x" << context.f12.u64
                << " f13=0x" << context.f13.u64
                << " fpscr=0x" << context.fpscr.value
                << std::dec << '\n';
        }
    }
    if constexpr (!kLegacyFunctionTracingEnabled) return;
    AppendDeepTrace("PC_PROBE", address, context);
    AppendGraphicsTrace("PC_PROBE", address, context);
    AppendAllocatorGeneralAllocationResult(context, base, address);
    std::filesystem::create_directories("logs");
    if (address == 0x820C85BCu) {
        const uint32_t runtimeThread = CurrentGuestThreadId();
        const uint32_t titleThread = context.r3.u32;
        if (!titleThread || titleThread != runtimeThread) {
            const uint32_t sequence =
                allocatorLockProbeSequence.fetch_add(1, std::memory_order_relaxed);
            std::ofstream out("logs/m5_allocator_thread_id_mismatch_pcr_fix.log", std::ios::app);
            out << "LOCK_THREAD_ID sequence=" << sequence << " runtime_thread=" << runtimeThread
                << " title_thread=0x" << std::hex << titleThread << " r13=0x" << context.r13.u32
                << " pcr=0x" << PPC_LOAD_U32(context.r13.u32 + 0x100u)
                << " lr=0x" << context.lr << std::dec << '\n';
        }
        return;
    }
    if (address == 0x820C85D8u || address == 0x820C85ECu || address == 0x820C8694u ||
        address == 0x820C86A0u || address == 0x820C8894u || address == 0x820C88C4u) {
        uint32_t critical = context.r31.u32;
        if (address == 0x820C86A0u) critical = context.r31.u32;
        const uint32_t runtimeThread = CurrentGuestThreadId();
        const uint32_t owner = PPC_LOAD_U32(critical + 8u);
        const uint32_t recursion = PPC_LOAD_U32(critical + 12u);
        const bool acquireExit = address == 0x820C85D8u || address == 0x820C86A0u;
        const bool badAcquire = acquireExit && (owner != runtimeThread || recursion == 0);
        const bool slowWait = address == 0x820C8694u;
        const bool releaseMismatch = address == 0x820C88C4u && owner != runtimeThread;
        if (badAcquire || slowWait || releaseMismatch) {
            const uint32_t sequence =
                allocatorLockProbeSequence.fetch_add(1, std::memory_order_relaxed);
            std::ofstream out("logs/m5_allocator_lock_paths_pcr_fix.log", std::ios::app);
            out << "LOCK_PATH sequence=" << sequence << " thread=" << runtimeThread
                << " pc=0x" << std::hex << address << " critical=0x" << critical
                << " lock_count=0x" << PPC_LOAD_U32(critical)
                << " waiter_state=0x" << PPC_LOAD_U32(critical + 4u)
                << " owner=0x" << owner << " recursion=0x" << recursion
                << " event=0x" << PPC_LOAD_U32(critical + 16u) << " r3=0x" << context.r3.u32
                << " r29=0x" << context.r29.u32 << " r30=0x" << context.r30.u32
                << " lr=0x" << context.lr << std::dec << " bad_acquire=" << badAcquire
                << " slow_wait=" << slowWait << " release_mismatch=" << releaseMismatch << '\n';
        }
        return;
    }
    if (address == 0x822159E8u || address == 0x82215A18u || address == 0x82216068u) {
        const uint32_t allocator = context.r24.u32;
        const uint32_t thread = CurrentGuestThreadId();
        const bool acquire = address == 0x822159E8u;
        const uint32_t guestLockCount = PPC_LOAD_U32(allocator + 4u);
        const bool invalidHeader = guestLockCount > 0x10000u;
        bool overlap = false;
        bool mismatch = false;
        uint32_t recordedOwner = 0;
        uint32_t recordedDepth = 0;
        {
            std::lock_guard<std::mutex> lock(allocatorCriticalMutex);
            auto& owner = allocatorCriticalOwners[allocator];
            recordedOwner = owner.thread;
            recordedDepth = owner.depth;
            if (acquire) {
                overlap = owner.depth != 0 && owner.thread != thread;
                if (!owner.depth) owner.thread = thread;
                ++owner.depth;
            } else {
                mismatch = !owner.depth || owner.thread != thread;
                if (owner.depth && owner.thread == thread && --owner.depth == 0) owner.thread = 0;
            }
        }
        const uint32_t sequence = allocatorCriticalSequence.fetch_add(1, std::memory_order_relaxed);
        if (sequence < 512 || overlap || mismatch || invalidHeader) {
            std::ofstream out("logs/m5_allocator_critical_overlap_pcr_fix.log", std::ios::app);
            out << "ALLOC_CRITICAL sequence=" << sequence << " action="
                << (acquire ? "acquire" : "release") << " thread=" << thread
                << " allocator=0x" << std::hex << allocator << " pc=0x" << address
                << " prior_owner=0x" << recordedOwner << " prior_depth=0x" << recordedDepth
                << " guest_lock_count=0x" << guestLockCount
                << " guest_waiter_state=0x" << PPC_LOAD_U32(allocator + 8)
                << " guest_owner=0x" << PPC_LOAD_U32(allocator + 12)
                << " guest_recursion=0x" << PPC_LOAD_U32(allocator + 16)
                << std::dec << " overlap=" << overlap << " mismatch=" << mismatch
                << " invalid_header=" << invalidHeader << '\n';
        }
        if (invalidHeader &&
            !allocatorHeaderCorruptionSeen.exchange(true, std::memory_order_relaxed)) {
            std::ofstream out("logs/m6d_allocator_first_header_corruption.log", std::ios::trunc);
            out << "FIRST_ALLOCATOR_HEADER_CORRUPTION sequence=" << sequence
                << " action=" << (acquire ? "acquire" : "release")
                << " thread=" << thread << " pc=0x" << std::hex << address
                << " allocator=0x" << allocator << " caller=0x" << context.lr
                << " vtable=0x" << PPC_LOAD_U32(allocator)
                << " lock_count=0x" << guestLockCount
                << " waiter=0x" << PPC_LOAD_U32(allocator + 8u)
                << " owner=0x" << PPC_LOAD_U32(allocator + 12u)
                << " recursion=0x" << PPC_LOAD_U32(allocator + 16u)
                << " event=0x" << PPC_LOAD_U32(allocator + 20u)
                << " tree_head=0x" << PPC_LOAD_U32(allocator + 36u)
                << " tree_tagged=0x" << PPC_LOAD_U32(allocator + 40u)
                << " heap_b8=0x" << PPC_LOAD_U32(allocator + 0xB8u)
                << " heap_bc=0x" << PPC_LOAD_U32(allocator + 0xBCu)
                << " heap_c4=0x" << PPC_LOAD_U32(allocator + 0xC4u)
                << " heap_cc=0x" << PPC_LOAD_U32(allocator + 0xCCu)
                << " heap_d4=0x" << PPC_LOAD_U32(allocator + 0xD4u)
                << " heap_d8=0x" << PPC_LOAD_U32(allocator + 0xD8u) << std::dec << '\n';
        }
        return;
    }
    if (address == 0x8259DDF4u || address == 0x8259DE0Cu || address == 0x8259DE10u) {
        const uint32_t sequence = tlsTransitionCount.load(std::memory_order_relaxed);
        if (sequence <= 128) {
            std::ofstream out("logs/m5_tls_state_transitions.log", std::ios::app);
            out << "TLS_SET_PROBE thread=" << CurrentGuestThreadId() << " pc=0x" << std::hex
                << address << " wrapper=0x" << context.r31.u32 << " value=0x" << context.r30.u32
                << " result_cell=0x" << context.r3.u32 << " ctr=0x" << context.ctr.u32
                << " lr=0x" << context.lr << std::dec << '\n';
        }
        return;
    }
    if (address == 0x825A558Cu || address == 0x825A5710u) {
        const uint32_t sequence = tlsInitializerProbeCount.fetch_add(1, std::memory_order_relaxed);
        if (sequence < 128) {
            std::ofstream out("logs/m5_tls_initializer_allocations_v3.log", std::ios::app);
            out << "TLS_INIT_PROBE sequence=" << sequence << " thread=" << CurrentGuestThreadId()
                << " pc=0x" << std::hex << address << " result=0x" << context.r3.u32
                << " wrapper=0x" << context.r27.u32 << " allocation=0x" << context.r28.u32
                << " cell=0x" << context.r31.u32 << " lr=0x" << context.lr << std::dec << '\n';
        }
        return;
    }
    if (address == 0x820C0F88u) {
        const uint32_t sequence = allocatorWrapperProbeCount.fetch_add(1, std::memory_order_relaxed);
        if (sequence < 512) {
            std::ofstream out("logs/m5_allocator_wrapper_results_v3.log", std::ios::app);
            out << "ALLOC_WRAPPER sequence=" << sequence << " thread=" << CurrentGuestThreadId()
                << " result=0x" << std::hex << context.r3.u32 << " wrapper=0x" << context.r31.u32
                << " lr=0x" << context.lr << " r1=0x" << context.r1.u32 << std::dec
                << " outcome=" << (context.r3.u32 ? "success" : "null") << '\n';
        }
        return;
    }
    if (address == 0x82215A00u) {
        const uint32_t descriptor = context.r3.u32;
        if (descriptor == 0) {
            const uint32_t sequence =
                allocatorInnerFailureCount.fetch_add(1, std::memory_order_relaxed);
            if (sequence < 128) {
                std::ofstream out("logs/m5_allocator_inner_failures_pcr_fix.log", std::ios::app);
                out << "ALLOC_INNER_NULL sequence=" << sequence
                    << " thread=" << CurrentGuestThreadId()
                    << " allocator=0x" << std::hex << context.r24.u32
                    << " aligned_size=0x" << context.r29.u32
                    << " alignment=0x" << context.r26.u32 << " lr=0x" << context.lr
                    << " r1=0x" << context.r1.u32
                    << " lock_count=0x" << PPC_LOAD_U32(context.r24.u32 + 4)
                    << " lock_waiters=0x" << PPC_LOAD_U32(context.r24.u32 + 8)
                    << " lock_owner=0x" << PPC_LOAD_U32(context.r24.u32 + 12)
                    << " lock_recursion=0x" << PPC_LOAD_U32(context.r24.u32 + 16)
                    << " heap_c4=0x" << PPC_LOAD_U32(context.r24.u32 + 0xC4)
                    << " heap_cc=0x" << PPC_LOAD_U32(context.r24.u32 + 0xCC)
                    << " heap_d4=0x" << PPC_LOAD_U32(context.r24.u32 + 0xD4)
                    << " heap_d8=0x" << PPC_LOAD_U32(context.r24.u32 + 0xD8)
                    << std::dec << '\n';
            }
        } else if (PPC_LOAD_U32(descriptor + 0x1Cu) == 0) {
            const uint32_t sequence =
                allocatorCorruptDescriptorCount.fetch_add(1, std::memory_order_relaxed);
            if (sequence < 128) {
                std::ofstream out("logs/m5_allocator_corrupt_descriptors_pcr_fix.log", std::ios::app);
                out << "ALLOC_CORRUPT_DESCRIPTOR sequence=" << sequence
                    << " thread=" << CurrentGuestThreadId() << " allocator=0x" << std::hex
                    << context.r24.u32 << " descriptor=0x" << descriptor
                    << " descriptor_00=0x" << PPC_LOAD_U32(descriptor)
                    << " descriptor_04=0x" << PPC_LOAD_U32(descriptor + 4)
                    << " descriptor_08=0x" << PPC_LOAD_U32(descriptor + 8)
                    << " descriptor_1c=0x" << PPC_LOAD_U32(descriptor + 0x1C)
                    << " aligned_size=0x" << context.r29.u32
                    << " alignment=0x" << context.r26.u32
                    << " lock_count=0x" << PPC_LOAD_U32(context.r24.u32 + 4)
                    << " lock_waiters=0x" << PPC_LOAD_U32(context.r24.u32 + 8)
                    << " lock_owner=0x" << PPC_LOAD_U32(context.r24.u32 + 12)
                    << " lock_recursion=0x" << PPC_LOAD_U32(context.r24.u32 + 16)
                    << " heap_c4=0x" << PPC_LOAD_U32(context.r24.u32 + 0xC4)
                    << " heap_cc=0x" << PPC_LOAD_U32(context.r24.u32 + 0xCC)
                    << " heap_d4=0x" << PPC_LOAD_U32(context.r24.u32 + 0xD4)
                    << " heap_d8=0x" << PPC_LOAD_U32(context.r24.u32 + 0xD8)
                    << " lr=0x" << context.lr << " r1=0x" << context.r1.u32 << std::dec
                    << '\n';
            }
        }
    }
}

bool RuntimeGeneratedAddressInRange(uint32_t address) noexcept {
    const uint64_t codeEnd = uint64_t(PPC_CODE_BASE) + uint64_t(PPC_CODE_SIZE);
    return (address & 3u) == 0 && address >= PPC_CODE_BASE && uint64_t(address) < codeEnd;
}

// Real call/return observation, not generated-function entry counts. A guest
// exception propagates without recording successful completion.
void InvokeObservedWorldWork(PPCFunc* target, PPCContext& context,
                             uint8_t* base, uint32_t address) {
    const uint32_t world = context.r3.u32;
    const uint32_t caller = context.lr;
    if (PPC_LOAD_U32(world) != 0x820807E0u) {
        target(context, base);
        return;
    }
    const bool consumer = address == 0x824A6898u;
    const auto index = static_cast<size_t>(darkness::diagnostics::ClassifyWorldWorkPath(caller));
    const uint32_t readBefore = PPC_LOAD_U32(world + 0xC3Cu);
    const uint32_t writeBefore = PPC_LOAD_U32(world + 0xC38u);
    const uint32_t dependency = PPC_LOAD_U32(world + 0xC28u);
    const uint32_t capacity = dependency ? PPC_LOAD_U32(dependency + 4u) : 0;
    const uint32_t item = consumer ? context.r4.u32 : 0;
    const uint32_t itemStamp = item ? PPC_LOAD_U32(item + 0x30u) : 0;
    // Existing optional observer only. No new dispatch hook, guest writes,
    // per-draw work, clocks, or log frequency changes.
    const uint32_t itemSteps = item ? PPC_LOAD_U32(item + 0x24u) : 0;
    const uint32_t clientFlags = consumer ? context.r5.u32 : 0;
    const uint32_t clientMode = consumer ? PPC_LOAD_U32(world + 0x204u) : 0;
    const uint32_t clientStepsBefore = consumer ? PPC_LOAD_U32(world + 0xCA0u) : 0;
    const uint64_t worldTimeBefore = PPC_LOAD_U64(world + 0x190u);
    const uint64_t localReturns = frameCadenceThreadConsumerReturns;
    uint64_t ordinal = 0;
    if (!consumer) {
        ordinal = frameCadenceWorldAttempts[index].fetch_add(1, std::memory_order_relaxed) + 1;
        if (readBefore == writeBefore)
            frameCadenceWorldEmpty[index].fetch_add(1, std::memory_order_relaxed);
    }
    const auto started = FrameCadenceClock::now();
    target(context, base);
    const auto finished = FrameCadenceClock::now();
    const uint32_t clientStepsAfter = consumer ? PPC_LOAD_U32(world + 0xCA0u) : 0;
    const auto clientDelta = darkness::diagnostics::ObserveWorldClientSteps(
        clientFlags, clientMode, itemSteps, clientStepsBefore, clientStepsAfter);
    if (consumer) {
        ++frameCadenceThreadConsumerReturns;
        ordinal = frameCadenceConsumerReturns.fetch_add(1, std::memory_order_relaxed) + 1;
        auto& steps = frameCadenceThreadClientSteps;
        if (steps.world != world) {
            const uint64_t nextEpoch = steps.epoch + 1u;
            steps = {};
            steps.world = world;
            steps.epoch = nextEpoch;
        }
        ++steps.returns;
        steps.requested += clientDelta.requested;
        if (clientDelta.verified) steps.verified += clientDelta.observed;
        else ++steps.mismatches;
    }
    const uint32_t readAfter = PPC_LOAD_U32(world + 0xC3Cu);
    const bool consumed = !consumer && darkness::diagnostics::IsVerifiedQueueConsumption(
        readBefore, writeBefore, readAfter, capacity,
        frameCadenceThreadConsumerReturns - localReturns);
    if (consumed)
        frameCadenceWorldConsumed[index].fetch_add(1, std::memory_order_relaxed);
    if (!darkness::diagnostics::ShouldSampleFrameCadence(ordinal)) return;
    std::ostringstream details;
    details << "world=0x" << std::hex << world << " callee=0x" << address
            << " caller=0x" << caller << " item=0x" << item
            << " item_timestamp_bits=0x" << itemStamp
            << " world_time_before_bits=0x" << worldTimeBefore
            << " world_time_after_bits=0x" << PPC_LOAD_U64(world + 0x190u)
            << " base_step_bits=0x" << PPC_LOAD_U32(world + 0x19Cu)
            << std::dec << " path=" << index << " read_before=" << readBefore
            << " write_before=" << writeBefore << " read_after=" << readAfter
            << " capacity=" << capacity << " verified_consumed=" << consumed
            << " duration_us=" << std::chrono::duration_cast<std::chrono::microseconds>(finished-started).count()
            << " path_attempts=" << frameCadenceWorldAttempts[index].load(std::memory_order_relaxed)
            << " path_empty=" << frameCadenceWorldEmpty[index].load(std::memory_order_relaxed)
            << " path_consumed=" << frameCadenceWorldConsumed[index].load(std::memory_order_relaxed)
            << " consumer_returns=" << frameCadenceConsumerReturns.load(std::memory_order_relaxed);
    if (consumer) {
        const auto& steps = frameCadenceThreadClientSteps;
        details << " item_client_steps=" << itemSteps
                << " client_flags=" << clientFlags << " client_mode=" << clientMode
                << " client_counter_before=" << clientStepsBefore
                << " client_counter_after=" << clientStepsAfter
                << " client_step_delta=" << clientDelta.observed
                << " client_step_verified=" << clientDelta.verified
                << " client_step_epoch=" << steps.epoch
                << " client_step_returns=" << steps.returns
                << " client_steps_requested_total=" << steps.requested
                << " client_steps_verified_total=" << steps.verified
                << " client_step_mismatches=" << steps.mismatches;
    }
    AppendFrameCadenceLine(consumer ? "world_consumer_return" : "world_queue_return", ordinal, details.str());
}

void RuntimeIndirectDispatch(PPCContext& context, uint8_t* base, uint32_t address) {
    if constexpr (kMainUpdatePathTraceEnabled) if (
        mainUpdateTraceActive && context.lr == 0x820DE17Cu) {
        std::ofstream out("logs/m6d_main_update_path.log", std::ios::app);
        out << "MAIN_UPDATE_COMPLETE thread=" << CurrentGuestThreadId()
            << " events=" << mainUpdateTraceSequence << " callback=0x" << std::hex
            << address << " object=0x" << mainUpdateTraceObject
            << " state2720=0x" << PPC_LOAD_U32(mainUpdateTraceObject + 2720u)
            << " state2724=0x" << PPC_LOAD_U32(mainUpdateTraceObject + 2724u)
            << " state2728=0x" << PPC_LOAD_U32(mainUpdateTraceObject + 2728u)
            << " state2732=0x" << PPC_LOAD_U32(mainUpdateTraceObject + 2732u)
            << std::dec << '\n';
        mainUpdateTraceActive = false;
    }
    if constexpr (kLegacyFunctionTracingEnabled) {
        AppendDeepTrace("INDIRECT", address, context);
        AppendGraphicsTrace("INDIRECT", address, context);
        AppendPostSwapTrace("INDIRECT", address, context);
    }
    // These two bctrl sites are the verified continuation of the completed
    // main-controller work loop. Record only this bounded pair so the
    // producer/consumer handoff remains visible without global call logging.
    if (kLegacyFunctionTracingEnabled &&
        (context.lr == 0x821F1000u || context.lr == 0x821F1010u)) {
        std::filesystem::create_directories("logs");
        std::ofstream out("logs/post_content_indirect_continuation.log", std::ios::app);
        out << "INDIRECT thread=" << CurrentGuestThreadId() << " caller_lr=0x" << std::hex
            << context.lr << " target=0x" << address << std::dec << '\n';
    }
    // PPC_LOOKUP_FUNC is a raw table-addressing macro. An address below
    // PPC_CODE_BASE underflows its unsigned index before a null entry can be
    // checked, which previously turned a guest control-flow defect into a
    // host access violation. Validate the generated-code domain first.
    if (!RuntimeGeneratedAddressInRange(address)) {
        std::ostringstream message;
        message << "guest indirect target outside generated code address=0x" << std::hex
                << address << " lr=0x" << context.lr << " r1=0x" << context.r1.u32
                << " r3=0x" << context.r3.u32 << " r4=0x" << context.r4.u32
                << " r5=0x" << context.r5.u32 << " r11=0x" << context.r11.u32
                << " ctr=0x" << context.ctr.u32 << " thread=" << std::dec
                << CurrentGuestThreadId();
        throw std::runtime_error(message.str());
    }
    PPCFunc* target = PPC_LOOKUP_FUNC(base, address);
    if (!target) {
        std::ostringstream message;
        message << "unresolved guest indirect target address=0x" << std::hex << address
                << " lr=0x" << context.lr << " thread=" << std::dec
                << CurrentGuestThreadId();
        throw std::runtime_error(message.str());
    }
    const auto invokeTarget = [&] {
        if (darkness::diagnostics::IsWorldWorkDispatch(address, context.lr) &&
            frameCadenceAttributionEnabled.load(std::memory_order_relaxed)) {
            InvokeObservedWorldWork(target, context, base, address);
        } else {
            target(context, base);
        }
    };
    if constexpr (!kLegacyFunctionTracingEnabled) {
        invokeTarget();
        return;
    }
    const uint32_t callerLr = context.lr;
    const bool traceXdfProviderSelection =
        kCompletedXdfTraceEnabled &&
        (callerLr == 0x82204E04u || callerLr == 0x82204F80u ||
         callerLr == 0x82204FF8u);
    uint32_t xdfProviderSequence = UINT32_MAX;
    uint32_t xdfProviderObject = 0;
    uint32_t xdfProviderVtable = 0;
    uint32_t xdfPathObject = 0;
    uint32_t xdfWrapper = 0;
    uint32_t xdfWrapperStreamBefore = 0;
    uint32_t xdfWrapperTypeBefore = 0;
    if (traceXdfProviderSelection) {
        xdfProviderSequence =
            xdfProviderSelectionTraceSequence.fetch_add(1, std::memory_order_relaxed);
        xdfProviderObject = context.r3.u32;
        xdfProviderVtable = xdfProviderObject ? PPC_LOAD_U32(xdfProviderObject) : 0;
        xdfPathObject = context.r4.u32;
        // r23 is the destination stream wrapper for the lifetime of
        // sub_822049F8. The three observed bctrl sites choose type 2, 0, or 1.
        xdfWrapper = context.r23.u32;
        if (xdfWrapper) {
            xdfWrapperStreamBefore = PPC_LOAD_U32(xdfWrapper + 16u);
            xdfWrapperTypeBefore = PPC_LOAD_U32(xdfWrapper + 20u);
        }
        if (xdfProviderSequence < 256u) {
            const uint32_t root = PPC_LOAD_U32(0x82A690F8u);
            const uint32_t manager = root ? PPC_LOAD_U32(root + 12u) : 0;
            std::lock_guard<std::mutex> lock(xdfProviderSelectionTraceMutex);
            std::filesystem::create_directories("logs");
            std::ofstream out(kXdfProviderSelectionTracePath,
                              xdfProviderSequence ? std::ios::app : std::ios::trunc);
            out << "XDF_PROVIDER sequence=" << xdfProviderSequence
                << " phase=begin thread=" << CurrentGuestThreadId()
                << " caller_lr=0x" << std::hex << callerLr << " target=0x"
                << address << " provider=0x" << xdfProviderObject << " vtable=0x"
                << xdfProviderVtable << " path_object=0x" << xdfPathObject
                << " flags=0x" << context.r5.u32 << " wrapper=0x" << xdfWrapper
                << " wrapper_stream=0x" << xdfWrapperStreamBefore
                << " wrapper_type=0x" << xdfWrapperTypeBefore << " r20=0x"
                << context.r20.u32 << " r21=0x" << context.r21.u32 << " r25=0x"
                << context.r25.u32 << " root=0x" << root << " manager=0x"
                << manager << " manager_thread=0x"
                << (manager ? PPC_LOAD_U32(manager + 156u) : 0)
                << " manager_package=0x"
                << (manager ? PPC_LOAD_U32(manager + 164u) : 0) << " path=\""
                << ReadStarbreezeNarrowString(base, xdfPathObject) << "\""
                << std::dec << '\n';
        }
    }
    const uint32_t gameContextWorker =
        gameContextWorkerObject.load(std::memory_order_relaxed);
    const bool tracePreCleanupVirtual = callerLr == 0x820C2B30u;
    if (tracePreCleanupVirtual) {
        std::filesystem::create_directories("logs");
        std::ofstream out("logs/m6d_pre_cleanup_virtual_dispatch.log",
                          std::ios::trunc);
        out << "PRE_CLEANUP_VIRTUAL phase=begin thread=" << CurrentGuestThreadId()
            << " caller_lr=0x" << std::hex << callerLr << " target=0x"
            << address << " object=0x" << context.r3.u32 << " vtable=0x"
            << (context.r3.u32 ? PPC_LOAD_U32(context.r3.u32) : 0)
            << " r4=0x" << context.r4.u32 << " r5=0x" << context.r5.u32
            << " r6=0x" << context.r6.u32 << " worker=0x" << gameContextWorker;
        if (gameContextWorker) {
            out << " worker_flags=0x" << PPC_LOAD_U32(gameContextWorker + 4u)
                << " worker_last_chunk=0x"
                << PPC_LOAD_U32(gameContextWorker + 76u)
                << " worker_cursor=0x" << PPC_LOAD_U32(gameContextWorker + 88u)
                << " worker_base_position=0x"
                << PPC_LOAD_U64(gameContextWorker + 304u)
                << " worker_active_buffer=0x"
                << PPC_LOAD_U32(gameContextWorker + 312u);
        }
        out << std::dec << '\n';
    }
    const bool traceGameContextWorkerStop =
        gameContextWorker && context.r3.u32 == gameContextWorker &&
        (address == 0x821FB970u || address == 0x820ED4C0u ||
         address == 0x820ED4D0u || address == 0x820ED4E0u ||
         address == 0x820ED4E8u);
    uint32_t gameContextWorkerFlagsBefore = 0;
    if (traceGameContextWorkerStop) {
        gameContextWorkerFlagsBefore = PPC_LOAD_U32(gameContextWorker + 4u);
        std::lock_guard<std::mutex> lock(gameContextWorkerFlagTraceMutex);
        if (gameContextWorkerFlagTraceSequence < 4096u) {
            std::ofstream out(kGameContextWorkerFlagTracePath, std::ios::app);
            out << "WORKER_FLAG sequence=" << gameContextWorkerFlagTraceSequence++
                << " stage=indirect-begin thread=" << CurrentGuestThreadId()
                << " caller_lr=0x" << std::hex << callerLr << " target=0x"
                << address << " object=0x" << gameContextWorker << " flags=0x"
                << gameContextWorkerFlagsBefore << " r4=0x" << context.r4.u32
                << " r5=0x" << context.r5.u32 << " r6=0x" << context.r6.u32
                << std::dec << '\n';
        }
    }
    const bool traceThreadVirtual =
        callerLr == 0x821FBB58u || callerLr == 0x821FBB6Cu || callerLr == 0x821FBB80u;
    const uint32_t threadVirtualSequence = traceThreadVirtual
        ? threadVirtualDispatchTraceSequence.fetch_add(1, std::memory_order_relaxed)
        : UINT32_MAX;
    if (traceThreadVirtual && threadVirtualSequence < 128u) {
        std::filesystem::create_directories("logs");
        std::ofstream out("logs/m6d_thread_virtual_dispatch.log",
                          threadVirtualSequence ? std::ios::app : std::ios::trunc);
        out << "THREAD_VIRTUAL sequence=" << threadVirtualSequence
            << " phase=begin thread=" << CurrentGuestThreadId()
            << " caller_lr=0x" << std::hex << callerLr << " target=0x" << address
            << " object=0x" << context.r3.u32 << " vtable=0x"
            << (context.r3.u32 ? PPC_LOAD_U32(context.r3.u32) : 0);
        if (context.r3.u32) {
            for (uint32_t offset = 0; offset < 0x80u; offset += 4u) {
                out << " o" << std::setw(2) << std::setfill('0') << offset
                    << "=0x" << PPC_LOAD_U32(context.r3.u32 + offset);
            }
        }
        out << std::dec << '\n';
    }
    const bool traceAudioInitializerCall =
        (callerLr >= 0x828B55F0u && callerLr <= 0x828B591Cu) ||
        callerLr == 0x828B0138u || callerLr == 0x828B016Cu ||
        callerLr == 0x828B3F78u || callerLr == 0x828B3F90u ||
        callerLr == 0x828B3FACu || callerLr == 0x828B59A8u ||
        callerLr == 0x828B5A04u || callerLr == 0x828B5A20u ||
        callerLr == 0x828C0054u || callerLr == 0x828C00B8u;
    uint32_t sequence = 0;
    if (traceAudioInitializerCall) {
        sequence = audioInitializerIndirectSequence.fetch_add(1, std::memory_order_relaxed);
        if (sequence < 256) {
            std::cout << "AUDIO_INIT_INDIRECT_BEGIN sequence=" << sequence
                      << " thread=" << CurrentGuestThreadId() << " caller_lr=0x" << std::hex
                      << callerLr << " target=0x" << address << " r3=0x" << context.r3.u32
                      << " r4=0x" << context.r4.u32 << " r5=0x" << context.r5.u32
                      << " r6=0x" << context.r6.u32 << std::dec << '\n';
        }
    }
    const bool beginXdfProviderDeepTrace =
        traceXdfProviderSelection && xdfProviderSequence == 0u &&
        address == 0x8220BF78u;
    if (beginXdfProviderDeepTrace) {
        xdfProviderDeepTraceEvents = 0;
        xdfProviderDeepTraceActive = true;
    }
    invokeTarget();
    if (beginXdfProviderDeepTrace) xdfProviderDeepTraceActive = false;
    if (traceXdfProviderSelection && xdfProviderSequence < 256u) {
        std::lock_guard<std::mutex> lock(xdfProviderSelectionTraceMutex);
        std::ofstream out(kXdfProviderSelectionTracePath, std::ios::app);
        out << "XDF_PROVIDER sequence=" << xdfProviderSequence
            << " phase=end thread=" << CurrentGuestThreadId() << " caller_lr=0x"
            << std::hex << callerLr << " target=0x" << address
            << " provider=0x" << xdfProviderObject << " vtable=0x"
            << xdfProviderVtable << " result_r3=0x" << context.r3.u32
            << " result_r4=0x" << context.r4.u32 << " wrapper=0x" << xdfWrapper
            << " wrapper_stream_before=0x" << xdfWrapperStreamBefore
            << " wrapper_type_before=0x" << xdfWrapperTypeBefore
            << " wrapper_stream_after=0x"
            << (xdfWrapper ? PPC_LOAD_U32(xdfWrapper + 16u) : 0)
            << " wrapper_type_after=0x"
            << (xdfWrapper ? PPC_LOAD_U32(xdfWrapper + 20u) : 0) << std::dec
            << '\n';
    }
    if (tracePreCleanupVirtual) {
        std::ofstream out("logs/m6d_pre_cleanup_virtual_dispatch.log",
                          std::ios::app);
        out << "PRE_CLEANUP_VIRTUAL phase=end thread=" << CurrentGuestThreadId()
            << " caller_lr=0x" << std::hex << callerLr << " target=0x"
            << address << " result_r3=0x" << context.r3.u32
            << " result_r4=0x" << context.r4.u32 << " result_r5=0x"
            << context.r5.u32 << " result_lr=0x" << context.lr << std::dec
            << '\n';
    }
    if (traceGameContextWorkerStop) {
        const uint32_t flagsAfter = PPC_LOAD_U32(gameContextWorker + 4u);
        std::lock_guard<std::mutex> lock(gameContextWorkerFlagTraceMutex);
        if (gameContextWorkerFlagTraceSequence < 4096u) {
            std::ofstream out(kGameContextWorkerFlagTracePath, std::ios::app);
            out << "WORKER_FLAG sequence=" << gameContextWorkerFlagTraceSequence++
                << " stage=indirect-end thread=" << CurrentGuestThreadId()
                << " caller_lr=0x" << std::hex << callerLr << " target=0x"
                << address << " object=0x" << gameContextWorker
                << " flags_before=0x" << gameContextWorkerFlagsBefore
                << " flags_after=0x" << flagsAfter << " result_r3=0x"
                << context.r3.u32 << std::dec << '\n';
        }
        gameContextWorkerLastFlags = flagsAfter;
    }
    if (traceThreadVirtual && threadVirtualSequence < 128u) {
        std::ofstream out("logs/m6d_thread_virtual_dispatch.log", std::ios::app);
        out << "THREAD_VIRTUAL sequence=" << threadVirtualSequence
            << " phase=end thread=" << CurrentGuestThreadId()
            << " caller_lr=0x" << std::hex << callerLr << " target=0x" << address
            << " result_r3=0x" << context.r3.u32 << " result_r4=0x" << context.r4.u32
            << " result_lr=0x" << context.lr << std::dec << '\n';
    }
    if (traceAudioInitializerCall && sequence < 256) {
        std::cout << "AUDIO_INIT_INDIRECT_END sequence=" << sequence
                  << " thread=" << CurrentGuestThreadId() << " caller_lr=0x" << std::hex
                  << callerLr << " target=0x" << address << " result_r3=0x"
                  << context.r3.u32 << " result_r4=0x" << context.r4.u32 << std::dec << '\n';
    }
}

void RuntimeTraceThreadStarted(uint32_t threadId, uint32_t guestObject, uint32_t startAddress,
                               uint32_t pcr, uint32_t teb, uint32_t tls) {
    auto trace = CurrentTrace();
    trace->id.store(threadId, std::memory_order_relaxed);
    trace->guestObject.store(guestObject, std::memory_order_relaxed);
    trace->start.store(startAddress, std::memory_order_relaxed);
    trace->pcr.store(pcr, std::memory_order_relaxed);
    trace->teb.store(teb, std::memory_order_relaxed);
    trace->tls.store(tls, std::memory_order_relaxed);
}

void RuntimeTraceThreadTerminated(uint32_t) {}

void RuntimeTraceImport(const char* name, PPCContext& context) {
    auto trace = CurrentTrace();
    const uint32_t slot = trace->importWrite.fetch_add(1, std::memory_order_relaxed) % kImportHistory;
    trace->imports[slot].store(name, std::memory_order_relaxed);
    trace->lastLr.store(context.lr, std::memory_order_relaxed);
    if constexpr (kLegacyFunctionTracingEnabled) {
        AppendDeepTrace("IMPORT", context.lr, context, name);
        AppendGraphicsTrace("IMPORT", context.lr, context, name);
        AppendPostSwapTrace("IMPORT", context.lr, context, name);
    }
}

void RuntimeBeginGraphicsTrace(PPCContext& context) {
    if constexpr (!kLegacyFunctionTracingEnabled) return;
    std::lock_guard<std::mutex> lock(graphicsTraceMutex);
    if (graphicsTraceStarted) return;
    graphicsTraceStarted = true;
    graphicsTraceThread = CurrentGuestThreadId();
    std::filesystem::create_directories("logs");
    std::ofstream out("logs/m6a_graphics_frontier_trace.log", std::ios::trunc);
    out << "TRACE_BEGIN thread=" << graphicsTraceThread << " import=XGetVideoMode"
        << " lr=0x" << std::hex << context.lr << " output=0x" << context.r3.u32
        << " r1=0x" << context.r1.u32 << std::dec << " limit=" << kDeepTraceLimit << '\n';
}

void RuntimeBeginPostSwapTrace(PPCContext& context) {
    if (!kPostSwapTraceEnabled) return;
    std::lock_guard<std::mutex> lock(postSwapTraceMutex);
    if (postSwapTraceStarted) return;
    postSwapTraceStarted = true;
    postSwapTraceThread = CurrentGuestThreadId();
    postSwapFunctionOccurrences.clear();
    std::filesystem::create_directories("logs");
    std::ofstream out("logs/m6d_post_swap_guest_trace_v2.log", std::ios::trunc);
    out << "TRACE_BEGIN thread=" << postSwapTraceThread << " import=VdSwap"
        << " lr=0x" << std::hex << context.lr << " r1=0x" << context.r1.u32
        << " r3=0x" << context.r3.u32 << " r4=0x" << context.r4.u32
        << std::dec << " limit=" << kPostSwapTraceLimit
        << " function_filter=first-two-and-powers-of-two-per-address-lr" << '\n';
}

void RuntimeTraceWait(uint32_t handle, bool hasTimeout, int64_t timeout, uint32_t status,
                      PPCContext& context) {
    if (frameCadenceAttributionEnabled.load(std::memory_order_relaxed)) {
        AppendFrameCadenceWait(handle, hasTimeout, timeout, status);
    }
    auto trace = CurrentTrace();
    trace->waitHandle.store(handle, std::memory_order_relaxed);
    trace->waitTimeout.store(hasTimeout ? timeout : INT64_MIN, std::memory_order_relaxed);
    trace->waitStatus.store(status, std::memory_order_relaxed);
    if (kLegacyFunctionTracingEnabled && postContent.load(std::memory_order_relaxed)) {
        const uint64_t waitOrdinal =
            postContentWaits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (waitOrdinal == 64)
            RuntimeWriteThreadSnapshot("post-first-extra-content-entry-64-waits");
        if (waitOrdinal == 512)
            RuntimeWriteThreadSnapshot("post-first-extra-content-entry-512-waits");
        if (waitOrdinal == 4096)
            RuntimeWriteThreadSnapshot("post-first-extra-content-entry-4096-waits");
        if (waitOrdinal == 8192)
            RuntimeWriteThreadSnapshot("post-first-extra-content-entry-8192-waits");
        if (waitOrdinal == 16384)
            RuntimeWriteThreadSnapshot("post-first-extra-content-entry-16384-waits");
        if (waitOrdinal == 32768)
            RuntimeWriteThreadSnapshot("post-first-extra-content-entry-32768-waits");
        if (waitOrdinal == 65536)
            RuntimeWriteThreadSnapshot("post-first-extra-content-entry-65536-waits");
    }
    if (hasTimeout && timeout == 0) {
        trace->pollCount.fetch_add(1, std::memory_order_relaxed);
        if (postContent.load(std::memory_order_relaxed))
            postContentPolls.fetch_add(1, std::memory_order_relaxed);
    }
    RuntimeTraceImport("NtWaitForSingleObjectEx", context);
}

void RuntimeTracePostContentCompleted() {
    if constexpr (!kLegacyFunctionTracingEnabled) return;
    postContent.store(true, std::memory_order_relaxed);
    if (!postContentSnapshot.exchange(true, std::memory_order_relaxed))
        RuntimeWriteThreadSnapshot("post-content-directory-complete");
}

void RuntimeTraceControlRead(uint32_t address, uint32_t value, uint32_t function, uint32_t threadId) {
    if constexpr (!kLegacyFunctionTracingEnabled) return;
    std::lock_guard<std::mutex> lock(watchMutex);
    const auto previous = watchedValues.find(address);
    if (previous == watchedValues.end() || previous->second != value) {
        std::filesystem::create_directories("logs");
        std::ofstream out("logs/post_content_liveness_watchpoints.log", std::ios::app);
        out << "WATCH order=read thread=" << threadId << " function=0x" << std::hex << function
            << " address=0x" << address << " old=";
        if (previous != watchedValues.end()) out << "0x" << previous->second;
        else out << "unknown";
        out << " new=0x" << value << std::dec << " source=main-controller-poll\n";
        watchedValues[address] = value;
    }
}

bool RuntimeMilestoneSnapshotsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("DARKNESS_MILESTONE_SNAPSHOTS");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

namespace {
// Beside runtime_crash.log, whatever the working directory.
std::filesystem::path SnapshotLogPath(const wchar_t* name) {
    const wchar_t* reports = RuntimeFatalDirectory();
    const std::filesystem::path logs =
        reports && reports[0] ? std::filesystem::path(reports) : std::filesystem::path(L"logs");
    std::error_code error;
    std::filesystem::create_directories(logs, error);
    return logs / name;
}
}  // namespace

void RuntimeWriteThreadSnapshot(const char* phase) {
    std::vector<std::shared_ptr<TraceThread>> traces;
    {
        std::lock_guard<std::mutex> lock(traceMutex);
        traces = traceThreads;
    }
    std::ofstream out(SnapshotLogPath(L"post_content_thread_snapshot.log"), std::ios::app);
    out << "SNAPSHOT phase=" << phase << " threads=" << traces.size() << '\n';
    for (const auto& trace : traces) {
        GuestThreadInfo info{};
        const uint32_t object = trace->guestObject.load(std::memory_order_relaxed);
        const bool haveInfo = object && GetGuestObjects().GetThreadInfo(object, &info);
        out << "THREAD id=" << trace->id.load() << " object=0x" << std::hex << object
            << " start=0x" << trace->start.load() << " pc_function=0x" << trace->lastFunction.load()
            << " lr=0x" << trace->lastLr.load() << " r1=0x" << trace->lastR1.load()
            << " r13=0x" << trace->lastR13.load() << " pcr=0x" << trace->pcr.load()
            << " teb=0x" << trace->teb.load() << " tls=0x" << trace->tls.load() << std::dec
            << " state=" << (haveInfo ? ThreadStateName(info.state) : "UNKNOWN")
            << " priority=" << (haveInfo ? info.priority : 0)
            << " affinity=0x" << std::hex << (haveInfo ? info.affinity : 0) << std::dec
            << " wait_handle=0x" << std::hex << trace->waitHandle.load() << std::dec
            << " timeout=" << trace->waitTimeout.load() << " wait_status=0x" << std::hex
            << trace->waitStatus.load() << std::dec << " functions=" << trace->functionCount.load()
            << " polls=" << trace->pollCount.load() << '\n';
        out << "  LAST" << kFunctionHistory;
        const uint32_t fw = trace->functionWrite.load();
        for (size_t i = 0; i < kFunctionHistory; ++i) {
            const uint32_t value = trace->functions[(fw + i) % kFunctionHistory].load();
            if (value) out << " 0x" << std::hex << value;
        }
        out << std::dec << "\n  LAST32_IMPORTS";
        const uint32_t iw = trace->importWrite.load();
        for (size_t i = 0; i < kImportHistory; ++i) {
            const char* value = trace->imports[(iw + i) % kImportHistory].load();
            if (value) out << ' ' << value;
        }
        out << '\n';
    }
}

void RuntimeWriteCurrentThreadSnapshot(const char* phase) {
    const auto trace = CurrentTrace();
    std::ofstream out(SnapshotLogPath(L"m6d_content_thread_trace.log"), std::ios::app);
    out << "CURRENT_THREAD_SNAPSHOT phase=" << phase
        << " id=" << trace->id.load(std::memory_order_relaxed)
        << " object=0x" << std::hex << trace->guestObject.load(std::memory_order_relaxed)
        << " pc_function=0x" << trace->lastFunction.load(std::memory_order_relaxed)
        << " lr=0x" << trace->lastLr.load(std::memory_order_relaxed)
        << " r1=0x" << trace->lastR1.load(std::memory_order_relaxed)
        << std::dec << " functions=" << trace->functionCount.load(std::memory_order_relaxed)
        << '\n';
    out << "  LAST" << kFunctionHistory;
    const uint32_t functionWrite = trace->functionWrite.load(std::memory_order_relaxed);
    for (size_t index = 0; index < kFunctionHistory; ++index) {
        const uint32_t value =
            trace->functions[(functionWrite + index) % kFunctionHistory].load(
                std::memory_order_relaxed);
        if (value) out << " 0x" << std::hex << value;
    }
    out << std::dec << "\n  LAST" << kImportHistory << "_IMPORTS";
    const uint32_t importWrite = trace->importWrite.load(std::memory_order_relaxed);
    for (size_t index = 0; index < kImportHistory; ++index) {
        const char* value = trace->imports[(importWrite + index) % kImportHistory].load(
            std::memory_order_relaxed);
        if (value) out << ' ' << value;
    }
    out << '\n';
}
