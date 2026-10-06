// MSAA mode (#16, V504): see runtime_msaa_mode_policy.h.
#include "runtime_msaa_mode.h"

#include "runtime_memory_access.h"
#include "ppc_recomp_shared.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_825EBCD0);

namespace {

using msaa_mode::Policy;

std::atomic<uint8_t> g_policy{static_cast<uint8_t>(Policy::kAlways4x)};
bool g_trace = false;
// -1 not yet checked, 0 the guest image differs (the hook passes everything
// through), 1 verified.
std::atomic<int> g_verified{-1};
std::atomic<uint32_t> g_lastRequested{0};
std::atomic<uint64_t> g_decisions{0};
std::atomic<uint64_t> g_requestChanges{0};

float FloatFromBits(uint32_t bits) noexcept {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double DoubleFromBits(uint64_t bits) noexcept {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool VerifyImage(uint8_t* base) noexcept {
    int verified = g_verified.load(std::memory_order_acquire);
    if (verified >= 0) return verified != 0;
    verified = 1;
    for (const auto& expected : msaa_mode::kVerifiedWords) {
        const uint32_t found = PPC_LOAD_U32(expected.address);
        if (found != expected.word) {
            std::printf("RUNTIME_MSAA_VERIFY mismatch address=0x%08X expected=0x%08X found=0x%08X\n",
                        expected.address, expected.word, found);
            verified = 0;
        }
    }
    g_verified.store(verified, std::memory_order_release);
    std::printf("RUNTIME_MSAA_VERIFY verified=%d policy=%s\n", verified,
                msaa_mode::PolicyName(RuntimeMsaaModePolicy()));
    std::fflush(stdout);
    return verified != 0;
}

// The samples behind the game's decision, read from its frame (the caller's
// r1 at the setter call); only at the performance-graph call site.
struct DecisionInputs {
    bool present = false;
    float lowest = 0.0f;
    float newest = 0.0f;
    double gpuUsePercent = 0.0;
    double frameSeconds = 0.0;
};

double NewestRingValue(uint8_t* base, uint32_t ring) noexcept {
    const uint32_t index = PPC_LOAD_U32(ring + msaa_mode::kRingIndexOffset);
    if (index >= msaa_mode::kRingLength) return -1.0;
    return DoubleFromBits(PPC_LOAD_U64(ring + msaa_mode::kRingEntriesOffset + index * 8));
}

DecisionInputs ReadDecisionInputs(PPCContext& ctx, uint8_t* base) noexcept {
    DecisionInputs inputs;
    if (uint32_t(ctx.lr) != msaa_mode::kDecisionReturn) return inputs;
    const uint32_t frame = ctx.r1.u32;
    inputs.present = true;
    inputs.lowest = FloatFromBits(PPC_LOAD_U32(frame + msaa_mode::kNewestSamplesOffset));
    for (uint32_t i = 1; i < msaa_mode::kDecisionSampleCount; ++i) {
        const float sample =
            FloatFromBits(PPC_LOAD_U32(frame + msaa_mode::kNewestSamplesOffset + 4 * i));
        // fsel min chain: a NaN sample is skipped like the title's compare.
        if (sample < inputs.lowest) inputs.lowest = sample;
    }
    inputs.newest = FloatFromBits(PPC_LOAD_U32(
        frame + msaa_mode::kNewestSamplesOffset + 4 * (msaa_mode::kDecisionSampleCount - 1)));
    inputs.gpuUsePercent = NewestRingValue(base, frame + msaa_mode::kGpuUseRingOffset);
    inputs.frameSeconds = NewestRingValue(base, frame + msaa_mode::kFrameTimeRingOffset);
    return inputs;
}

// The test input's mark lines carry the same counter (phase splitting).
uint64_t QpcNow() noexcept {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return uint64_t(value.QuadPart);
}

}  // namespace

void ConfigureRuntimeMsaaMode(Policy policy, const char* source) noexcept {
    const uint8_t previous = g_policy.exchange(static_cast<uint8_t>(policy), std::memory_order_acq_rel);
    if (previous == static_cast<uint8_t>(policy) && std::strcmp(source, "config") != 0) return;
    std::printf("RUNTIME_MSAA_MODE policy=%s source=%s\n", msaa_mode::PolicyName(policy), source);
    std::fflush(stdout);
}

Policy RuntimeMsaaModePolicy() noexcept {
    return static_cast<Policy>(g_policy.load(std::memory_order_acquire));
}

void InitializeRuntimeMsaaModeDiagnostics() noexcept {
    const char* trace = std::getenv("DARKNESS_MSAA_TRACE");
    g_trace = trace && trace[0] == '1' && trace[1] == '\0';
    if (const char* mode = std::getenv("DARKNESS_MSAA_MODE")) {
        Policy policy;
        if (msaa_mode::ParsePolicy(mode, policy)) {
            ConfigureRuntimeMsaaMode(policy, "environment");
        } else {
            std::printf("RUNTIME_MSAA_MODE ignored_environment=%s\n", mode);
        }
    }
    std::printf("RUNTIME_MSAA_TRACE enabled=%u\n", g_trace ? 1u : 0u);
    std::fflush(stdout);
}

// The renderer's option setter (r3 renderer, r4 option, r5 value). Option 20
// is the scene sample count; every other option passes through untouched.
PPC_FUNC(sub_825EBCD0) {
    if (ctx.r4.u32 == msaa_mode::kSampleCountOption && VerifyImage(base)) {
        const uint32_t requested = ctx.r5.u32;
        const Policy policy = RuntimeMsaaModePolicy();
        const uint32_t applied = msaa_mode::AppliedSamples(policy, requested);
        const uint64_t decision = g_decisions.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint32_t previous = g_lastRequested.exchange(requested, std::memory_order_relaxed);
        const bool changed = previous != 0 && previous != requested;
        const uint64_t changes = changed
            ? g_requestChanges.fetch_add(1, std::memory_order_relaxed) + 1
            : g_requestChanges.load(std::memory_order_relaxed);
        // Without the trace: the game's own changes only (the first 32, then
        // every 256th), so a log shows when it wanted the other mode.
        if (g_trace || (changed && (changes <= 32 || changes % 256 == 0)) || decision == 1) {
            const DecisionInputs inputs = ReadDecisionInputs(ctx, base);
            const float dropBelow = FloatFromBits(PPC_LOAD_U32(msaa_mode::kDropBelowAddress));
            const float riseAbove = msaa_mode::kRiseAbove;
            const uint32_t expected = inputs.present && previous
                ? msaa_mode::GameDecision(previous, inputs.lowest, dropBelow, riseAbove)
                : requested;
            std::printf(
                "%s n=%llu qpc=%llu lr=0x%08X game=%u->%u applied=%u policy=%s low=%.2f "
                "newest=%.2f gpu_use=%.1f frame_ms=%.2f rule=%s changes=%llu\n",
                g_trace ? "MSAA_DECISION" : "RUNTIME_MSAA_REQUEST",
                static_cast<unsigned long long>(decision), static_cast<unsigned long long>(QpcNow()),
                uint32_t(ctx.lr), previous, requested, applied, msaa_mode::PolicyName(policy),
                inputs.lowest, inputs.newest, inputs.gpuUsePercent,
                inputs.frameSeconds * 1000.0,
                !inputs.present ? "n/a" : (expected == requested ? "match" : "MISMATCH"),
                static_cast<unsigned long long>(changes));
            if (!g_trace) std::fflush(stdout);
        }
        ctx.r5.u64 = applied;
    }
    __imp__sub_825EBCD0(ctx, base);
}
