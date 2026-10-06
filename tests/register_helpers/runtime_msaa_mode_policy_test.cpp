// MSAA mode (#16, V504): runtime/runtime_msaa_mode_policy.h.
#include "runtime_msaa_mode_policy.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace {
bool Check(bool condition, const std::string& message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}
}  // namespace

int main() {
  using msaa_mode::Policy;
  bool ok = true;
  Policy policy = Policy::kAutomatic;
  ok &= Check(msaa_mode::ParsePolicy("4x", policy) && policy == Policy::kAlways4x, "4x parses");
  ok &= Check(msaa_mode::ParsePolicy("2x", policy) && policy == Policy::kAlways2x, "2x parses");
  ok &= Check(msaa_mode::ParsePolicy("auto", policy) && policy == Policy::kAutomatic, "auto parses");
  policy = Policy::kAlways2x;
  ok &= Check(!msaa_mode::ParsePolicy("4X", policy) && !msaa_mode::ParsePolicy("", policy) &&
                  !msaa_mode::ParsePolicy("automatic", policy) && policy == Policy::kAlways2x,
              "other text is rejected and leaves the policy");
  for (Policy p : {Policy::kAlways4x, Policy::kAlways2x, Policy::kAutomatic}) {
    Policy parsed{};
    ok &= Check(msaa_mode::ParsePolicy(msaa_mode::PolicyName(p), parsed) && parsed == p,
                "names round-trip");
  }

  // The lock replaces only the two scene modes the game switches between.
  ok &= Check(msaa_mode::AppliedSamples(Policy::kAlways4x, 2) == 4 &&
                  msaa_mode::AppliedSamples(Policy::kAlways4x, 4) == 4,
              "always 4x");
  ok &= Check(msaa_mode::AppliedSamples(Policy::kAlways2x, 4) == 2 &&
                  msaa_mode::AppliedSamples(Policy::kAlways2x, 2) == 2,
              "always 2x");
  ok &= Check(msaa_mode::AppliedSamples(Policy::kAutomatic, 2) == 2 &&
                  msaa_mode::AppliedSamples(Policy::kAutomatic, 4) == 4,
              "automatic passes the game's choice");
  for (Policy p : {Policy::kAlways4x, Policy::kAlways2x, Policy::kAutomatic}) {
    ok &= Check(msaa_mode::AppliedSamples(p, 1) == 1 && msaa_mode::AppliedSamples(p, 0) == 0 &&
                    msaa_mode::AppliedSamples(p, 8) == 8,
                "other sample counts pass through");
  }

  // The title's rule (32 / 38 hysteresis on the lowest of the newest samples).
  ok &= Check(msaa_mode::GameDecision(4, 31.9f, 32.0f, 38.0f) == 2, "4x drops below 32");
  ok &= Check(msaa_mode::GameDecision(4, 32.0f, 32.0f, 38.0f) == 4, "4x keeps at 32");
  ok &= Check(msaa_mode::GameDecision(2, 38.0f, 32.0f, 38.0f) == 2, "2x keeps at 38");
  ok &= Check(msaa_mode::GameDecision(2, 38.1f, 32.0f, 38.0f) == 4, "2x returns above 38");
  // 60 FPS on 75 Hz: frames of one and two refresh intervals, lowest 37.5.
  ok &= Check(msaa_mode::GameDecision(2, 37.5f, 32.0f, 38.0f) == 2 &&
                  msaa_mode::GameDecision(4, 37.5f, 32.0f, 38.0f) == 4,
              "37.5 keeps whichever mode it found (stuck in 2x after one slow frame)");
  const float nan = std::numeric_limits<float>::quiet_NaN();
  ok &= Check(msaa_mode::GameDecision(4, nan, 32.0f, 38.0f) == 4 &&
                  msaa_mode::GameDecision(2, nan, 32.0f, 38.0f) == 2,
              "NaN changes nothing (fcmpu unordered)");

  // The verified words are the analysed code (encodings).
  ok &= Check(msaa_mode::kVerifiedWords[1].word == ((36u << 26) | (5u << 21) | (3u << 16) | 1204u),
              "stw r5,1204(r3)");
  ok &= Check(msaa_mode::kVerifiedWords[2].word == ((14u << 26) | (4u << 21) | 20u), "li r4,20");
  ok &= Check(msaa_mode::kVerifiedWords[3].word == ((32u << 26) | (5u << 21) | (23u << 16) | 656u),
              "lwz r5,656(r23)");
  ok &= Check(msaa_mode::kVerifiedWords[0].word == ((10u << 26) | (6u << 23) | (4u << 16) | 23u),
              "cmplwi cr6,r4,23");
  ok &= Check(msaa_mode::kDecisionReturn == msaa_mode::kVerifiedWords[3].address + 20,
              "the decision returns after lwz, lwz, lwz, mtctr, bctrl");
  if (!ok) return 1;
  std::cout << "runtime_msaa_mode_policy: all checks passed\n";
  return 0;
}
