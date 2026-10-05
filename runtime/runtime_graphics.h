#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct PPCContext;
class RuntimePcConfigSnapshot;

struct RuntimeGraphicsSwapCommandInfo {
    uint32_t frontbufferVirtual;
    uint32_t frontbufferPhysical;
    uint32_t textureFormat;
    uint32_t width;
    uint32_t height;
};

struct RuntimeGraphicsHostInputState {
    uint32_t packetNumber{};
    uint16_t buttons{};
    uint8_t leftTrigger{};
    uint8_t rightTrigger{};
    int16_t thumbLX{};
    int16_t thumbLY{};
    int16_t thumbRX{};
    int16_t thumbRY{};
};

bool InitializeRuntimeGraphics(uint8_t* guest_virtual_base);
void ConfigureRuntimeGraphicsPcConfig(
    const RuntimePcConfigSnapshot& config,
    const std::filesystem::path& asset_root);
void ConfigureRuntimeGraphicsCache(const std::filesystem::path& cache_root,
                                   uint32_t title_id);
void ConfigureRuntimeGraphicsScreenshotRoot(
    const std::filesystem::path& screenshot_root);
void ShutdownRuntimeGraphics() noexcept;
bool RuntimeGraphicsIsActive() noexcept;
void RuntimeGraphicsSetClockGating(bool enabled) noexcept;
bool RuntimeGraphicsClockGatingEnabled() noexcept;

void RuntimeGraphicsSetInterruptCallback(uint32_t callback, uint32_t callback_data);
void RuntimeGraphicsSetSystemCommandBufferGpuIdentifierAddress(uint32_t address) noexcept;
uint32_t RuntimeGraphicsSystemCommandBufferGpuIdentifierAddress() noexcept;
void RuntimeGraphicsInitializeRingBuffer(uint32_t physical_address, uint32_t size_log2);
void RuntimeGraphicsEnableReadPointerWriteback(uint32_t physical_address,
                                               uint32_t block_size_log2);
RuntimeGraphicsSwapCommandInfo RuntimeGraphicsBuildSwapCommand(
    uint8_t* base, uint32_t commandBuffer, uint32_t textureFetch,
    uint32_t systemBuffer, uint32_t systemToken, uint32_t frontbufferAddress,
    uint32_t textureFormat, uint32_t colorSpace, uint32_t width,
    uint32_t height);
void RuntimeNotifyGuestPhysicalWrite(uint32_t guest_address, uint32_t length,
                                     const char* source_file = nullptr,
                                     uint32_t source_line = 0) noexcept;
void RuntimeFlushGuestPhysicalWrites() noexcept;
// Title guest frame-production boundary (runtime_function_trace.h
// kRuntimeFrameLimiterBoundary): paces production per display_frame_limit.
void RuntimeGraphicsGuestFrameBoundary() noexcept;
// The title's frame start (its frame driver, before the frame-pool wait and
// input processing): the plugin's latency sleep and markers.
void RuntimeGraphicsGuestFrameStart() noexcept;
// The title's input processor runs for the frame (latency measurement).
void RuntimeGraphicsGuestInputSample() noexcept;
// VdSwap queued the frame's swap command (the plugin's latency markers).
void RuntimeGraphicsGuestSwapQueued() noexcept;
// Whether the title shows a menu, the title screen or loading (the plugin
// paces those by display.menu_frame_rate): 0 gameplay, 1 menu.
void RuntimeGraphicsSetMenuState(uint32_t state) noexcept;
// The GPU busy time of the most recent frames in microseconds, oldest first
// (the plugin's GPU frame meter). Returns the number copied; totalFrames
// receives the number measured so far (0 without a meter).
uint32_t RuntimeGraphicsGpuFrameBusyUs(uint32_t* out, uint32_t capacity,
                                       uint64_t* totalFrames) noexcept;
// Arithmetic only, not allocation lifetime or concurrent-write certification.
bool RuntimeGraphicsGuestPhysicalRange(uint32_t address, uint32_t bytes,
                                      uint32_t& physical) noexcept;

// Diagnostic-only correlation marker. It never changes GPU or guest state;
// an updated ReXGlue plugin uses it to timestamp the next bounded swaps.
// Keyboard button prompts (0.9.1): the device of the latest player input
// (1 = controller, 2 = keyboard/mouse), reported when it changes; and the
// plugin's prompt state: bit 0 = keyboard prompts wanted, bits 8-31 = labels
// generation, labels as "button=label" lines in the buffer.
void RuntimeGraphicsNoteInputDevice(uint32_t device) noexcept;
uint32_t RuntimeGraphicsPromptLabels(char* buffer, uint32_t size) noexcept;
void RuntimeGraphicsNoteInputTransition(int64_t host_performance_counter,
                                        int64_t host_performance_frequency,
                                        uint32_t packet_number,
                                        uint32_t buttons) noexcept;

// Read-only diagnostic request identity. Zero means no enabled manual request;
// this is deliberately not presented as a guest/GPU frame identifier.
uint64_t RuntimeGraphicsCameraCaptureGeneration() noexcept;

// Bounded read-only copy of an existing physical alias for packet provenance.
// Returns raw guest bytes; does not notify, flush, wait or change guest memory.
bool RuntimeGraphicsCopyPacketBytes(uint32_t guest_address, uint32_t bytes,
                                    void* destination, uint32_t& physical) noexcept;

// Optional physical keyboard/relative-mouse source owned by the same window
// that receives host events. Disabled configuration leaves the proven XInput
// path untouched.
bool RuntimeGraphicsKeyboardMouseEnabled(uint32_t user_index) noexcept;
uint32_t RuntimeGraphicsKeyboardMouseUserIndex() noexcept;
bool RuntimeGraphicsPollKeyboardMouse(
    uint32_t user_index, RuntimeGraphicsHostInputState& output) noexcept;

// Native mouse look: raw relative counts accumulated since the previous call
// (consumed), with the user's sensitivity and Y inversion. False while
// keyboard/mouse is off, unfocused, not captured or in stick-bridge mode.
struct RuntimeGraphicsMouseLook {
    bool active{};
    int32_t dx{};
    int32_t dy{};
    float sensitivity{1.0f};
    bool invertY{};
};
bool RuntimeGraphicsConsumeMouseLook(uint32_t user_index,
                                     RuntimeGraphicsMouseLook& output) noexcept;

// In-game settings overlay (rex_gpu_embedded_settings_*): the host's schema,
// saved values and defaults as TOML; edits are polled as TOML; the host
// reports each save result. OverlayOpen: keyboard/mouse and pad belong to
// the overlay (the host neutralizes the pad for the title).
bool RuntimeGraphicsSettingsConfigure(const std::string& schema, const std::string& saved,
                                      const std::string& defaults, bool persistence) noexcept;
bool RuntimeGraphicsSettingsPoll(std::string& changes);
void RuntimeGraphicsSettingsSaved(const std::string& saved, const std::string& status) noexcept;
bool RuntimeGraphicsSettingsOverlayOpen() noexcept;
// Developer test input: the script's virtual controller buttons (`pad`, XInput
// wButtons), 0 without a driving script. Joins the native controller path.
uint16_t RuntimeGraphicsTestPadButtons() noexcept;
// V380: the player closed the game window; the title stops cleanly.
bool RuntimeGraphicsCloseRequested() noexcept;

// The pinned hardware adapter also owns XMA decode execution, but uses the
// runtime's physical allocation and the same authoritative guest mapping.
bool RuntimeGraphicsInitializeXma(uint32_t context_array_guest_address);
void RuntimeGraphicsShutdownXma() noexcept;
uint32_t RuntimeGraphicsAllocateXmaContext();
bool RuntimeGraphicsReleaseXmaContext(uint32_t context_guest_address);

void QueueRuntimeGraphicsInterrupt(uint32_t callback, uint32_t source,
                                   uint32_t cpu, uint32_t callback_data) noexcept;
bool DeliverRuntimeGraphicsInterrupts(PPCContext& context, uint8_t* base);
