#include "runtime_graphics.h"
#include "runtime_host_write_callbacks.h"
#include "runtime_guest_physical_range.h"
#include "runtime_graphics_pc_config.h"

#include "guest_physical_write_tracker.h"

#include "runtime_diagnostics.h"
#include "runtime_camera.h"
#include "ppc_recomp_shared.h"
#include "runtime_function_trace.h"
#include "runtime_mmio.h"
#include "runtime_threads.h"
#include "runtime_video_mode.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>

uint32_t RuntimeActiveNonHelperGuestFunction() noexcept;

// Queued graphics interrupts. Also read (relaxed) by the generated-code
// function-entry fast path, which calls the full entry hook only when this or
// the slow-path mask is nonzero (runtime_function_trace.h).
std::atomic<uint32_t> g_runtime_pending_interrupt_count{};
// CPU physical-write dirty pages (store hot path reads it lock-free).
GuestPhysicalWriteTracker g_guest_physical_writes;

namespace {
constexpr uint32_t kEmbeddedGpuAbiVersion = 12;
constexpr uint32_t kXenosMmioFirst = 0x7FC80000u;
constexpr uint32_t kXenosMmioLast = 0x7FCFFFFFu;
constexpr uint32_t kXmaMmioFirst = 0x7FEA0000u;
constexpr uint32_t kXmaMmioLast = 0x7FEAFFFFu;

using InterruptCallback = void (*)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
struct EmbeddedGpuCreateInfo {
    uint32_t struct_size;
    const char* backend;
    uint8_t* virtual_membase;
    uint8_t* physical_membase;
    double refresh_rate_hz;
    InterruptCallback interrupt_callback;
    void* interrupt_context;
    const char* config_path_utf8;
    const char* asset_root_utf8;
    const char* cache_root_utf8;
    uint32_t title_id;
    uint32_t shader_storage_blocking;
    const char* screenshot_root_utf8;
    const char* config_contents_utf8;
    uint64_t config_contents_size;
    uint32_t config_present;
    struct HostWriteCallbacks {
        void* context;
        void* (*begin)(void*, uint32_t, uint32_t) noexcept;
        void (*end)(void*, void*) noexcept;
    } host_writes;
    rex::graphics::pc_owned_camera_packet::Callbacks camera_packets;
};

struct EmbeddedGpuPcSettings {
    uint32_t struct_size;
    float gameplay_fov_degrees;
    uint32_t trace_camera_state;
    uint32_t keyboard_mouse_enabled;
    uint32_t keyboard_mouse_user_index;
};

struct EmbeddedHostInputState {
    uint32_t struct_size;
    uint32_t packet_number;
    uint16_t buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t thumb_lx;
    int16_t thumb_ly;
    int16_t thumb_rx;
    int16_t thumb_ry;
};

// Mirrors rex::system::EmbeddedMouseLook (rex/system/gpu_plugin.h).
struct EmbeddedMouseLook {
    uint32_t struct_size;
    uint32_t active;
    int32_t dx;
    int32_t dy;
    float sensitivity;
    uint32_t invert_y;
};

using CreateFn = void* (*)(uint32_t, const EmbeddedGpuCreateInfo*);
using DestroyFn = void (*)(void*);
using ReadMmioFn = uint32_t (*)(void*, uint32_t);
using WriteMmioFn = void (*)(void*, uint32_t, uint32_t);
using SetInterruptFn = void (*)(void*, uint32_t, uint32_t);
using InitializeRingFn = void (*)(void*, uint32_t, uint32_t);
using EnableReadPointerWritebackFn = void (*)(void*, uint32_t, uint32_t);
using NotifyPhysicalWriteFn = void (*)(void*, uint32_t, uint32_t);
using InterruptTimingTokenFn = uint64_t (*)(void*);
using InterruptCompletedFn = void (*)(void*);
using FrameBoundaryFn = void (*)(void*);
using GuestSwapFn = void (*)(void*);
using FrameStartFn = void (*)(void*);
using InputSampleFn = void (*)(void*);
using InterruptTimingRecordFn = void (*)(void*, uint64_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint64_t, uint64_t, uint64_t);
using NoteInputTransitionFn = void (*)(void*, int64_t, int64_t, uint32_t, uint32_t);
using NoteInputDeviceFn = void (*)(void*, uint32_t);
using GetPromptLabelsFn = uint32_t (*)(void*, char*, uint32_t);
using GetManualCaptureFn = uint32_t (*)(void*, uint64_t*);
using GetPcSettingsFn = uint32_t (*)(void*, EmbeddedGpuPcSettings*);
using PollKeyboardMouseFn = uint32_t (*)(void*, uint32_t,
                                         EmbeddedHostInputState*);
using ConsumeMouseLookFn = uint32_t (*)(void*, uint32_t, EmbeddedMouseLook*);
using SettingsConfigureFn = uint32_t (*)(void*, const char*, const char*, const char*, uint32_t);
using SettingsPollFn = uint32_t (*)(void*, char*, uint32_t);
using SettingsSavedFn = uint32_t (*)(void*, const char*, const char*);
using SettingsOverlayOpenFn = uint32_t (*)(void*);
using TestPadButtonsFn = uint32_t (*)(void*);
using CloseRequestedFn = uint32_t (*)(void*);
using XmaSetupFn = uint32_t (*)(void*, uint32_t);
using XmaShutdownFn = void (*)(void*);
using XmaAllocateFn = uint32_t (*)(void*);
using SetMenuStateFn = void (*)(void*, uint32_t);
using GpuFrameBusyFn = uint32_t (*)(void*, uint32_t*, uint32_t, uint64_t*);
using XmaReleaseFn = void (*)(void*, uint32_t);
using XmaReadMmioFn = uint32_t (*)(void*, uint32_t);
using XmaWriteMmioFn = void (*)(void*, uint32_t, uint32_t);

struct GraphicsAdapter {
    HMODULE module{};
    void* gpu{};
    DestroyFn destroy{};
    ReadMmioFn read_mmio{};
    WriteMmioFn write_mmio{};
    SetInterruptFn set_interrupt{};
    InitializeRingFn initialize_ring{};
    EnableReadPointerWritebackFn enable_read_pointer_writeback{};
    NotifyPhysicalWriteFn notify_physical_write{};
    InterruptTimingTokenFn interrupt_timing_token{};
    InterruptCompletedFn interrupt_completed{};
    FrameBoundaryFn frame_boundary{};
    GuestSwapFn guest_swap{};
    FrameStartFn frame_start{};
    InputSampleFn input_sample{};
    InterruptTimingRecordFn interrupt_timing_record{};
    NoteInputTransitionFn note_input_transition{};
    NoteInputDeviceFn note_input_device{};
    GetPromptLabelsFn get_prompt_labels{};
    GetManualCaptureFn get_manual_capture{};
    GetPcSettingsFn get_pc_settings{};
    PollKeyboardMouseFn poll_keyboard_mouse{};
    ConsumeMouseLookFn consume_mouse_look{};
    SettingsConfigureFn settings_configure{};
    SettingsPollFn settings_poll{};
    SettingsSavedFn settings_saved{};
    SettingsOverlayOpenFn settings_overlay_open{};
    TestPadButtonsFn test_pad_buttons{};
    CloseRequestedFn close_requested{};
    SetMenuStateFn set_menu_state{};
    GpuFrameBusyFn gpu_frame_busy{};
    XmaSetupFn xma_setup{};
    XmaShutdownFn xma_shutdown{};
    XmaAllocateFn xma_allocate{};
    XmaReleaseFn xma_release{};
    XmaReadMmioFn xma_read_mmio{};
    XmaWriteMmioFn xma_write_mmio{};
    uint8_t* virtual_membase{};
    uint8_t* physical_membase{};
    std::atomic<uint32_t> ring_physical_base{};
    std::atomic<uint32_t> ring_dword_count{};
    std::atomic<uint32_t> captured_write_pointer{};
    std::atomic<uint32_t> read_pointer_writeback{};
    std::atomic<uint32_t> last_read_pointer{UINT32_MAX};
    std::atomic<bool> active{};
    std::atomic<bool> xma_active{};
    std::atomic<bool> keyboard_mouse_enabled{};
    std::atomic<uint32_t> keyboard_mouse_user_index{};
};

GraphicsAdapter adapter;
std::optional<RuntimeGraphicsPcConfig> graphics_pc_config;
std::string graphics_cache_root_utf8;
std::string graphics_screenshot_root_utf8;
uint32_t graphics_cache_title_id{};
std::atomic<uint32_t> system_command_buffer_gpu_identifier_address{};
std::atomic<bool> graphics_clock_gating_enabled{};

struct PendingInterrupt {
    uint32_t callback;
    uint32_t source;
    uint32_t cpu;
    uint32_t callback_data;
    uint64_t timing_token{};
    uint64_t enqueue_tick{};
};
std::mutex interrupt_mutex;
std::condition_variable interrupt_wake;
std::deque<PendingInterrupt> pending_interrupts;
std::atomic<uint32_t>& pending_interrupt_count = g_runtime_pending_interrupt_count;
// ReXGlue's FunctionDispatcher::ExecuteInterrupt holds its global critical
// region for the complete callback. Embedded delivery must provide the same
// exclusion because vblank and command-processor notifications can otherwise
// enter the title callback concurrently on different guest threads.
std::recursive_mutex interrupt_dispatch_mutex;
thread_local bool delivering_interrupt;
std::atomic<uint64_t> interrupt_delivery_count{};
std::atomic<uint64_t> interrupt_queue_trace_count{};
std::atomic<bool> interrupt_worker_started{};
std::atomic<uint32_t> interrupt_worker_thread_id{};
std::atomic<uint64_t> ring_submission_count{};
std::atomic<uint64_t> mmio_read_trace_count{};
std::atomic<uint64_t> ring_write_trace_count{};
std::atomic<uint64_t> read_pointer_trace_count{};
std::atomic<uint64_t> xma_mmio_read_trace_count{};
std::atomic<uint64_t> xma_mmio_write_trace_count{};
std::atomic<uint64_t> nan_physical_write_count{};
GuestPhysicalWriteTracker& pending_guest_physical_writes = g_guest_physical_writes;
std::atomic<bool> nan_physical_write_snapshot_captured{};
std::array<std::atomic<uint32_t>, 8> watched_float_constant_sources{};
std::atomic<uint64_t> float_constant_nan_write_count{};
std::atomic<bool> float_constant_nan_producer_captured{};
std::array<std::atomic<uint32_t>, 8> watched_transform_sources{};
std::atomic<uint64_t> transform_nan_write_count{};
std::array<std::atomic<uint32_t>, 8> watched_transform_input_sources{};
std::atomic<uint64_t> transform_input_nan_write_count{};
std::mutex ring_trace_mutex;
std::mutex nan_physical_write_trace_mutex;
std::mutex float_constant_nan_write_trace_mutex;
std::mutex transform_nan_write_trace_mutex;
std::mutex transform_input_nan_write_trace_mutex;

constexpr bool ShouldTraceLiveness(uint64_t sequence,
                                   uint64_t startup_limit = 64) noexcept {
    return sequence <= startup_limit || (sequence & (sequence - 1)) == 0;
}

class InterruptDeliveryScope {
public:
    InterruptDeliveryScope() { delivering_interrupt = true; }
    ~InterruptDeliveryScope() { delivering_interrupt = false; }
    InterruptDeliveryScope(const InterruptDeliveryScope&) = delete;
    InterruptDeliveryScope& operator=(const InterruptDeliveryScope&) = delete;
};

template <typename T>
T Resolve(HMODULE module, const char* name) {
    auto* address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(std::string("missing ReXGlue export: ") + name);
    return reinterpret_cast<T>(address);
}

template <typename T>
T ResolveOptional(HMODULE module, const char* name) noexcept {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

uint32_t LoadPhysicalWord(uint32_t address) {
    const auto* word = reinterpret_cast<const volatile uint32_t*>(adapter.physical_membase + address);
    return __builtin_bswap32(*word);
}

void TraceReadPointerWriteback() {
    const uint32_t address = adapter.read_pointer_writeback.load(std::memory_order_acquire);
    if (!address || address > 0x1FFFFFFCu || !adapter.physical_membase) return;
    const uint32_t value = LoadPhysicalWord(address);
    const uint32_t previous = adapter.last_read_pointer.exchange(value, std::memory_order_relaxed);
    if (value != previous) {
        const uint64_t sequence =
            read_pointer_trace_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ShouldTraceLiveness(sequence)) {
            std::cout << "GPU_RPTR_OBSERVED sequence=" << sequence
                      << " address=0x" << std::hex << address
                      << " value=0x" << value << std::dec << '\n';
        }
    }
}

void TraceIndirectBuffer(uint64_t submission, uint32_t physicalAddress, uint32_t dwordCount,
                         uint32_t depth) {
    constexpr uint32_t kMaximumTraceDepth = 4;
    constexpr uint32_t kMaximumTracedDwords = 4096;
    if (!dwordCount || dwordCount > kMaximumTracedDwords || depth > kMaximumTraceDepth ||
        physicalAddress >= 0x20000000u ||
        uint64_t(physicalAddress) + uint64_t(dwordCount) * 4 > 0x20000000ull) {
        std::cout << "GPU_INDIRECT_BUFFER_INVALID submission=" << submission << " depth="
                  << depth << " address=0x" << std::hex << physicalAddress << std::dec
                  << " dwords=" << dwordCount << '\n';
        return;
    }

    std::cout << "GPU_INDIRECT_BUFFER submission=" << submission << " depth=" << depth
              << " address=0x" << std::hex << physicalAddress << std::dec
              << " dwords=" << dwordCount << '\n';
    uint32_t cursor{};
    while (cursor < dwordCount) {
        const uint32_t header = LoadPhysicalWord(physicalAddress + cursor * 4);
        const uint32_t type = header >> 30;
        uint32_t payloadDwords{};
        uint32_t totalDwords{1};
        uint32_t opcode{};
        if (header != 0) {
            if (type == 0) {
                payloadDwords = ((header >> 16) & 0x3FFFu) + 1;
                totalDwords += payloadDwords;
            } else if (type == 1) {
                payloadDwords = 2;
                totalDwords = 3;
            } else if (type == 3) {
                opcode = (header >> 8) & 0x7Fu;
                payloadDwords = ((header >> 16) & 0x3FFFu) + 1;
                totalDwords += payloadDwords;
            }
        }
        const uint32_t remaining = dwordCount - cursor;
        const bool complete = totalDwords <= remaining;
        std::cout << "GPU_PM4_INDIRECT_PACKET submission=" << submission << " depth=" << depth
                  << " buffer_index=0x" << std::hex << cursor << " header=0x" << header
                  << " type=0x" << type;
        if (type == 3) std::cout << " opcode=0x" << opcode;
        std::cout << std::dec << " payload_dwords=" << payloadDwords
                  << " total_dwords=" << totalDwords << " complete=" << complete << '\n';
        const uint32_t wordsToTrace = complete ? totalDwords : remaining;
        for (uint32_t index = 0; index < wordsToTrace; ++index) {
            std::cout << "GPU_INDIRECT_WORD submission=" << submission << " depth=" << depth
                      << " buffer_index=0x" << std::hex << (cursor + index) << " value=0x"
                      << LoadPhysicalWord(physicalAddress + (cursor + index) * 4) << std::dec
                      << '\n';
        }
        if (complete && type == 3 && opcode == 0x3Fu && payloadDwords >= 2 &&
            depth < kMaximumTraceDepth) {
            const uint32_t nestedAddress =
                LoadPhysicalWord(physicalAddress + (cursor + 1) * 4) & 0x1FFFFFFFu;
            const uint32_t nestedDwords =
                LoadPhysicalWord(physicalAddress + (cursor + 2) * 4) & 0xFFFFFu;
            TraceIndirectBuffer(submission, nestedAddress, nestedDwords, depth + 1);
        }
        cursor += wordsToTrace;
        if (!complete) break;
    }
}

void TraceRingSubmission(uint32_t writePointer) {
    std::lock_guard<std::mutex> lock(ring_trace_mutex);
    const uint32_t ringBase = adapter.ring_physical_base.load(std::memory_order_acquire);
    const uint32_t dwordCount = adapter.ring_dword_count.load(std::memory_order_acquire);
    if (!adapter.physical_membase || !dwordCount || (dwordCount & (dwordCount - 1))) return;
    const uint32_t mask = dwordCount - 1;
    uint32_t cursor = adapter.captured_write_pointer.load(std::memory_order_relaxed) & mask;
    const uint32_t end = writePointer & mask;
    const uint32_t submitted = (end - cursor) & mask;
    const uint64_t submission = ring_submission_count.fetch_add(1, std::memory_order_relaxed) + 1;
    // The first 207 complete submissions and their nested PM4 words are
    // preserved in logs/m6d_scene_field_watch.stdout.log. Keep a small
    // startup sample and logarithmic liveness summaries now; decoding every
    // word again is purely observational and materially delays guest frames.
    constexpr uint64_t kDetailedSubmissionLimit = 4;
    const bool traceSummary = submission <= 16 || (submission & (submission - 1)) == 0;
    if (traceSummary) {
        std::cout << "GPU_RING_SUBMISSION ordinal=" << submission << " old_wptr=0x" << std::hex
                  << cursor << " new_wptr=0x" << end << std::dec << " dwords=" << submitted
                  << " detail=" << (submission <= kDetailedSubmissionLimit) << '\n';
    }
    if (submission > kDetailedSubmissionLimit) {
        adapter.captured_write_pointer.store(end, std::memory_order_release);
        return;
    }

    uint32_t remaining = submitted;
    while (remaining) {
        const uint32_t packetIndex = cursor;
        const uint32_t header = LoadPhysicalWord(ringBase + packetIndex * 4);
        const uint32_t type = header >> 30;
        uint32_t payloadDwords{};
        uint32_t totalDwords{1};
        uint32_t opcode{};
        if (header != 0) {
            if (type == 0) {
                payloadDwords = ((header >> 16) & 0x3FFFu) + 1;
                totalDwords += payloadDwords;
            } else if (type == 1) {
                payloadDwords = 2;
                totalDwords = 3;
            } else if (type == 3) {
                opcode = (header >> 8) & 0x7Fu;
                payloadDwords = ((header >> 16) & 0x3FFFu) + 1;
                totalDwords += payloadDwords;
            }
        }
        const bool complete = totalDwords <= remaining;
        std::cout << "GPU_PM4_PACKET submission=" << submission << " ring_index=0x" << std::hex
                  << packetIndex << " header=0x" << header << " type=0x" << type;
        if (type == 3) std::cout << " opcode=0x" << opcode;
        std::cout << std::dec << " payload_dwords=" << payloadDwords
                  << " total_dwords=" << totalDwords << " complete=" << complete << '\n';
        const uint32_t wordsToTrace = complete ? totalDwords : remaining;
        for (uint32_t index = 0; index < wordsToTrace; ++index) {
            const uint32_t ringIndex = (cursor + index) & mask;
            std::cout << "GPU_RING_WORD submission=" << submission << " ring_index=0x"
                      << std::hex << ringIndex << " value=0x"
                      << LoadPhysicalWord(ringBase + ringIndex * 4) << std::dec << '\n';
        }
        if (complete && type == 3 && opcode == 0x3Fu && payloadDwords >= 2) {
            const uint32_t indirectAddress =
                LoadPhysicalWord(ringBase + ((cursor + 1) & mask) * 4) & 0x1FFFFFFFu;
            const uint32_t indirectDwords =
                LoadPhysicalWord(ringBase + ((cursor + 2) & mask) * 4) & 0xFFFFFu;
            TraceIndirectBuffer(submission, indirectAddress, indirectDwords, 1);
        }
        cursor = (cursor + wordsToTrace) & mask;
        remaining -= wordsToTrace;
        if (!complete) break;
    }
    adapter.captured_write_pointer.store(end, std::memory_order_release);
}

uint32_t ReadGpuMmio(void*, uint32_t address) {
    const uint32_t value = adapter.read_mmio(adapter.gpu, address);
    const uint64_t sequence = mmio_read_trace_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence <= 64 || (sequence & (sequence - 1)) == 0) {
        std::cout << "GPU_MMIO_READ sequence=" << sequence << " address=0x" << std::hex
                  << address << " value=0x" << value
                  << " pc=0x" << RuntimeActiveGuestFunction()
                  << " lr=0x" << RuntimeActiveGuestLr() << std::dec
                  << " thread=" << CurrentGuestThreadId() << '\n';
    }
    return value;
}

void WriteGpuMmio(void*, uint32_t address, uint32_t value) {
    // A Xenon CPU publishes command and resource memory before advancing GPU
    // MMIO state (most importantly CP_RB_WPTR). Drain page-precise CPU dirties
    // at that ordering boundary instead of taking ReXGlue's global cache lock
    // for every scalar store that merely happens to use a physical alias.
    RuntimeFlushGuestPhysicalWrites();
    const bool ringWrite = address == 0x7FC80714u;
    const uint64_t sequence = ringWrite
        ? ring_write_trace_count.fetch_add(1, std::memory_order_relaxed) + 1
        : 0;
    // Full post-Vd MMIO evidence is preserved in the earlier M6B/M6C logs.
    // CP_RB_WPTR is now a high-frequency liveness signal, so retain a startup
    // sample and powers of two while continuing to report every other register.
    if (!ringWrite || ShouldTraceLiveness(sequence)) {
        std::cout << "GPU_MMIO_WRITE";
        if (ringWrite) std::cout << " sequence=" << sequence;
        std::cout << " address=0x" << std::hex << address << " value=0x" << value
                  << " pc=0x" << RuntimeActiveGuestFunction()
                  << " lr=0x" << RuntimeActiveGuestLr()
                  << std::dec << " thread=" << CurrentGuestThreadId() << '\n';
    }
    if (address == 0x7FC80714u) {
        TraceReadPointerWriteback();
        TraceRingSubmission(value);
    }
    adapter.write_mmio(adapter.gpu, address, value);
}

uint32_t ReadXmaMmio(void*, uint32_t address) {
    const uint32_t value = adapter.xma_read_mmio(adapter.gpu, address);
    const uint64_t sequence =
        xma_mmio_read_trace_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ShouldTraceLiveness(sequence)) {
        std::cout << "XMA_MMIO_READ sequence=" << sequence
                  << " address=0x" << std::hex << address
                  << " value=0x" << value
                  << " pc=0x" << RuntimeActiveGuestFunction()
                  << " lr=0x" << RuntimeActiveGuestLr()
                  << std::dec << " thread=" << CurrentGuestThreadId() << '\n';
    }
    return value;
}

void WriteXmaMmio(void*, uint32_t address, uint32_t value) {
    RuntimeFlushGuestPhysicalWrites();
    const uint64_t sequence =
        xma_mmio_write_trace_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ShouldTraceLiveness(sequence)) {
        std::cout << "XMA_MMIO_WRITE sequence=" << sequence
                  << " address=0x" << std::hex << address
                  << " value=0x" << value
                  << " pc=0x" << RuntimeActiveGuestFunction()
                  << " lr=0x" << RuntimeActiveGuestLr()
                  << std::dec << " thread=" << CurrentGuestThreadId() << '\n';
    }
    adapter.xma_write_mmio(adapter.gpu, address, value);
}

void EmbeddedInterrupt(void*, uint32_t callback, uint32_t source, uint32_t cpu,
                        uint32_t callback_data) {
    QueueRuntimeGraphicsInterrupt(callback, source, cpu, callback_data);
}

void EnsureRuntimeGraphicsInterruptWorker(uint32_t reportedEntry) {
    bool expected = false;
    if (!interrupt_worker_started.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    try {
        const auto worker = StartGuestHostThread(
            adapter.virtual_membase, 0x10000u, reportedEntry,
            [](PPCContext& context, uint8_t* base) -> uint32_t {
                const uint32_t threadId = CurrentGuestThreadId();
                interrupt_worker_thread_id.store(threadId, std::memory_order_release);
                // Xenia dispatches GPU interrupts on the GPU command thread's
                // own guest ThreadState and selects the requested hardware CPU.
                // Give this runtime-owned command thread a persistent CPU-2
                // identity; individual PM4 masks may temporarily select another
                // valid CPU while a callback is executing.
                PPC_STORE_U32(context.r13.u32 + 0x10Cu, 2u);
                std::cout << "GPU_INTERRUPT_WORKER_READY thread=" << threadId
                          << " cpu=2\n";
                while (!GuestRuntimeStopRequested()) {
                    {
                        std::unique_lock<std::mutex> lock(interrupt_mutex);
                        interrupt_wake.wait_for(lock, std::chrono::milliseconds(10), [] {
                            return !pending_interrupts.empty() ||
                                   GuestRuntimeStopRequested() ||
                                   !adapter.active.load(std::memory_order_acquire);
                        });
                        if (GuestRuntimeStopRequested() ||
                            !adapter.active.load(std::memory_order_acquire)) {
                            break;
                        }
                        if (pending_interrupts.empty()) continue;
                    }
                    DeliverRuntimeGraphicsInterrupts(context, base);
                }
                return 0;
            });
        std::cout << "GPU_INTERRUPT_WORKER_CREATED thread=" << worker.threadId
                  << " handle=0x" << std::hex << worker.handle
                  << " object=0x" << worker.guestObject << std::dec << '\n';
    } catch (...) {
        interrupt_worker_thread_id.store(0, std::memory_order_release);
        interrupt_worker_started.store(false, std::memory_order_release);
        throw;
    }
}

bool GuestAliasToPhysical(uint32_t address, uint32_t length, uint32_t& physical) {
    return RuntimeCanonicalGuestPhysicalRange(address, length, physical);
}
}

bool RuntimeGraphicsGuestPhysicalRange(uint32_t address, uint32_t bytes,
                                      uint32_t& physical) noexcept {
    return GuestAliasToPhysical(address, bytes, physical);
}

bool RuntimeGraphicsCopyPacketBytes(uint32_t guest_address, uint32_t bytes,
                                    void* destination, uint32_t& physical) noexcept {
    physical = UINT32_MAX;
    uint32_t source = 0;
    if (!destination || !adapter.physical_membase || !bytes || bytes > 256u * 1024u ||
        (guest_address & 3u) || (bytes & 3u) ||
        !GuestAliasToPhysical(guest_address, bytes, source)) return false;
    std::memcpy(destination, adapter.physical_membase + source, bytes);
    physical = source;
    return true;
}

void RuntimeWatchGpuFloatConstantSource(uint32_t guestAddress) noexcept {
    uint32_t physical = 0;
    if (!GuestAliasToPhysical(guestAddress, 48u * 4u, physical)) return;
    for (auto& slot : watched_float_constant_sources) {
        if (slot.load(std::memory_order_acquire) == physical) return;
    }
    for (auto& slot : watched_float_constant_sources) {
        uint32_t expected = 0;
        if (slot.compare_exchange_strong(expected, physical, std::memory_order_acq_rel)) {
            std::cout << "GPU_FLOAT_CONSTANT_SOURCE_WATCH guest=0x" << std::hex
                      << guestAddress << " physical=0x" << physical << std::dec
                      << " bytes=192\n";
            return;
        }
    }
}

void RuntimeWatchGpuTransformSource(uint32_t guestAddress) noexcept {
    uint32_t physical = 0;
    if (!GuestAliasToPhysical(guestAddress, 16u * 4u, physical)) return;
    for (auto& slot : watched_transform_sources) {
        if (slot.load(std::memory_order_acquire) == physical) return;
    }
    for (auto& slot : watched_transform_sources) {
        uint32_t expected = 0;
        if (slot.compare_exchange_strong(expected, physical, std::memory_order_acq_rel)) {
            std::cout << "GPU_TRANSFORM_SOURCE_WATCH guest=0x" << std::hex
                      << guestAddress << " physical=0x" << physical << std::dec
                      << " bytes=64\n";
            return;
        }
    }
}

void RuntimeWatchGpuTransformInputSource(uint32_t guestAddress) noexcept {
    uint32_t physical = 0;
    if (!GuestAliasToPhysical(guestAddress, 16u * 4u, physical)) return;
    for (auto& slot : watched_transform_input_sources) {
        if (slot.load(std::memory_order_acquire) == physical) return;
    }
    for (auto& slot : watched_transform_input_sources) {
        uint32_t expected = 0;
        if (slot.compare_exchange_strong(expected, physical, std::memory_order_acq_rel)) {
            std::cout << "GPU_TRANSFORM_INPUT_SOURCE_WATCH guest=0x" << std::hex
                      << guestAddress << " physical=0x" << physical << std::dec
                      << " bytes=64\n";
            return;
        }
    }
}

void ConfigureRuntimeGraphicsPcConfig(
    const RuntimePcConfigSnapshot& config,
    const std::filesystem::path& asset_root) {
    if (adapter.active.load(std::memory_order_acquire)) {
        throw std::runtime_error(
            "PC graphics configuration cannot change after GPU initialization");
    }
    graphics_pc_config.emplace(config, asset_root);
}

void ConfigureRuntimeGraphicsCache(const std::filesystem::path& cache_root,
                                   uint32_t title_id) {
    if (adapter.active.load(std::memory_order_acquire)) {
        throw std::runtime_error(
            "PC graphics cache cannot change after GPU initialization");
    }
    if (cache_root.empty() || !title_id) {
        throw std::runtime_error("PC graphics cache identity is incomplete");
    }
    graphics_cache_root_utf8 = cache_root.u8string();
    graphics_cache_title_id = title_id;
}

void ConfigureRuntimeGraphicsScreenshotRoot(
    const std::filesystem::path& screenshot_root) {
    if (adapter.active.load(std::memory_order_acquire)) {
        throw std::runtime_error(
            "PC screenshot root cannot change after GPU initialization");
    }
    if (screenshot_root.empty() || !screenshot_root.is_absolute()) {
        throw std::runtime_error("PC screenshot root must be absolute");
    }
    graphics_screenshot_root_utf8 = screenshot_root.u8string();
}

bool InitializeRuntimeGraphics(uint8_t* guest_virtual_base) {
    if (adapter.active.load(std::memory_order_acquire)) return true;
    if (!guest_virtual_base) return false;

    try {
        if (!graphics_pc_config) {
            throw std::runtime_error("owned startup graphics configuration is missing");
        }
        adapter.module = LoadLibraryW(L"rexgpu-xenos.dll");
        if (!adapter.module) {
            throw std::runtime_error("unable to load rexgpu-xenos.dll error=" +
                                     std::to_string(GetLastError()));
        }
        const auto create = Resolve<CreateFn>(adapter.module, "rex_gpu_embedded_create");
        adapter.destroy = Resolve<DestroyFn>(adapter.module, "rex_gpu_embedded_destroy");
        adapter.read_mmio = Resolve<ReadMmioFn>(adapter.module, "rex_gpu_embedded_read_mmio");
        adapter.write_mmio = Resolve<WriteMmioFn>(adapter.module, "rex_gpu_embedded_write_mmio");
        adapter.set_interrupt =
            Resolve<SetInterruptFn>(adapter.module, "rex_gpu_embedded_set_interrupt");
        adapter.initialize_ring =
            Resolve<InitializeRingFn>(adapter.module, "rex_gpu_embedded_initialize_ring");
        adapter.enable_read_pointer_writeback = Resolve<EnableReadPointerWritebackFn>(
            adapter.module, "rex_gpu_embedded_enable_read_pointer_writeback");
        adapter.notify_physical_write = Resolve<NotifyPhysicalWriteFn>(
            adapter.module, "rex_gpu_embedded_notify_physical_write");
        adapter.note_input_transition = ResolveOptional<NoteInputTransitionFn>(
            adapter.module, "rex_gpu_embedded_note_input_transition");
        adapter.note_input_device = ResolveOptional<NoteInputDeviceFn>(
            adapter.module, "rex_gpu_embedded_note_input_device");
        adapter.get_prompt_labels = ResolveOptional<GetPromptLabelsFn>(
            adapter.module, "rex_gpu_embedded_get_prompt_labels");
        adapter.get_manual_capture = ResolveOptional<GetManualCaptureFn>(
            adapter.module, "rex_gpu_embedded_get_manual_capture");
        adapter.interrupt_timing_token = ResolveOptional<InterruptTimingTokenFn>(
            adapter.module, "rex_gpu_embedded_interrupt_timing_token");
        adapter.interrupt_completed = ResolveOptional<InterruptCompletedFn>(
            adapter.module, "rex_gpu_embedded_interrupt_completed");
        adapter.frame_boundary = ResolveOptional<FrameBoundaryFn>(
            adapter.module, "rex_gpu_embedded_frame_boundary");
        adapter.interrupt_timing_record = ResolveOptional<InterruptTimingRecordFn>(
            adapter.module, "rex_gpu_embedded_interrupt_timing_record");
        adapter.get_pc_settings = Resolve<GetPcSettingsFn>(
            adapter.module, "rex_gpu_embedded_get_pc_settings");
        adapter.poll_keyboard_mouse = Resolve<PollKeyboardMouseFn>(
            adapter.module, "rex_gpu_embedded_poll_keyboard_mouse");
        adapter.consume_mouse_look = ResolveOptional<ConsumeMouseLookFn>(
            adapter.module, "rex_gpu_embedded_consume_mouse_look");
        adapter.settings_configure = ResolveOptional<SettingsConfigureFn>(
            adapter.module, "rex_gpu_embedded_settings_configure");
        adapter.settings_poll = ResolveOptional<SettingsPollFn>(
            adapter.module, "rex_gpu_embedded_settings_poll");
        adapter.settings_saved = ResolveOptional<SettingsSavedFn>(
            adapter.module, "rex_gpu_embedded_settings_saved");
        adapter.settings_overlay_open = ResolveOptional<SettingsOverlayOpenFn>(
            adapter.module, "rex_gpu_embedded_settings_overlay_open");
        adapter.test_pad_buttons = ResolveOptional<TestPadButtonsFn>(
            adapter.module, "rex_gpu_embedded_test_pad_buttons");
        adapter.close_requested = ResolveOptional<CloseRequestedFn>(
            adapter.module, "rex_gpu_embedded_close_requested");
        adapter.set_menu_state = ResolveOptional<SetMenuStateFn>(
            adapter.module, "rex_gpu_embedded_set_menu_state");
        adapter.guest_swap = ResolveOptional<GuestSwapFn>(
            adapter.module, "rex_gpu_embedded_guest_swap");
        adapter.frame_start = ResolveOptional<FrameStartFn>(
            adapter.module, "rex_gpu_embedded_frame_start");
        adapter.input_sample = ResolveOptional<InputSampleFn>(
            adapter.module, "rex_gpu_embedded_input_sample");
        adapter.gpu_frame_busy = ResolveOptional<GpuFrameBusyFn>(
            adapter.module, "rex_gpu_embedded_get_gpu_frame_busy_us");
        adapter.xma_setup =
            Resolve<XmaSetupFn>(adapter.module, "rex_gpu_embedded_xma_setup");
        adapter.xma_shutdown =
            Resolve<XmaShutdownFn>(adapter.module, "rex_gpu_embedded_xma_shutdown");
        adapter.xma_allocate =
            Resolve<XmaAllocateFn>(adapter.module, "rex_gpu_embedded_xma_allocate");
        adapter.xma_release =
            Resolve<XmaReleaseFn>(adapter.module, "rex_gpu_embedded_xma_release");
        adapter.xma_read_mmio =
            Resolve<XmaReadMmioFn>(adapter.module, "rex_gpu_embedded_xma_read_mmio");
        adapter.xma_write_mmio =
            Resolve<XmaWriteMmioFn>(adapter.module, "rex_gpu_embedded_xma_write_mmio");

        EmbeddedGpuCreateInfo info{};
        info.struct_size = sizeof(info);
        info.backend = "d3d12";
        info.virtual_membase = guest_virtual_base;
        // The runtime's A-segment view is the authoritative 512 MiB physical
        // backing and aliases all title physical allocations.
        info.physical_membase = guest_virtual_base + 0xA0000000ull;
        adapter.physical_membase = info.physical_membase;
        adapter.virtual_membase = guest_virtual_base;
        info.refresh_rate_hz = darkness::guest_video_mode::kRefreshRateHz;
        info.interrupt_callback = EmbeddedInterrupt;
        info.config_path_utf8 = graphics_pc_config->originUtf8().c_str();
        info.asset_root_utf8 = graphics_pc_config->assetRootUtf8().c_str();
        info.config_contents_utf8 = graphics_pc_config->snapshot().contents().data();
        info.config_contents_size = graphics_pc_config->snapshot().contents().size();
        info.config_present = graphics_pc_config->snapshot().present() ? 1 : 0;
        if (RuntimeGuestSourceCoordinator().TrackingEnabled()) {
            info.host_writes.begin = runtime_host_writes::Begin;
            info.host_writes.end = runtime_host_writes::End;
            info.camera_packets.context = info.physical_membase;
            info.camera_packets.copy = RuntimeCopyOwnedCameraPacket;
        }
        info.cache_root_utf8 = graphics_cache_root_utf8.empty()
            ? nullptr
            : graphics_cache_root_utf8.c_str();
        info.title_id = graphics_cache_title_id;
        info.shader_storage_blocking = 1;
        info.screenshot_root_utf8 = graphics_screenshot_root_utf8.empty()
            ? nullptr
            : graphics_screenshot_root_utf8.c_str();
        adapter.gpu = create(kEmbeddedGpuAbiVersion, &info);
        if (!adapter.gpu) throw std::runtime_error("embedded ReXGlue GPU creation failed");
        EmbeddedGpuPcSettings pcSettings{};
        pcSettings.struct_size = sizeof(pcSettings);
        if (!adapter.get_pc_settings(adapter.gpu, &pcSettings)) {
            throw std::runtime_error("embedded ReXGlue PC settings query failed");
        }
        uint64_t initialCaptureGeneration{};
        const bool manualCameraCapture = adapter.get_manual_capture &&
            adapter.get_manual_capture(adapter.gpu, &initialCaptureGeneration);
        ConfigureRuntimeCamera(pcSettings.gameplay_fov_degrees,
                               pcSettings.trace_camera_state != 0,
                               manualCameraCapture);
        adapter.keyboard_mouse_user_index.store(
            pcSettings.keyboard_mouse_user_index, std::memory_order_release);
        adapter.keyboard_mouse_enabled.store(
            pcSettings.keyboard_mouse_enabled != 0, std::memory_order_release);
        if (!RegisterRuntimeMmioRange(kXenosMmioFirst, kXenosMmioLast, &adapter,
                                      ReadGpuMmio, WriteGpuMmio)) {
            throw std::runtime_error("unable to register Xenos MMIO range");
        }
        adapter.active.store(true, std::memory_order_release);
        interrupt_delivery_count.store(0, std::memory_order_relaxed);
        interrupt_queue_trace_count.store(0, std::memory_order_relaxed);
        pending_interrupt_count.store(0, std::memory_order_release);
        ring_submission_count.store(0, std::memory_order_relaxed);
        mmio_read_trace_count.store(0, std::memory_order_relaxed);
        ring_write_trace_count.store(0, std::memory_order_relaxed);
        read_pointer_trace_count.store(0, std::memory_order_relaxed);
        xma_mmio_read_trace_count.store(0, std::memory_order_relaxed);
        xma_mmio_write_trace_count.store(0, std::memory_order_relaxed);
        std::cout << "GRAPHICS_EMBEDDED_READY backend=d3d12 mmio_first=0x" << std::hex
                  << kXenosMmioFirst << " mmio_last=0x" << kXenosMmioLast
                  << " physical_base=0x" << reinterpret_cast<uintptr_t>(info.physical_membase)
                  << std::dec << " second_guest_memory=0 cache_root="
                  << graphics_cache_root_utf8 << " cache_title=0x" << std::hex
                  << graphics_cache_title_id << std::dec << '\n';
        return true;
    } catch (const std::exception& error) {
        std::cerr << "GRAPHICS_INITIALIZATION_FAILED " << error.what() << '\n';
        ShutdownRuntimeGraphics();
        return false;
    }
}

void ShutdownRuntimeGraphics() noexcept {
    adapter.active.store(false, std::memory_order_release);
    adapter.xma_active.store(false, std::memory_order_release);
    adapter.keyboard_mouse_enabled.store(false, std::memory_order_release);
    system_command_buffer_gpu_identifier_address.store(0, std::memory_order_release);
    graphics_clock_gating_enabled.store(false, std::memory_order_release);
    if (adapter.gpu && adapter.destroy) adapter.destroy(adapter.gpu);
    adapter.gpu = nullptr;
    adapter.virtual_membase = nullptr;
    adapter.physical_membase = nullptr;
    adapter.ring_physical_base.store(0, std::memory_order_release);
    adapter.ring_dword_count.store(0, std::memory_order_release);
    adapter.captured_write_pointer.store(0, std::memory_order_release);
    adapter.read_pointer_writeback.store(0, std::memory_order_release);
    adapter.last_read_pointer.store(UINT32_MAX, std::memory_order_release);
    // The plugin is intentionally retained for process lifetime. ReXGlue owns
    // process-global registries whose teardown order is not part of this ABI.
    {
        std::lock_guard<std::mutex> lock(interrupt_mutex);
        pending_interrupts.clear();
        pending_interrupt_count.store(0, std::memory_order_release);
    }
    interrupt_wake.notify_all();
}

uint64_t RuntimeGraphicsCameraCaptureGeneration() noexcept {
    uint64_t generation{};
    if (adapter.active.load(std::memory_order_acquire) &&
        adapter.get_manual_capture) {
        adapter.get_manual_capture(adapter.gpu, &generation);
    }
    return generation;
}

bool RuntimeGraphicsKeyboardMouseEnabled(uint32_t user_index) noexcept {
    return adapter.active.load(std::memory_order_acquire) &&
           adapter.keyboard_mouse_enabled.load(std::memory_order_acquire) &&
           user_index == adapter.keyboard_mouse_user_index.load(
                             std::memory_order_acquire);
}

uint32_t RuntimeGraphicsKeyboardMouseUserIndex() noexcept {
    return adapter.keyboard_mouse_user_index.load(std::memory_order_acquire);
}

bool RuntimeGraphicsPollKeyboardMouse(
    uint32_t user_index, RuntimeGraphicsHostInputState& output) noexcept {
    if (!RuntimeGraphicsKeyboardMouseEnabled(user_index) ||
        !adapter.poll_keyboard_mouse || !adapter.gpu) {
        return false;
    }
    EmbeddedHostInputState state{};
    state.struct_size = sizeof(state);
    if (!adapter.poll_keyboard_mouse(adapter.gpu, user_index, &state)) {
        return false;
    }
    output.packetNumber = state.packet_number;
    output.buttons = state.buttons;
    output.leftTrigger = state.left_trigger;
    output.rightTrigger = state.right_trigger;
    output.thumbLX = state.thumb_lx;
    output.thumbLY = state.thumb_ly;
    output.thumbRX = state.thumb_rx;
    output.thumbRY = state.thumb_ry;
    return true;
}

bool RuntimeGraphicsConsumeMouseLook(uint32_t user_index,
                                     RuntimeGraphicsMouseLook& output) noexcept {
    output = {};
    if (!RuntimeGraphicsKeyboardMouseEnabled(user_index) ||
        !adapter.consume_mouse_look || !adapter.gpu) {
        return false;
    }
    EmbeddedMouseLook look{};
    look.struct_size = sizeof(look);
    if (!adapter.consume_mouse_look(adapter.gpu, user_index, &look)) {
        return false;
    }
    output.active = look.active != 0;
    output.dx = look.dx;
    output.dy = look.dy;
    output.sensitivity = look.sensitivity;
    output.invertY = look.invert_y != 0;
    return output.active;
}

bool RuntimeGraphicsSettingsConfigure(const std::string& schema, const std::string& saved,
                                      const std::string& defaults, bool persistence) noexcept {
    if (!adapter.active.load(std::memory_order_acquire) || !adapter.gpu ||
        !adapter.settings_configure) {
        return false;
    }
    return adapter.settings_configure(adapter.gpu, schema.c_str(), saved.c_str(),
                                      defaults.c_str(), persistence ? 1u : 0u) != 0;
}

bool RuntimeGraphicsSettingsPoll(std::string& changes) {
    changes.clear();
    if (!adapter.active.load(std::memory_order_acquire) || !adapter.gpu ||
        !adapter.settings_poll) {
        return false;
    }
    uint32_t length = adapter.settings_poll(adapter.gpu, nullptr, 0);
    for (int attempt = 0; length && attempt < 4; ++attempt) {
        std::string buffer(size_t(length) + 1, '\0');
        const uint32_t written =
            adapter.settings_poll(adapter.gpu, buffer.data(), uint32_t(buffer.size()));
        if (written && written < buffer.size()) {
            buffer.resize(written);
            changes = std::move(buffer);
            return true;
        }
        length = written;  // grew meanwhile: retry with the new size
    }
    return false;
}

void RuntimeGraphicsSettingsSaved(const std::string& saved, const std::string& status) noexcept {
    if (!adapter.active.load(std::memory_order_acquire) || !adapter.gpu ||
        !adapter.settings_saved) {
        return;
    }
    adapter.settings_saved(adapter.gpu, saved.c_str(), status.c_str());
}

bool RuntimeGraphicsSettingsOverlayOpen() noexcept {
    return adapter.active.load(std::memory_order_acquire) && adapter.gpu &&
           adapter.settings_overlay_open && adapter.settings_overlay_open(adapter.gpu) != 0;
}

uint16_t RuntimeGraphicsTestPadButtons() noexcept {
    if (!adapter.active.load(std::memory_order_acquire) || !adapter.gpu ||
        !adapter.test_pad_buttons) {
        return 0;
    }
    return uint16_t(adapter.test_pad_buttons(adapter.gpu));
}

bool RuntimeGraphicsCloseRequested() noexcept {
    return adapter.active.load(std::memory_order_acquire) && adapter.gpu &&
           adapter.close_requested && adapter.close_requested(adapter.gpu) != 0;
}

void RuntimeGraphicsNoteInputTransition(int64_t host_performance_counter,
                                        int64_t host_performance_frequency,
                                        uint32_t packet_number,
                                        uint32_t buttons) noexcept {
    if (!adapter.active.load(std::memory_order_acquire) || !adapter.gpu ||
        !adapter.note_input_transition || host_performance_frequency <= 0) {
        return;
    }
    adapter.note_input_transition(adapter.gpu, host_performance_counter,
                                  host_performance_frequency, packet_number,
                                  buttons);
}

void RuntimeGraphicsNoteInputDevice(uint32_t device) noexcept {
    if (!adapter.active.load(std::memory_order_acquire) || !adapter.gpu ||
        !adapter.note_input_device) {
        return;
    }
    adapter.note_input_device(adapter.gpu, device);
}

uint32_t RuntimeGraphicsPromptLabels(char* buffer, uint32_t size) noexcept {
    if (buffer && size) buffer[0] = '\0';
    if (!adapter.active.load(std::memory_order_acquire) || !adapter.gpu ||
        !adapter.get_prompt_labels) {
        return 0;
    }
    return adapter.get_prompt_labels(adapter.gpu, buffer, size);
}

bool RuntimeGraphicsIsActive() noexcept {
    return adapter.active.load(std::memory_order_acquire);
}

bool RuntimeGraphicsInitializeXma(uint32_t context_array_guest_address) {
    if (!RuntimeGraphicsIsActive() || !context_array_guest_address) return false;
    if (adapter.xma_active.load(std::memory_order_acquire)) return true;
    if (!adapter.xma_setup(adapter.gpu, context_array_guest_address)) return false;
    if (!RegisterRuntimeMmioRange(kXmaMmioFirst, kXmaMmioLast, &adapter,
                                  ReadXmaMmio, WriteXmaMmio)) {
        throw std::runtime_error("unable to register XMA MMIO range");
    }
    adapter.xma_active.store(true, std::memory_order_release);
    std::cout << "XMA_HARDWARE_READY context_array=0x" << std::hex
              << context_array_guest_address << " mmio_first=0x" << kXmaMmioFirst
              << " mmio_last=0x" << kXmaMmioLast << std::dec
              << " shared_guest_memory=1\n";
    return true;
}

void RuntimeGraphicsShutdownXma() noexcept {
    if (!adapter.xma_active.exchange(false, std::memory_order_acq_rel)) return;
    if (adapter.gpu && adapter.xma_shutdown) adapter.xma_shutdown(adapter.gpu);
}

uint32_t RuntimeGraphicsAllocateXmaContext() {
    if (!adapter.xma_active.load(std::memory_order_acquire)) return 0;
    return adapter.xma_allocate(adapter.gpu);
}

bool RuntimeGraphicsReleaseXmaContext(uint32_t context_guest_address) {
    if (!adapter.xma_active.load(std::memory_order_acquire) ||
        !context_guest_address) return false;
    adapter.xma_release(adapter.gpu, context_guest_address);
    return true;
}

void RuntimeGraphicsSetClockGating(bool enabled) noexcept {
    // Host D3D12 power management remains owned by the display driver, but the
    // guest-visible Xenos policy transition is retained exactly for later
    // queries and shutdown/reinitialization sequencing.
    graphics_clock_gating_enabled.store(enabled, std::memory_order_release);
    std::cout << "GPU_CLOCK_GATING requested=" << (enabled ? 1 : 0)
              << " host_power_policy=driver_owned\n";
}

bool RuntimeGraphicsClockGatingEnabled() noexcept {
    return graphics_clock_gating_enabled.load(std::memory_order_acquire);
}

void RuntimeGraphicsSetInterruptCallback(uint32_t callback, uint32_t callback_data) {
    if (!RuntimeGraphicsIsActive()) throw std::runtime_error("graphics engine is not initialized");
    adapter.set_interrupt(adapter.gpu, callback, callback_data);
    if (callback) EnsureRuntimeGraphicsInterruptWorker(callback);
    std::cout << "GPU_INTERRUPT_CALLBACK callback=0x" << std::hex << callback
              << " data=0x" << callback_data << std::dec << '\n';
}

void RuntimeGraphicsSetSystemCommandBufferGpuIdentifierAddress(uint32_t address) noexcept {
    // This video-driver API is a setter, not a command submission. Retain the
    // guest address so the system-command-buffer path can publish its actual
    // GPU identifier when that path is dynamically reached. A null address
    // explicitly disconnects the publication target.
    system_command_buffer_gpu_identifier_address.store(address, std::memory_order_release);
    std::cout << "GPU_SYSTEM_COMMAND_BUFFER_IDENTIFIER_ADDRESS address=0x" << std::hex
              << address << std::dec << '\n';
}

uint32_t RuntimeGraphicsSystemCommandBufferGpuIdentifierAddress() noexcept {
    return system_command_buffer_gpu_identifier_address.load(std::memory_order_acquire);
}

void RuntimeGraphicsInitializeRingBuffer(uint32_t physical_address, uint32_t size_log2) {
    if (!RuntimeGraphicsIsActive()) throw std::runtime_error("graphics engine is not initialized");
    if (physical_address >= 0x20000000u || size_log2 > 28u)
        throw std::runtime_error("invalid graphics ring buffer");
    adapter.initialize_ring(adapter.gpu, physical_address, size_log2);
    adapter.ring_physical_base.store(physical_address, std::memory_order_release);
    adapter.ring_dword_count.store(uint32_t{1} << (size_log2 + 1), std::memory_order_release);
    adapter.captured_write_pointer.store(0, std::memory_order_release);
    std::cout << "GPU_RING_INITIALIZED base=0x" << std::hex << physical_address << std::dec
              << " size_log2=" << size_log2 << " bytes=" << (1ull << (size_log2 + 3)) << '\n';
}

void RuntimeGraphicsEnableReadPointerWriteback(uint32_t physical_address,
                                               uint32_t block_size_log2) {
    if (!RuntimeGraphicsIsActive()) throw std::runtime_error("graphics engine is not initialized");
    if (physical_address >= 0x20000000u)
        throw std::runtime_error("invalid ring read-pointer writeback address");
    adapter.enable_read_pointer_writeback(adapter.gpu, physical_address, block_size_log2);
    adapter.read_pointer_writeback.store(physical_address, std::memory_order_release);
    adapter.last_read_pointer.store(UINT32_MAX, std::memory_order_release);
    std::cout << "GPU_RPTR_WRITEBACK address=0x" << std::hex << physical_address << std::dec
              << " block_size_log2=" << block_size_log2 << '\n';
}

RuntimeGraphicsSwapCommandInfo RuntimeGraphicsBuildSwapCommand(
    uint8_t* base, uint32_t commandBuffer, uint32_t textureFetch,
    uint32_t systemBuffer, uint32_t systemToken, uint32_t frontbufferAddress,
    uint32_t textureFormatAddress, uint32_t colorSpaceAddress,
    uint32_t widthAddress, uint32_t heightAddress) {
    constexpr uint32_t kSystemBufferTag = 0xBEEF0000u;
    constexpr uint32_t kSystemToken = 0xBEEF0001u;
    constexpr uint32_t kTextureFetchRegister = 0x4800u;
    constexpr uint32_t kSwapOpcode = 0x64u;
    constexpr uint32_t kSwapSignature = 0x50415753u;  // memory::fourcc("SWAP")
    constexpr uint32_t kPacketType2 = 0x80000000u;
    constexpr uint32_t kCommandDwords = 64;
    if (!base || !commandBuffer || commandBuffer > UINT32_MAX - kCommandDwords * 4u ||
        !textureFetch || textureFetch > UINT32_MAX - 24u || !systemBuffer ||
        !frontbufferAddress || !textureFormatAddress || !colorSpaceAddress ||
        !widthAddress || !heightAddress) {
        throw std::runtime_error("VdSwap reached an invalid guest buffer contract");
    }
    if (PPC_LOAD_U32(systemBuffer) != kSystemBufferTag || systemToken != kSystemToken) {
        throw std::runtime_error("VdSwap system command-buffer tags do not match VdGetSystemCommandBuffer");
    }

    uint32_t fetch[6]{};
    for (uint32_t index = 0; index < 6; ++index) {
        fetch[index] = PPC_LOAD_U32(textureFetch + index * 4);
    }
    const uint32_t frontbufferVirtual = fetch[1] & 0xFFFFF000u;
    uint32_t frontbufferPhysical{};
    if (!frontbufferVirtual ||
        !GuestAliasToPhysical(frontbufferVirtual, 1, frontbufferPhysical)) {
        throw std::runtime_error("VdSwap frontbuffer is not in shared physical guest memory");
    }
    const uint32_t frontbufferArgument = PPC_LOAD_U32(frontbufferAddress);
    const uint32_t textureFormat = PPC_LOAD_U32(textureFormatAddress);
    const uint32_t colorSpace = PPC_LOAD_U32(colorSpaceAddress);
    const uint32_t width = PPC_LOAD_U32(widthAddress);
    const uint32_t height = PPC_LOAD_U32(heightAddress);
    const uint32_t fetchWidth = (fetch[2] & 0x1FFFu) + 1;
    const uint32_t fetchHeight = ((fetch[2] >> 13) & 0x1FFFu) + 1;
    const uint32_t fetchFormat = fetch[1] & 0x3Fu;
    if (frontbufferArgument != frontbufferVirtual || width != fetchWidth ||
        height != fetchHeight || textureFormat != fetchFormat || colorSpace != 0) {
        std::ostringstream message;
        message << "VdSwap frontbuffer contract mismatch virtual=0x" << std::hex
                << frontbufferVirtual << " argument=0x" << frontbufferArgument
                << " format=0x" << textureFormat << " fetch_format=0x" << fetchFormat
                << std::dec << " size=" << width << 'x' << height
                << " fetch_size=" << fetchWidth << 'x' << fetchHeight
                << " color_space=" << colorSpace;
        throw std::runtime_error(message.str());
    }
    if (textureFormat != 6u && textureFormat != 54u) {
        throw std::runtime_error("VdSwap reached an unsupported frontbuffer texture format");
    }

    fetch[1] = (fetch[1] & 0xFFFu) | (frontbufferPhysical & 0xFFFFF000u);
    uint32_t offset{};
    PPC_STORE_U32(commandBuffer + offset++ * 4,
                  ((6u - 1u) << 16) | kTextureFetchRegister);
    for (uint32_t word : fetch) PPC_STORE_U32(commandBuffer + offset++ * 4, word);
    PPC_STORE_U32(commandBuffer + offset++ * 4,
                  0xC0000000u | ((4u - 1u) << 16) | (kSwapOpcode << 8));
    PPC_STORE_U32(commandBuffer + offset++ * 4, kSwapSignature);
    PPC_STORE_U32(commandBuffer + offset++ * 4, frontbufferPhysical);
    PPC_STORE_U32(commandBuffer + offset++ * 4, width);
    PPC_STORE_U32(commandBuffer + offset++ * 4, height);
    while (offset < kCommandDwords) {
        PPC_STORE_U32(commandBuffer + offset++ * 4, kPacketType2);
    }
    static std::atomic<uint64_t> swapCommandTraceCount{};
    const uint64_t ordinal =
        swapCommandTraceCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal <= 8 || (ordinal & 0xFFFu) == 0) {
        std::cout << "GPU_VD_SWAP_COMMAND buffer=0x" << std::hex << commandBuffer
                  << " fetch=0x" << textureFetch << " frontbuffer_virtual=0x"
                  << frontbufferVirtual << " frontbuffer_physical=0x"
                  << frontbufferPhysical << " format=0x" << textureFormat << std::dec
                  << " width=" << width << " height=" << height
                  << " dwords=" << kCommandDwords << " guest_origin=1 ordinal="
                  << ordinal << '\n';
    }
    return {frontbufferVirtual, frontbufferPhysical, textureFormat, width, height};
}

void RuntimeNotifyGuestPhysicalWrite(uint32_t guest_address, uint32_t length,
                                     const char* source_file,
                                     uint32_t source_line) noexcept {
    if (!RuntimeGraphicsIsActive()) return;
    uint32_t physical = 0;
    if (!GuestAliasToPhysical(guest_address, length, physical)) return;
    pending_guest_physical_writes.Mark(physical, length);

    // The remaining observers belong exclusively to the completed PRESS START
    // NaN-producer investigation. The page-dirty publication above remains
    // unconditional; retiring these scans changes no guest or GPU state.
    constexpr bool kCompletedPromptNanTraceEnabled = false;
    if constexpr (!kCompletedPromptNanTraceEnabled) return;

    constexpr uint32_t kFloatConstantBytes = 48u * 4u;
    constexpr uint64_t kMaximumFloatConstantNanRecords = 8;
    const uint64_t physicalEnd = uint64_t(physical) + length;
    for (const auto& slot : watched_float_constant_sources) {
        const uint32_t sourcePhysical = slot.load(std::memory_order_acquire);
        if (!sourcePhysical || physicalEnd <= sourcePhysical ||
            physical >= sourcePhysical + kFloatConstantBytes) {
            continue;
        }
        const uint32_t firstWord =
            ((physical > sourcePhysical ? physical : sourcePhysical) & ~3u);
        const uint32_t lastByte = static_cast<uint32_t>(
            (physicalEnd < uint64_t(sourcePhysical) + kFloatConstantBytes
                 ? physicalEnd
                 : uint64_t(sourcePhysical) + kFloatConstantBytes) - 1);
        const uint32_t lastWord = lastByte & ~3u;
        bool introducedCanonicalQuietNan = false;
        for (uint32_t wordPhysical = firstWord;; wordPhysical += 4) {
            const uint32_t value = LoadPhysicalWord(wordPhysical);
            introducedCanonicalQuietNan |=
                (value & 0x7FFFFFFFu) == 0x7FC00000u;
            if (wordPhysical == lastWord) break;
        }
        if (!introducedCanonicalQuietNan) continue;

        const uint64_t ordinal =
            float_constant_nan_write_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ordinal <= kMaximumFloatConstantNanRecords) {
            uint32_t sourceHash = 2166136261u;
            uint32_t sourceWords[48]{};
            for (uint32_t index = 0; index < 48; ++index) {
                sourceWords[index] = LoadPhysicalWord(sourcePhysical + index * 4u);
                sourceHash ^= sourceWords[index];
                sourceHash *= 16777619u;
            }
            const auto timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::lock_guard lock(float_constant_nan_write_trace_mutex);
            std::filesystem::create_directories("logs");
            std::ofstream out("logs/m7_prompt_nan_source_writes_probe48.log",
                              std::ios::app);
            out << "GPU_FLOAT_CONSTANT_NAN_WRITE ordinal=" << ordinal
                << " timestamp_us=" << timestampUs
                << " guest=0x" << std::hex << guest_address
                << " physical=0x" << physical
                << " length=0x" << length
                << " source_physical=0x" << sourcePhysical
                << " source_offset=0x" << (physical - sourcePhysical)
                << " function=0x" << RuntimeActiveNonHelperGuestFunction()
                << " active_function=0x" << RuntimeActiveGuestFunction()
                << " lr=0x" << RuntimeActiveGuestLr()
                << " hash=0x" << sourceHash
                << " store_file=" << (source_file ? source_file : "unknown")
                << std::dec << " store_line=" << source_line
                << " thread=" << CurrentGuestThreadId()
                << std::hex << " words=";
            for (uint32_t index = 0; index < 48; ++index) {
                if (index) out << ',';
                out << "0x" << sourceWords[index];
            }
            out << std::dec << '\n';
            out.flush();
            std::cout << "GPU_FLOAT_CONSTANT_NAN_WRITE ordinal=" << ordinal
                      << " source=0x" << std::hex << sourcePhysical
                      << " offset=0x" << (physical - sourcePhysical)
                      << " function=0x" << RuntimeActiveNonHelperGuestFunction()
                      << " lr=0x" << RuntimeActiveGuestLr()
                      << std::dec << " thread=" << CurrentGuestThreadId() << '\n';
        }
        if (!float_constant_nan_producer_captured.exchange(
                true, std::memory_order_acq_rel)) {
            PPCContext* context = RuntimeActivePpcContext();
            if (context && adapter.virtual_membase) {
                auto loadGuestWord = [](uint32_t address) {
                    uint32_t value{};
                    std::memcpy(&value, adapter.virtual_membase + address, sizeof(value));
                    return __builtin_bswap32(value);
                };
                constexpr uint32_t kGpuState = 0x82A69B00u;
                const uint32_t transformIndex = loadGuestWord(kGpuState + 8232u);
                const uint32_t transformPool = loadGuestWord(kGpuState + 8224u);
                const uint32_t transformAddress =
                    transformPool + transformIndex * 656u + 16u;
                const PPCVRegister* vectors[] = {
                    &context->v0, &context->v1, &context->v2, &context->v3,
                    &context->v4, &context->v5, &context->v6, &context->v7,
                    &context->v8, &context->v9, &context->v10, &context->v11,
                    &context->v12, &context->v13, &context->v14, &context->v15,
                    &context->v16, &context->v17, &context->v18, &context->v19,
                    &context->v20, &context->v21, &context->v22, &context->v23,
                    &context->v24, &context->v25, &context->v26, &context->v27,
                    &context->v28, &context->v29, &context->v30, &context->v31,
                };
                std::lock_guard lock(float_constant_nan_write_trace_mutex);
                std::ofstream out("logs/m7_prompt_nan_producer_state_probe48.log",
                                  std::ios::app);
                out << "GPU_FLOAT_CONSTANT_NAN_PRODUCER thread="
                    << CurrentGuestThreadId()
                    << " function=0x" << std::hex
                    << RuntimeActiveNonHelperGuestFunction()
                    << " lr=0x" << RuntimeActiveGuestLr()
                    << " source_physical=0x" << sourcePhysical
                    << " transform_index=0x" << transformIndex
                    << " transform_pool=0x" << transformPool
                    << " transform_address=0x" << transformAddress
                    << " command_builder=0x" << loadGuestWord(kGpuState + 15748u)
                    << " r10=0x" << context->r10.u32
                    << " r11=0x" << context->r11.u32
                    << " r12=0x" << context->r12.u32 << '\n';
                out << "  TRANSFORM_INPUT";
                for (uint32_t index = 0; index < 16; ++index) {
                    out << " 0x" << loadGuestWord(transformAddress + index * 4u);
                }
                out << "\n  GPU_MATRIX_INPUT";
                for (uint32_t index = 0; index < 16; ++index) {
                    out << " 0x" << loadGuestWord(kGpuState + 17088u + index * 4u);
                }
                out << "\n  MASK_82A47230";
                for (uint32_t index = 0; index < 4; ++index) {
                    out << " 0x" << loadGuestWord(0x82A47230u + index * 4u);
                }
                out << "\n  MASK_82A47280";
                for (uint32_t index = 0; index < 4; ++index) {
                    out << " 0x" << loadGuestWord(0x82A47280u + index * 4u);
                }
                for (uint32_t vectorIndex = 0; vectorIndex < 32; ++vectorIndex) {
                    out << "\n  V" << std::dec << vectorIndex << std::hex;
                    for (uint32_t lane = 0; lane < 4; ++lane) {
                        out << " 0x" << vectors[vectorIndex]->u32[lane];
                    }
                }
                out << std::dec << '\n';
                out.flush();
            }
        }
    }

    constexpr uint32_t kTransformBytes = 16u * 4u;
    constexpr uint64_t kMaximumTransformNanRecords = 16;
    for (const auto& slot : watched_transform_sources) {
        const uint32_t sourcePhysical = slot.load(std::memory_order_acquire);
        if (!sourcePhysical || physicalEnd <= sourcePhysical ||
            physical >= sourcePhysical + kTransformBytes) {
            continue;
        }
        const uint32_t firstWord =
            ((physical > sourcePhysical ? physical : sourcePhysical) & ~3u);
        const uint32_t lastByte = static_cast<uint32_t>(
            (physicalEnd < uint64_t(sourcePhysical) + kTransformBytes
                 ? physicalEnd
                 : uint64_t(sourcePhysical) + kTransformBytes) - 1);
        const uint32_t lastWord = lastByte & ~3u;
        bool introducedCanonicalQuietNan = false;
        for (uint32_t wordPhysical = firstWord;; wordPhysical += 4) {
            const uint32_t value = LoadPhysicalWord(wordPhysical);
            introducedCanonicalQuietNan |=
                (value & 0x7FFFFFFFu) == 0x7FC00000u;
            if (wordPhysical == lastWord) break;
        }
        if (!introducedCanonicalQuietNan) continue;

        const uint64_t ordinal =
            transform_nan_write_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ordinal <= kMaximumTransformNanRecords) {
            uint32_t sourceHash = 2166136261u;
            uint32_t sourceWords[16]{};
            for (uint32_t index = 0; index < 16; ++index) {
                sourceWords[index] = LoadPhysicalWord(sourcePhysical + index * 4u);
                sourceHash ^= sourceWords[index];
                sourceHash *= 16777619u;
            }
            const auto timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::lock_guard lock(transform_nan_write_trace_mutex);
            std::filesystem::create_directories("logs");
            std::ofstream out("logs/m7_prompt_nan_transform_writes_probe48.log",
                              std::ios::app);
            out << "GPU_TRANSFORM_NAN_WRITE ordinal=" << ordinal
                << " timestamp_us=" << timestampUs
                << " guest=0x" << std::hex << guest_address
                << " physical=0x" << physical
                << " length=0x" << length
                << " source_physical=0x" << sourcePhysical
                << " source_offset=0x" << (physical - sourcePhysical)
                << " function=0x" << RuntimeActiveNonHelperGuestFunction()
                << " active_function=0x" << RuntimeActiveGuestFunction()
                << " lr=0x" << RuntimeActiveGuestLr()
                << " hash=0x" << sourceHash
                << std::dec << " thread=" << CurrentGuestThreadId()
                << std::hex << " words=";
            for (uint32_t index = 0; index < 16; ++index) {
                if (index) out << ',';
                out << "0x" << sourceWords[index];
            }
            out << std::dec << '\n';
            out.flush();
            std::cout << "GPU_TRANSFORM_NAN_WRITE ordinal=" << ordinal
                      << " source=0x" << std::hex << sourcePhysical
                      << " offset=0x" << (physical - sourcePhysical)
                      << " function=0x" << RuntimeActiveNonHelperGuestFunction()
                      << " lr=0x" << RuntimeActiveGuestLr()
                      << std::dec << " thread=" << CurrentGuestThreadId() << '\n';
        }
    }

    constexpr uint64_t kMaximumTransformInputNanRecords = 32;
    for (const auto& slot : watched_transform_input_sources) {
        const uint32_t sourcePhysical = slot.load(std::memory_order_acquire);
        if (!sourcePhysical || physicalEnd <= sourcePhysical ||
            physical >= sourcePhysical + kTransformBytes) {
            continue;
        }
        const uint32_t firstWord =
            ((physical > sourcePhysical ? physical : sourcePhysical) & ~3u);
        const uint32_t lastByte = static_cast<uint32_t>(
            (physicalEnd < uint64_t(sourcePhysical) + kTransformBytes
                 ? physicalEnd
                 : uint64_t(sourcePhysical) + kTransformBytes) - 1);
        const uint32_t lastWord = lastByte & ~3u;
        bool introducedCanonicalQuietNan = false;
        for (uint32_t wordPhysical = firstWord;; wordPhysical += 4) {
            const uint32_t value = LoadPhysicalWord(wordPhysical);
            introducedCanonicalQuietNan |=
                (value & 0x7FFFFFFFu) == 0x7FC00000u;
            if (wordPhysical == lastWord) break;
        }
        if (!introducedCanonicalQuietNan) continue;

        const uint64_t ordinal = transform_input_nan_write_count.fetch_add(
                                     1, std::memory_order_relaxed) + 1;
        if (ordinal <= kMaximumTransformInputNanRecords) {
            uint32_t sourceHash = 2166136261u;
            uint32_t sourceWords[16]{};
            for (uint32_t index = 0; index < 16; ++index) {
                sourceWords[index] = LoadPhysicalWord(sourcePhysical + index * 4u);
                sourceHash ^= sourceWords[index];
                sourceHash *= 16777619u;
            }
            const auto timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            std::lock_guard lock(transform_input_nan_write_trace_mutex);
            std::filesystem::create_directories("logs");
            std::ofstream out("logs/m7_prompt_nan_transform_input_writes_probe48.log",
                              std::ios::app);
            out << "GPU_TRANSFORM_INPUT_NAN_WRITE ordinal=" << ordinal
                << " timestamp_us=" << timestampUs
                << " guest=0x" << std::hex << guest_address
                << " physical=0x" << physical
                << " length=0x" << length
                << " source_physical=0x" << sourcePhysical
                << " source_offset=0x" << (physical - sourcePhysical)
                << " function=0x" << RuntimeActiveNonHelperGuestFunction()
                << " active_function=0x" << RuntimeActiveGuestFunction()
                << " lr=0x" << RuntimeActiveGuestLr()
                << " hash=0x" << sourceHash
                << " store_file=" << (source_file ? source_file : "unknown")
                << std::dec << " store_line=" << source_line
                << " thread=" << CurrentGuestThreadId()
                << std::hex << " words=";
            for (uint32_t index = 0; index < 16; ++index) {
                if (index) out << ',';
                out << "0x" << sourceWords[index];
            }
            out << std::dec << '\n';
            out.flush();
            std::cout << "GPU_TRANSFORM_INPUT_NAN_WRITE ordinal=" << ordinal
                      << " source=0x" << std::hex << sourcePhysical
                      << " offset=0x" << (physical - sourcePhysical)
                      << " function=0x" << RuntimeActiveNonHelperGuestFunction()
                      << " lr=0x" << RuntimeActiveGuestLr()
                      << std::dec << " thread=" << CurrentGuestThreadId() << '\n';
        }
    }

    // The title builds its transient indirect command buffers at the top of
    // physical RAM. Probe only NaN payloads written there: the prompt failure
    // has already been narrowed to a guest-produced Type-0 constant packet,
    // and broad per-store logging would materially perturb CPU/GPU timing.
    constexpr uint32_t kProbeFirst = 0x1F000000u;
    constexpr uint32_t kProbeEnd = 0x20000000u;
    constexpr uint64_t kMaximumNanRecords = 256;
    const uint64_t writeEnd = uint64_t(physical) + length;
    if (!adapter.physical_membase || writeEnd <= kProbeFirst || physical >= kProbeEnd) return;

    const uint32_t firstWord = ((physical > kProbeFirst ? physical : kProbeFirst) & ~3u);
    const uint32_t lastByte = static_cast<uint32_t>(
        (writeEnd < kProbeEnd ? writeEnd : kProbeEnd) - 1);
    const uint32_t lastWord = lastByte & ~3u;
    for (uint32_t wordPhysical = firstWord;; wordPhysical += 4) {
        const uint32_t value = LoadPhysicalWord(wordPhysical);
        const bool isCanonicalQuietNan =
            (value & 0x7FFFFFFFu) == 0x7FC00000u;
        if (isCanonicalQuietNan) {
            uint32_t packetPhysical = 0;
            for (uint32_t distance = 4; distance <= 0xC0 && distance <= wordPhysical;
                 distance += 4) {
                const uint32_t candidate = wordPhysical - distance;
                if (candidate < kProbeFirst) break;
                if (LoadPhysicalWord(candidate) == 0x002F4000u) {
                    packetPhysical = candidate;
                    break;
                }
            }
            if (!packetPhysical) {
                if (wordPhysical == lastWord) break;
                continue;
            }
            const uint64_t ordinal =
                nan_physical_write_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (ordinal <= kMaximumNanRecords) {
                const auto timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::lock_guard lock(nan_physical_write_trace_mutex);
                std::filesystem::create_directories("logs");
                std::ofstream out("logs/m7_prompt_nan_cpu_writes_probe48.log", std::ios::app);
                out << "GPU_NAN_PHYSICAL_WRITE ordinal=" << ordinal
                    << " timestamp_us=" << timestampUs
                    << " guest=0x" << std::hex << guest_address
                    << " physical=0x" << physical
                    << " length=0x" << length
                    << " packet_physical=0x" << packetPhysical
                    << " word_physical=0x" << wordPhysical
                    << " value=0x" << value
                    << " function=0x" << RuntimeActiveGuestFunction()
                    << " lr=0x" << RuntimeActiveGuestLr()
                    << std::dec << " thread=" << CurrentGuestThreadId() << '\n';
                out.flush();
                std::cout << "GPU_NAN_PHYSICAL_WRITE ordinal=" << ordinal
                          << " physical=0x" << std::hex << wordPhysical
                          << " packet=0x" << packetPhysical
                          << " value=0x" << value
                          << " function=0x" << RuntimeActiveGuestFunction()
                          << " lr=0x" << RuntimeActiveGuestLr()
                          << std::dec << " thread=" << CurrentGuestThreadId() << '\n';
            }
            if (!nan_physical_write_snapshot_captured.exchange(
                    true, std::memory_order_acq_rel)) {
                RuntimeWriteCurrentThreadSnapshot("gpu-nan-physical-write-probe48");
            }
        }
        if (wordPhysical == lastWord) break;
    }
}

void RuntimeGraphicsGuestFrameBoundary() noexcept {
    // The plugin owns frame-rate policy (display_frame_limit, developer mode
    // switch, minimized-window cap); the runtime only reports the boundary.
    if (adapter.frame_boundary && RuntimeGraphicsIsActive()) {
        adapter.frame_boundary(adapter.gpu);
    }
}

void RuntimeGraphicsGuestFrameStart() noexcept {
    if (adapter.frame_start && RuntimeGraphicsIsActive()) {
        adapter.frame_start(adapter.gpu);
    }
}

void RuntimeGraphicsGuestInputSample() noexcept {
    if (adapter.input_sample && RuntimeGraphicsIsActive()) {
        adapter.input_sample(adapter.gpu);
    }
}

void RuntimeGraphicsGuestSwapQueued() noexcept {
    if (adapter.guest_swap && RuntimeGraphicsIsActive()) {
        adapter.guest_swap(adapter.gpu);
    }
}

void RuntimeGraphicsSetMenuState(uint32_t state) noexcept {
    // The plugin paces menus by display.menu_frame_rate; the runtime only
    // reports what the title shows.
    if (adapter.set_menu_state && RuntimeGraphicsIsActive()) {
        adapter.set_menu_state(adapter.gpu, state);
    }
}

uint32_t RuntimeGraphicsGpuFrameBusyUs(uint32_t* out, uint32_t capacity,
                                       uint64_t* totalFrames) noexcept {
    if (totalFrames) *totalFrames = 0;
    if (!adapter.gpu_frame_busy || !RuntimeGraphicsIsActive()) return 0;
    return adapter.gpu_frame_busy(adapter.gpu, out, capacity, totalFrames);
}

void RuntimeFlushGuestPhysicalWrites() noexcept {
    if (!RuntimeGraphicsIsActive() || !adapter.notify_physical_write) return;
    pending_guest_physical_writes.Drain([](uint32_t physical, uint32_t length) noexcept {
        adapter.notify_physical_write(adapter.gpu, physical, length);
    });
}

void QueueRuntimeGraphicsInterrupt(uint32_t callback, uint32_t source,
                                   uint32_t cpu, uint32_t callback_data) noexcept {
    if (!callback || !RuntimeGraphicsIsActive()) {
        if (source != 0) {
            std::cout << "GPU_INTERRUPT_QUEUE_REJECT callback=0x" << std::hex << callback
                      << " source=0x" << source << " cpu=0x" << cpu
                      << " data=0x" << callback_data << std::dec
                      << " active=" << RuntimeGraphicsIsActive() << '\n';
        }
        return;
    }
    size_t pendingCount = 0;
    const uint64_t queueOrdinal =
        interrupt_queue_trace_count.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t timingToken = adapter.interrupt_timing_token && adapter.interrupt_timing_record
        ? adapter.interrupt_timing_token(adapter.gpu) : 0;
    LARGE_INTEGER enqueueTick{};
    if (timingToken) QueryPerformanceCounter(&enqueueTick);
    {
        std::lock_guard<std::mutex> lock(interrupt_mutex);
        pending_interrupts.push_back({callback, source, cpu, callback_data,
                                      timingToken, uint64_t(enqueueTick.QuadPart)});
        pendingCount = pending_interrupts.size();
        pending_interrupt_count.store(static_cast<uint32_t>(pendingCount),
                                      std::memory_order_release);
    }
    interrupt_wake.notify_one();
    if (source != 0 && (ShouldTraceLiveness(queueOrdinal) || pendingCount > 1)) {
        std::cout << "GPU_INTERRUPT_QUEUE callback=0x" << std::hex << callback
                  << " source=0x" << source << " cpu=0x" << cpu
                  << " data=0x" << callback_data << std::dec
                  << " ordinal=" << queueOrdinal
                  << " pending=" << pendingCount << '\n';
    }
}

static __declspec(noinline) bool DeliverPendingRuntimeGraphicsInterrupts(
    PPCContext& context, uint8_t* base) {
    TraceReadPointerWriteback();
    std::lock_guard<std::recursive_mutex> dispatch_lock(interrupt_dispatch_mutex);
    // A second guest thread may have waited for the active callback. Re-check
    // after acquiring the process-wide dispatch lock.
    if (delivering_interrupt) return false;
    bool delivered = false;
    InterruptDeliveryScope delivery_scope;
    while (!GuestRuntimeStopRequested()) {
        PendingInterrupt interrupt{};
        {
            std::lock_guard<std::mutex> lock(interrupt_mutex);
            if (pending_interrupts.empty()) break;
            interrupt = pending_interrupts.front();
            pending_interrupts.pop_front();
            pending_interrupt_count.store(
                static_cast<uint32_t>(pending_interrupts.size()),
                std::memory_order_release);
        }
        if (!RuntimeGeneratedAddressInRange(interrupt.callback)) {
            throw std::runtime_error("GPU interrupt callback is outside generated code");
        }
        PPCFunc* routine = PPC_LOOKUP_FUNC(base, interrupt.callback);
        if (!routine) {
            throw std::runtime_error("GPU interrupt callback has no generated function");
        }
        if (interrupt.cpu >= 6) {
            throw std::runtime_error("GPU interrupt callback targets an invalid CPU");
        }
        const PPCContext interrupted = context;
        const uint32_t pcr = context.r13.u32;
        if (!pcr) throw std::runtime_error("GPU interrupt callback has no guest PCR");
        auto* tls_pointer = reinterpret_cast<volatile uint32_t*>(base + pcr);
        const uint32_t saved_tls_pointer = __builtin_bswap32(*tls_pointer);
        auto* processor_number = reinterpret_cast<volatile uint8_t*>(base + pcr + 0x10Cu);
        const uint8_t saved_processor_number = *processor_number;
        // Xbox interrupt callbacks run with the KPCR TLS pointer cleared. The
        // title tests this value to distinguish interrupt execution.
        *tls_pointer = __builtin_bswap32(uint32_t{0});
        // ReXGlue decodes the PM4 CPU mask (and routes vblank to CPU 2). Keep
        // that architectural target through the embedded ABI so guest code
        // indexing per-CPU interrupt queues observes the requested processor.
        *processor_number = static_cast<uint8_t>(interrupt.cpu);
        context.r3.u64 = interrupt.source;
        context.r4.u64 = interrupt.callback_data;
        const uint64_t interrupt_ordinal =
            interrupt_delivery_count.fetch_add(1, std::memory_order_relaxed) + 1;
        uint32_t commandSemaphore = 0;
        uint32_t commandSemaphoreBefore = 0;
        if (interrupt.source != 0 && interrupt.callback_data) {
            commandSemaphore = __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(
                base + interrupt.callback_data + 0x2A94u));
            if (commandSemaphore) {
                commandSemaphoreBefore = __builtin_bswap32(
                    *reinterpret_cast<const volatile uint32_t*>(base + commandSemaphore));
            }
        }
        if (interrupt_ordinal == 512 && RuntimeMilestoneSnapshotsEnabled()) {
            RuntimeWriteThreadSnapshot("m6c-post-command-512-gpu-interrupts");
        } else if (interrupt_ordinal == 2048 && RuntimeMilestoneSnapshotsEnabled()) {
            RuntimeWriteThreadSnapshot("m6c-post-command-2048-gpu-interrupts");
        }
        const bool traceInterrupt = interrupt.source == 0
            ? ShouldTraceLiveness(interrupt_ordinal, 16)
            : ShouldTraceLiveness(interrupt_ordinal);
        if (traceInterrupt) {
            std::cout << "GPU_INTERRUPT_DELIVER callback=0x" << std::hex << interrupt.callback
                      << " source=0x" << interrupt.source << " data=0x"
                      << interrupt.callback_data << " cpu=0x" << interrupt.cpu << std::dec
                      << " thread=" << CurrentGuestThreadId()
                      << " ordinal=" << interrupt_ordinal;
            if (interrupt.source != 0) {
                std::cout << " semaphore=0x" << std::hex << commandSemaphore
                          << " value_before=0x" << commandSemaphoreBefore << std::dec;
            }
            std::cout << '\n';
        }
        LARGE_INTEGER dispatchTick{}, returnedTick{};
        if (interrupt.timing_token) QueryPerformanceCounter(&dispatchTick);
        try {
            routine(context, base);
        } catch (...) {
            *tls_pointer = __builtin_bswap32(saved_tls_pointer);
            *processor_number = saved_processor_number;
            context = interrupted;
            throw;
        }
        // Advisory host wake after the real guest callback. Never signals a
        // guest event or GPU fence; the CP must reread its actual comparison.
        if (adapter.interrupt_completed) adapter.interrupt_completed(adapter.gpu);
        if (interrupt.timing_token) {
            QueryPerformanceCounter(&returnedTick);
            const uint32_t after = commandSemaphore
                ? __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + commandSemaphore)) : 0;
            // Return is an upper bound on guest acknowledgment, not the exact
            // store instruction. CP can observe the store before this record.
            adapter.interrupt_timing_record(adapter.gpu, interrupt.timing_token,
                interrupt.source, commandSemaphore, commandSemaphoreBefore, after,
                interrupt.enqueue_tick, uint64_t(dispatchTick.QuadPart), uint64_t(returnedTick.QuadPart));
        }
        if (interrupt.source != 0 && traceInterrupt) {
            const uint32_t commandSemaphoreAfter = commandSemaphore
                ? __builtin_bswap32(
                      *reinterpret_cast<const volatile uint32_t*>(base + commandSemaphore))
                : 0;
            std::cout << "GPU_INTERRUPT_RETURN callback=0x" << std::hex << interrupt.callback
                      << " source=0x" << interrupt.source << " semaphore=0x"
                      << commandSemaphore << " value_after=0x" << commandSemaphoreAfter
                      << " cpu=0x" << interrupt.cpu << std::dec
                      << " thread=" << CurrentGuestThreadId()
                      << " ordinal=" << interrupt_ordinal << '\n';
        }
        *tls_pointer = __builtin_bswap32(saved_tls_pointer);
        *processor_number = saved_processor_number;
        context = interrupted;
        delivered = true;
    }
    return delivered;
}

bool DeliverRuntimeGraphicsInterrupts(PPCContext& context, uint8_t* base) {
    if (interrupt_worker_started.load(std::memory_order_acquire) &&
        CurrentGuestThreadId() !=
            interrupt_worker_thread_id.load(std::memory_order_acquire)) {
        return false;
    }
    if (delivering_interrupt) return false;
    // The worker checks this at every generated function boundary. Avoid two
    // mutex acquisitions and diagnostic read-pointer work in the overwhelmingly
    // common empty state. Enqueue publishes the queue entry before this count;
    // an interrupt racing a zero observation remains eligible at the very next
    // function boundary and also wakes the dedicated worker.
    if (!pending_interrupt_count.load(std::memory_order_acquire)) return false;
    // Keep callback register snapshots and delivery temporaries out of this
    // frequent rejection path. The selected helper retains the exact queue,
    // locking, callback, restoration and stop behavior after these predicates.
    return DeliverPendingRuntimeGraphicsInterrupts(context, base);
}
