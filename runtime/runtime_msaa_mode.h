#pragma once

// MSAA mode (#16, V504): graphics.msaa_mode keeps The Darkness in one scene
// MSAA mode. See runtime_msaa_mode_policy.h for the title's own switch.

#include "runtime_msaa_mode_policy.h"

// Startup (the configuration) and live (the in-game settings overlay).
void ConfigureRuntimeMsaaMode(msaa_mode::Policy policy, const char* source) noexcept;
msaa_mode::Policy RuntimeMsaaModePolicy() noexcept;

// Test switches, read once at startup:
//   DARKNESS_MSAA_MODE=auto|4x|2x  replaces the configured policy;
//   DARKNESS_MSAA_TRACE=1          logs every decision (one stdout line per
//                                  frame: the game's request, the applied
//                                  count, the samples behind it).
// Without the trace only changes of the game's request are logged.
void InitializeRuntimeMsaaModeDiagnostics() noexcept;
