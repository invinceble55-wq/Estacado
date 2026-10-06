cmake_minimum_required(VERSION 3.20)
project(TheDarknessRuntime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# No paths of the build machine in the binaries: __FILE__ (assertion and log
# texts) and debug information name files relative to the repository root.
get_filename_component(DARKNESS_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(TO_NATIVE_PATH "${DARKNESS_REPOSITORY_ROOT}" DARKNESS_REPOSITORY_ROOT_NATIVE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(
        "/clang:-ffile-prefix-map=${DARKNESS_REPOSITORY_ROOT_NATIVE}\\="
        "/clang:-ffile-prefix-map=${DARKNESS_REPOSITORY_ROOT}/=")
endif()
set(PPC_DIR "${CMAKE_CURRENT_LIST_DIR}/../generated/ppc")
set(SIMDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../external/XenonRecomp/thirdparty/simde")

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS "${PPC_DIR}/ppc_recomp.*.cpp")
list(APPEND GENERATED_SOURCES "${PPC_DIR}/ppc_func_mapping.cpp")

set(IMPORT_TRAPS "${CMAKE_CURRENT_BINARY_DIR}/generated_import_traps.cpp")
add_custom_command(
    OUTPUT "${IMPORT_TRAPS}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_CURRENT_LIST_DIR}/generate_import_traps.ps1"
        -SharedHeader "${PPC_DIR}/ppc_recomp_shared.h" -Output "${IMPORT_TRAPS}"
    DEPENDS "${PPC_DIR}/ppc_recomp_shared.h" "${CMAKE_CURRENT_LIST_DIR}/generate_import_traps.ps1")

add_executable(TheDarkness main.cpp pc_settings_schema.cpp runtime_audio.cpp runtime_camera.cpp runtime_camera_submission.cpp runtime_movement_packet.cpp runtime_motion_origins.cpp runtime_motion_delta.cpp runtime_temporal_camera.cpp runtime_diagnostics.cpp runtime_fatal.cpp runtime_filesystem.cpp runtime_frame_wait.cpp runtime_function_trace.cpp runtime_graphics.cpp runtime_graphics_cache.cpp runtime_input.cpp runtime_job_poll_wake.cpp runtime_memory.cpp runtime_mouse_look.cpp runtime_msaa_mode.cpp runtime_stick_look.cpp runtime_widescreen.cpp runtime_video_mode.cpp runtime_language_pack.cpp runtime_button_prompts.cpp runtime_button_prompt_hooks.cpp runtime_mmio.cpp runtime_modules.cpp runtime_mods.cpp runtime_objects.cpp runtime_package_validation.cpp runtime_pc_settings.cpp runtime_user_data.cpp runtime_game_setup.cpp runtime_auto_scale.cpp runtime_gpu_calibration.cpp runtime_gpu_calibration_model.cpp runtime_gpu_calibration_store.cpp pc_settings_ui.cpp pc_settings_arabic.cpp runtime_settings_service.cpp "${CMAKE_CURRENT_LIST_DIR}/../external/ReXGlue/src/ui/settings_schema.cpp" runtime_single_instance.cpp runtime_sync.cpp runtime_title_notifications.cpp runtime_tls.cpp runtime_threads.cpp runtime_xam.cpp verified_imports.cpp "${IMPORT_TRAPS}" ${GENERATED_SOURCES})
target_include_directories(TheDarkness PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PPC_DIR}" "${SIMDE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../external/XenonRecomp/thirdparty/TinySHA1"
    "${CMAKE_CURRENT_LIST_DIR}/../external/XenonRecomp/XenonUtils"
    "${CMAKE_CURRENT_LIST_DIR}/../external/ReXGlue/include")
target_compile_options(TheDarkness PRIVATE /arch:AVX /EHsc)
# The generated title is extremely sensitive to accidentally packaging a
# nominal Release configuration whose compiler cache has empty Release flags.
# Keep the optimization contract on the target itself so it doesn't depend on
# toolchain-populated CMAKE_CXX_FLAGS_RELEASE defaults.
function(apply_darkness_release_policy target)
if(MSVC)
    target_compile_options(${target} PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Ob2>)
    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:Release>:NDEBUG>)
    target_link_options(${target} PRIVATE
        $<$<CONFIG:Release>:/INCREMENTAL:NO>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
        $<$<CONFIG:Release>:/Brepro>)
endif()
endfunction()
apply_darkness_release_policy(TheDarkness)
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS
    "/FI${CMAKE_CURRENT_LIST_DIR}/runtime_function_trace.h;/FI${CMAKE_CURRENT_LIST_DIR}/runtime_memory_access.h")

# dcbz/dcbzl currently emit direct memset rather than PPC_STORE. Intercept only
# the affected generated units; preserve generated files and all other units.
foreach(generated_source IN LISTS GENERATED_SOURCES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${generated_source}")
    file(STRINGS "${generated_source}" generated_bulk_writes REGEX "memset\\(")
    if(generated_bulk_writes)
        set_property(SOURCE "${generated_source}" APPEND PROPERTY COMPILE_OPTIONS
            "/FI${CMAKE_CURRENT_LIST_DIR}/runtime_generated_bulk_write.h")
    endif()
    file(STRINGS "${generated_source}" generated_atomic_writes REGEX "__sync_bool_compare_and_swap\\(")
    if(generated_atomic_writes)
        set_property(SOURCE "${generated_source}" APPEND PROPERTY COMPILE_OPTIONS
            "/FI${CMAKE_CURRENT_LIST_DIR}/runtime_generated_atomic_write.h")
    endif()
endforeach()
target_link_options(TheDarkness PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/TheDarkness.map")
target_link_libraries(TheDarkness PRIVATE XenonUtils bcrypt winmm tomlplusplus::tomlplusplus)

# The lightweight native settings front end never starts guest execution. It
# delegates all writes to TheDarkness.exe's already-verified, single-writer,
# package-validated --install-preset action. V315+: Dear ImGui (Win32 +
# Direct3D 11) around the shared settings panel also used in game.
set(DARKNESS_IMGUI_DIR "${CMAKE_CURRENT_LIST_DIR}/../external/ReXGlue/thirdparty/imgui")
set(DARKNESS_REXGLUE_DIR "${CMAKE_CURRENT_LIST_DIR}/../external/ReXGlue")
add_executable(TheDarknessSettings WIN32
    settings_launcher.cpp runtime_user_data.cpp runtime_game_setup.cpp runtime_language_install.cpp pc_settings_ui.cpp pc_settings_arabic.cpp pc_settings_utility_model.cpp
    pc_settings_schema.cpp runtime_single_instance.cpp runtime_auto_scale.cpp runtime_gpu_calibration_model.cpp runtime_gpu_calibration_store.cpp
    "${DARKNESS_REXGLUE_DIR}/src/ui/settings_panel.cpp"
    "${DARKNESS_REXGLUE_DIR}/src/ui/settings_schema.cpp"
    "${DARKNESS_IMGUI_DIR}/imgui.cpp" "${DARKNESS_IMGUI_DIR}/imgui_draw.cpp"
    "${DARKNESS_IMGUI_DIR}/imgui_tables.cpp" "${DARKNESS_IMGUI_DIR}/imgui_widgets.cpp"
    "${DARKNESS_IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${DARKNESS_IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(TheDarknessSettings PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}" "${DARKNESS_REXGLUE_DIR}/include"
    "${DARKNESS_IMGUI_DIR}" "${DARKNESS_IMGUI_DIR}/backends")
target_compile_options(TheDarknessSettings PRIVATE /EHsc)
# The optional Arabic language pack of a release (release builds set both;
# empty = install from a downloaded file only).
set(DARKNESS_LANGUAGE_PACK_URL "" CACHE STRING "HTTPS URL of the release's language pack archive")
set(DARKNESS_LANGUAGE_PACK_SHA256 "" CACHE STRING "SHA-256 (lower-case hex) of that archive")
target_compile_definitions(TheDarknessSettings PRIVATE
    "DARKNESS_LANGUAGE_PACK_URL=\"${DARKNESS_LANGUAGE_PACK_URL}\""
    "DARKNESS_LANGUAGE_PACK_SHA256=\"${DARKNESS_LANGUAGE_PACK_SHA256}\"")
target_link_libraries(TheDarknessSettings PRIVATE tomlplusplus::tomlplusplus
    shell32 d3d11 dxgi d3dcompiler dwmapi bcrypt winhttp)
apply_darkness_release_policy(TheDarknessSettings)
add_dependencies(TheDarkness TheDarknessSettings)

# Keep the native settings front end functional directly from a developer
# Release directory as well as from a packaged candidate. Each output is a
# declared dependency so adding or editing a source-controlled preset cannot
# leave a stale or incomplete adjacent preset set.
set(DARKNESS_PC_PRESET_NAMES
    enhanced
    performance
    original
    original_720p
    native_desktop_original
    performance_1080p_fxaa
    balanced_1080p
    quality_1440p
    quality_4k
    validation_1440p_internal_2x
    validation_4k_windowed_internal_2x
    steam_deck_800p_compatibility)
set(DARKNESS_PC_PRESET_OUTPUTS)
foreach(preset_name IN LISTS DARKNESS_PC_PRESET_NAMES)
    set(preset_source
        "${CMAKE_CURRENT_LIST_DIR}/../config/pc_presets/${preset_name}.toml")
    set(preset_output
        "${CMAKE_CURRENT_BINARY_DIR}/presets/${preset_name}.toml")
    add_custom_command(
        OUTPUT "${preset_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/presets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${preset_source}" "${preset_output}"
        DEPENDS "${preset_source}"
        COMMENT "Synchronizing PC preset ${preset_name}")
    list(APPEND DARKNESS_PC_PRESET_OUTPUTS "${preset_output}")
endforeach()
add_custom_target(TheDarknessPcPresets
    DEPENDS ${DARKNESS_PC_PRESET_OUTPUTS})
add_dependencies(TheDarknessSettings TheDarknessPcPresets)

# ReXGlue is a multi-config build and places deployable DLLs in a
# configuration subdirectory (for example, out/win-amd64/Release). Pointing at
# the parent permits a stale manually copied DLL to satisfy DEPENDS forever.
if(CMAKE_CONFIGURATION_TYPES)
    set(REXGLUE_OUTPUT_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../external/ReXGlue/out/win-amd64/$<CONFIG>")
else()
    set(REXGLUE_OUTPUT_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../external/ReXGlue/out/win-amd64/${CMAKE_BUILD_TYPE}")
endif()
set(RUNTIME_REXRUNTIME "${CMAKE_CURRENT_BINARY_DIR}/rexruntime.dll")
# Read-only display discovery must use the same pinned SDL ordering as the
# window backend, not an assumed Win32 adapter-to-monitor index mapping.
target_include_directories(TheDarknessSettings PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../external/ReXGlue/thirdparty/sdl3/include")
set_property(TARGET TheDarknessSettings PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_link_libraries(TheDarknessSettings PRIVATE
    "${REXGLUE_OUTPUT_DIR}/SDL3-static.lib"
    kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version uuid advapi32 setupapi hid)
set(RUNTIME_REXGPU "${CMAKE_CURRENT_BINARY_DIR}/rexgpu-xenos.dll")
set(RUNTIME_PC_CONFIG_EXAMPLE
    "${CMAKE_CURRENT_BINARY_DIR}/TheDarkness.pc.example.toml")
set(RUNTIME_MODS_CONFIG_EXAMPLE
    "${CMAKE_CURRENT_BINARY_DIR}/TheDarkness.mods.example.toml")
add_custom_command(
    OUTPUT "${RUNTIME_REXRUNTIME}" "${RUNTIME_REXGPU}"
           "${RUNTIME_PC_CONFIG_EXAMPLE}" "${RUNTIME_MODS_CONFIG_EXAMPLE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${REXGLUE_OUTPUT_DIR}/rexruntime.dll" "${RUNTIME_REXRUNTIME}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${REXGLUE_OUTPUT_DIR}/rexgpu-xenos.dll" "${RUNTIME_REXGPU}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_LIST_DIR}/../config/pc_settings_v1.toml"
        "${RUNTIME_PC_CONFIG_EXAMPLE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_LIST_DIR}/../config/mods_v1.toml"
        "${RUNTIME_MODS_CONFIG_EXAMPLE}"
    DEPENDS
        "${REXGLUE_OUTPUT_DIR}/rexruntime.dll"
        "${REXGLUE_OUTPUT_DIR}/rexgpu-xenos.dll"
        "${CMAKE_CURRENT_LIST_DIR}/../config/pc_settings_v1.toml"
        "${CMAKE_CURRENT_LIST_DIR}/../config/mods_v1.toml"
    COMMENT "Synchronizing pinned ReXGlue DLLs and PC settings example")
add_custom_target(TheDarknessRuntimeFiles
    DEPENDS "${RUNTIME_REXRUNTIME}" "${RUNTIME_REXGPU}"
            "${RUNTIME_PC_CONFIG_EXAMPLE}" "${RUNTIME_MODS_CONFIG_EXAMPLE}")
add_dependencies(TheDarkness TheDarknessRuntimeFiles)
# The XEX loader's LZX decoder (libmspack, LGPL-2.1) is a separate library,
# mspack_lzx.dll, next to TheDarkness.exe: players can replace it.
set(XENONUTILS_LZX_SHARED ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../external/XenonRecomp" XenonRecomp)
add_custom_command(TARGET TheDarkness POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mspack_lzx> $<TARGET_FILE_DIR:TheDarkness>
    COMMENT "Copying mspack_lzx.dll next to TheDarkness.exe")
