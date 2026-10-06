#pragma once

// MSAA mode (#16, V504; runtime_msaa_mode.cpp): pure rules, tested by
// tests/register_helpers/runtime_msaa_mode_policy_test.cpp.
//
// The Darkness chooses its scene MSAA every frame in its performance-graph
// routine sub_820E06C0 (the developer graph "Real FPS, %dX" / "PFPS (Main,
// Sys,GPU)"). sub_820EC718 turns two 60-entry rings into 60 GPU "potential
// frame rate" samples, 100 / (A x B), from the frame's GPU use in percent (A,
// a copy of the statistics at 0x82A8A6D0) and the frame time in seconds (B).
// The lowest of the newest 10 decides: in 4x mode a value below 32 selects
// 2x, in 2x mode a value above 38 selects 4x again (otherwise the mode
// stays). The 2x mode draws the scene in 2 predicated tiles instead of 3; the
// console used it to save GPU time on heavy frames.
//
// On PC the GPU use always reads 100%, so the samples are the frame rate.
// Any frame slower than 31.25 ms (loading) drops to 2x; at 37.5 FPS (a menu
// at half of a 75 Hz refresh) no sample rises above 38, so the game stays in
// 2x mode for good (a 37 FPS cap here: every frame 2x after the first
// loading hitch). The #16 black dots and flickering squares come with that
// mode, and on PC it costs about twice the GPU time above internal scale 1x.
//
// The choice reaches the renderer through its option setter (vtable +200,
// sub_825EBCD0; option 20 stores the sample count at +1204), which the
// renderer copies to its active sample count (+1200) at the next frame start
// (sub_825EB6B8). A locked policy replaces the requested count there.

#include <cstdint>
#include <string_view>

namespace msaa_mode {

enum class Policy : uint8_t {
    kAutomatic = 0,  // the game's own switch (as on the console)
    kAlways2x = 2,
    kAlways4x = 4,
};

constexpr uint32_t kSampleCountOption = 20;
// The setter call in sub_820E06C0 (bctrl; the return address the guest sees).
constexpr uint32_t kDecisionReturn = 0x820E1190;

// Guest code words of the analysed code, checked before the first change.
struct ImageWord {
    uint32_t address;
    uint32_t word;
};
constexpr ImageWord kVerifiedWords[] = {
    {0x825EBCD0, 0x2B040017},  // setter entry: cmplwi cr6,r4,23
    {0x825EBE94, 0x90A304B4},  // option 20: stw r5,1204(r3)
    {0x820E1178, 0x38800014},  // decision call: li r4,20
    {0x820E117C, 0x80B70290},  // lwz r5,656(r23)
};
// The thresholds (for the diagnostic log only): 4x -> 2x below the float in
// .data at 0x82A48A78 (32.0, read live); 2x -> 4x above the .rdata constant
// 38.0 (not read: the runtime reads only protected .rdata).
constexpr uint32_t kDropBelowAddress = 0x82A48A78;
constexpr float kRiseAbove = 38.0f;

// The decision's stack frame (the caller's r1 at the setter call): the 60
// samples (floats, oldest first) end at +2044; the decision reads the newest
// 10 (+2008..+2044). The rings: doubles at +24 + 8 * index, index at +504.
constexpr uint32_t kNewestSamplesOffset = 2008;
constexpr uint32_t kDecisionSampleCount = 10;
constexpr uint32_t kGpuUseRingOffset = 3008;
constexpr uint32_t kFrameTimeRingOffset = 4032;
constexpr uint32_t kRingEntriesOffset = 24;
constexpr uint32_t kRingIndexOffset = 504;
constexpr uint32_t kRingLength = 60;

inline bool ParsePolicy(std::string_view text, Policy& policy) {
    if (text == "4x") {
        policy = Policy::kAlways4x;
    } else if (text == "2x") {
        policy = Policy::kAlways2x;
    } else if (text == "auto") {
        policy = Policy::kAutomatic;
    } else {
        return false;
    }
    return true;
}

inline const char* PolicyName(Policy policy) {
    switch (policy) {
        case Policy::kAlways4x:
            return "4x";
        case Policy::kAlways2x:
            return "2x";
        default:
            return "auto";
    }
}

// The sample count the renderer receives for the game's request. A locked
// policy replaces only the two scene modes the game switches between.
inline uint32_t AppliedSamples(Policy policy, uint32_t requested) {
    if (requested != 2 && requested != 4) return requested;
    if (policy == Policy::kAlways4x) return 4;
    if (policy == Policy::kAlways2x) return 2;
    return requested;
}

// The game's rule (for the diagnostic log): the next mode from the current
// one and the lowest of the newest samples (fcmpu: NaN changes nothing).
inline uint32_t GameDecision(uint32_t current, float lowest, float dropBelow, float riseAbove) {
    if (current == 4) return lowest < dropBelow ? 2u : 4u;
    return lowest > riseAbove ? 4u : current;
}

}  // namespace msaa_mode
