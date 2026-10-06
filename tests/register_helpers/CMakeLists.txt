cmake_minimum_required(VERSION 3.20)
project(DarknessRegisterHelperInspection LANGUAGES CXX)

include(CTest)

# Policy tests of the development tree's own automation (validation probes,
# candidate staging, visual regression) and of local packs: registered only
# where those files exist. The public source leaves them out.
function(darkness_local_test name command)
    foreach(required ${ARGN})
        if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../${required}")
            message(STATUS "${name}: needs ${required} (development tree), not registered")
            return()
        endif()
    endforeach()
    add_test(NAME ${name} COMMAND ${command})
endfunction()

add_executable(runtime_movement_packet_test runtime_movement_packet_test.cpp)
target_compile_features(runtime_movement_packet_test PRIVATE cxx_std_17)
add_test(NAME runtime_movement_packet COMMAND runtime_movement_packet_test)

add_executable(gpu_native_resolve_regions_test gpu_native_resolve_regions_test.cpp)
target_include_directories(gpu_native_resolve_regions_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_features(gpu_native_resolve_regions_test PRIVATE cxx_std_17)
target_compile_definitions(gpu_native_resolve_regions_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME gpu_native_resolve_regions COMMAND gpu_native_resolve_regions_test)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp" XenonRecomp)

add_executable(register_helper_scan helper_scan.cpp)
target_link_libraries(register_helper_scan PRIVATE XenonUtils)

add_executable(generated_diagnostic_triage generated_diagnostic_triage.cpp)

add_executable(xex_region_disasm xex_region_disasm.cpp)
target_link_libraries(xex_region_disasm PRIVATE XenonUtils)

add_executable(xex_word_search xex_word_search.cpp)
target_link_libraries(xex_word_search PRIVATE XenonUtils)

add_executable(xex_string_search xex_string_search.cpp)
target_link_libraries(xex_string_search PRIVATE XenonUtils)

add_executable(xex_address_references xex_address_references.cpp)
target_link_libraries(xex_address_references PRIVATE XenonUtils)

add_executable(raw_ppc_disasm raw_ppc_disasm.cpp)
target_link_libraries(raw_ppc_disasm PRIVATE XenonUtils)

add_executable(xex_entry xex_entry.cpp)
target_link_libraries(xex_entry PRIVATE XenonUtils)

add_executable(switch_pattern_audit switch_pattern_audit.cpp)
target_link_libraries(switch_pattern_audit PRIVATE XenonUtils)

add_executable(inline_switch_scan inline_switch_scan.cpp)
target_link_libraries(inline_switch_scan PRIVATE XenonUtils)

add_executable(switch_source_report switch_source_report.cpp)
target_link_libraries(switch_source_report PRIVATE XenonUtils)

add_executable(switch_range_proposal switch_range_proposal.cpp)
target_link_libraries(switch_range_proposal PRIVATE XenonUtils)

add_executable(xex_cross_reference xex_cross_reference.cpp)
target_link_libraries(xex_cross_reference PRIVATE XenonUtils)

# A small, synthetic harness for the XenonRecomp switch code generator.  It
# links the generator sources directly so no game image or generated output is
# involved in the regression tests.
add_library(xenonrecomp_codegen_test_support STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonRecomp/recompiler.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonRecomp/recompiler_config.cpp")
target_include_directories(xenonrecomp_codegen_test_support PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonRecomp")
target_precompile_headers(xenonrecomp_codegen_test_support PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonRecomp/pch.h")
target_link_libraries(xenonrecomp_codegen_test_support PRIVATE
    LibXenonAnalyse XenonUtils fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)

add_executable(external_switch_codegen_test external_switch_codegen_test.cpp)
target_link_libraries(external_switch_codegen_test PRIVATE
    xenonrecomp_codegen_test_support LibXenonAnalyse XenonUtils
    fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
add_test(NAME external_switch_codegen COMMAND external_switch_codegen_test)

# Protect the evidence-backed, title-specific switch ownership overlays without
# embedding or reading any copyrighted game data in the test.
add_executable(darkness_switch_config_test darkness_switch_config_test.cpp)
target_compile_definitions(darkness_switch_config_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(darkness_switch_config_test PRIVATE
    xenonrecomp_codegen_test_support LibXenonAnalyse XenonUtils
    fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
add_test(NAME darkness_switch_config COMMAND darkness_switch_config_test)

add_executable(vslh_test vslh_test.cpp)
target_include_directories(vslh_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonUtils")
target_link_libraries(vslh_test PRIVATE
    xenonrecomp_codegen_test_support LibXenonAnalyse XenonUtils
    fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
add_test(NAME vslh COMMAND vslh_test)

add_executable(vmx_batch1_test vmx_batch1_test.cpp)
target_include_directories(vmx_batch1_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonUtils")
target_link_libraries(vmx_batch1_test PRIVATE
    xenonrecomp_codegen_test_support LibXenonAnalyse XenonUtils
    fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
add_test(NAME vmx_batch1 COMMAND vmx_batch1_test)

# vctuxs / vcfpuxws128: the unsigned saturating conversion the recompiler emits.
add_executable(vctuxs_test vctuxs_test.cpp)
target_include_directories(vctuxs_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonUtils"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/thirdparty/simde")
target_compile_features(vctuxs_test PRIVATE cxx_std_20)
add_test(NAME vctuxs COMMAND vctuxs_test)

add_executable(mulhd_test mulhd_test.cpp)
target_include_directories(mulhd_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonUtils")
target_link_libraries(mulhd_test PRIVATE
    xenonrecomp_codegen_test_support LibXenonAnalyse XenonUtils
    fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
add_test(NAME mulhd COMMAND mulhd_test)

add_executable(frsqrte_test frsqrte_test.cpp)
target_include_directories(frsqrte_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonUtils")
target_link_libraries(frsqrte_test PRIVATE
    xenonrecomp_codegen_test_support LibXenonAnalyse XenonUtils
    fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
add_test(NAME frsqrte COMMAND frsqrte_test)

add_executable(crlogic_test crlogic_test.cpp)
target_include_directories(crlogic_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonUtils")
target_link_libraries(crlogic_test PRIVATE
    xenonrecomp_codegen_test_support LibXenonAnalyse XenonUtils
    fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
add_test(NAME crlogic COMMAND crlogic_test)

add_executable(timebase_test timebase_test.cpp)
target_include_directories(timebase_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonUtils")
target_link_libraries(timebase_test PRIVATE
    xenonrecomp_codegen_test_support LibXenonAnalyse XenonUtils
    fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
add_test(NAME timebase COMMAND timebase_test)

add_executable(db16cyc_test db16cyc_test.cpp)
target_include_directories(db16cyc_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonUtils")
target_link_libraries(db16cyc_test PRIVATE
    xenonrecomp_codegen_test_support LibXenonAnalyse XenonUtils
    fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
add_test(NAME db16cyc COMMAND db16cyc_test)

add_executable(vector_store_codegen_test vector_store_codegen_test.cpp)
target_include_directories(vector_store_codegen_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonUtils")
target_link_libraries(vector_store_codegen_test PRIVATE
    xenonrecomp_codegen_test_support LibXenonAnalyse XenonUtils
    fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
add_test(NAME vector_store_codegen COMMAND vector_store_codegen_test)

add_executable(runtime_import_test runtime_import_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_audio.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_camera.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_motion_origins.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_motion_delta.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_temporal_camera.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_diagnostics.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_fatal.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_filesystem.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_function_trace.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_graphics.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_input.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_button_prompts.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_job_poll_wake.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_memory.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_mmio.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_modules.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_objects.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_sync.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_title_notifications.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_tls.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_threads.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_xam.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/verified_imports.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_video_mode.cpp")
target_include_directories(runtime_import_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../../generated/ppc"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/thirdparty/simde"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/thirdparty/TinySHA1"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/XenonUtils")
target_compile_options(runtime_import_test PRIVATE /arch:AVX /EHsc)
target_link_libraries(runtime_import_test PRIVATE XenonUtils bcrypt winmm)
add_test(NAME runtime_imports COMMAND runtime_import_test)

add_executable(runtime_mmio_test runtime_mmio_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_mmio.cpp")
target_include_directories(runtime_mmio_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_mmio_test PRIVATE /EHsc)
add_test(NAME runtime_mmio COMMAND runtime_mmio_test)

add_executable(runtime_memory_access_test runtime_memory_access_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_mmio.cpp")
target_include_directories(runtime_memory_access_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_memory_access_test PRIVATE /EHsc)
add_test(NAME runtime_memory_access COMMAND runtime_memory_access_test)

add_executable(gpu_memory_range_test gpu_memory_range_test.cpp)
target_include_directories(gpu_memory_range_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
add_test(NAME gpu_memory_range COMMAND gpu_memory_range_test)

add_executable(gpu_resolve_rectangle_test gpu_resolve_rectangle_test.cpp)
target_include_directories(gpu_resolve_rectangle_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_resolve_rectangle COMMAND gpu_resolve_rectangle_test)

add_executable(gpu_scaled_resolve_initialization_test
    gpu_scaled_resolve_initialization_test.cpp)
target_include_directories(gpu_scaled_resolve_initialization_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_scaled_resolve_initialization
    COMMAND gpu_scaled_resolve_initialization_test)

add_executable(gpu_scaled_resolve_replication_test gpu_scaled_resolve_replication_test.cpp)
target_link_libraries(gpu_scaled_resolve_replication_test PRIVATE d3d12 d3dcompiler)
add_test(NAME gpu_scaled_resolve_replication COMMAND gpu_scaled_resolve_replication_test)

add_executable(gpu_texture_local_x_address_policy_test
    gpu_texture_local_x_address_policy_test.cpp)
target_compile_definitions(gpu_texture_local_x_address_policy_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/../../external/ReXGlue")
add_test(NAME gpu_texture_local_x_address_policy
    COMMAND gpu_texture_local_x_address_policy_test)

add_executable(gpu_scaled_texture_weights_policy_test
    gpu_scaled_texture_weights_policy_test.cpp)
target_compile_definitions(gpu_scaled_texture_weights_policy_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/../../external/ReXGlue")
add_test(NAME gpu_scaled_texture_weights_policy
    COMMAND gpu_scaled_texture_weights_policy_test)

add_executable(gpu_native_scale_threshold_policy_test
    gpu_native_scale_threshold_policy_test.cpp)
target_compile_definitions(gpu_native_scale_threshold_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
target_include_directories(gpu_native_scale_threshold_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
darkness_local_test(gpu_native_scale_threshold_policy gpu_native_scale_threshold_policy_test
    config/pc_phase1_validation/internal_only_2x_720p.toml)

add_executable(gpu_native_shader_grid_policy_test gpu_native_shader_grid_policy_test.cpp)
target_include_directories(gpu_native_shader_grid_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_native_shader_grid_policy COMMAND gpu_native_shader_grid_policy_test)

add_executable(gpu_edram_canonical_sample_layout_test
    gpu_edram_canonical_sample_layout_test.cpp)
target_compile_definitions(gpu_edram_canonical_sample_layout_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
add_test(NAME gpu_edram_canonical_sample_layout
    COMMAND gpu_edram_canonical_sample_layout_test)

add_executable(gpu_texture_readback_policy_test
    gpu_texture_readback_policy_test.cpp)
target_compile_definitions(gpu_texture_readback_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME gpu_texture_readback_policy
    COMMAND gpu_texture_readback_policy_test)

add_executable(gpu_zpd_report_test gpu_zpd_report_test.cpp)
target_include_directories(gpu_zpd_report_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/thirdparty/simde")
target_compile_features(gpu_zpd_report_test PRIVATE cxx_std_23)
add_test(NAME gpu_zpd_report COMMAND gpu_zpd_report_test)

add_executable(gpu_present_policy_test gpu_present_policy_test.cpp)
target_include_directories(gpu_present_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_present_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME gpu_present_policy COMMAND gpu_present_policy_test)

add_executable(gpu_pipeline_preload_policy_test gpu_pipeline_preload_policy_test.cpp)
target_include_directories(gpu_pipeline_preload_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_pipeline_preload_policy_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
add_test(NAME gpu_pipeline_preload_policy COMMAND gpu_pipeline_preload_policy_test)

add_executable(gpu_hitch_diagnostics_policy_test gpu_hitch_diagnostics_policy_test.cpp)
target_compile_definitions(gpu_hitch_diagnostics_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
darkness_local_test(gpu_hitch_diagnostics_policy gpu_hitch_diagnostics_policy_test
    scripts/start-pc-phase1-validation-probe.ps1)

add_executable(gpu_scene_vertex_output_trace_policy_test
    gpu_scene_vertex_output_trace_policy_test.cpp)
target_compile_definitions(gpu_scene_vertex_output_trace_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME gpu_scene_vertex_output_trace_policy
    COMMAND gpu_scene_vertex_output_trace_policy_test)

add_executable(runtime_audio_diagnostics_policy_test
    runtime_audio_diagnostics_policy_test.cpp)
target_compile_definitions(runtime_audio_diagnostics_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
darkness_local_test(runtime_audio_diagnostics_policy runtime_audio_diagnostics_policy_test
    scripts/start-pc-phase1-validation-probe.ps1)

add_executable(gpu_host_frame_limiter_test gpu_host_frame_limiter_test.cpp)
target_include_directories(gpu_host_frame_limiter_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_host_frame_limiter COMMAND gpu_host_frame_limiter_test)

add_executable(gpu_guest_frame_limiter_test gpu_guest_frame_limiter_test.cpp)
target_include_directories(gpu_guest_frame_limiter_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_guest_frame_limiter COMMAND gpu_guest_frame_limiter_test)

add_executable(gpu_frame_rate_policy_test gpu_frame_rate_policy_test.cpp)
target_include_directories(gpu_frame_rate_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_frame_rate_policy COMMAND gpu_frame_rate_policy_test)

add_executable(runtime_input_capabilities_cache_test runtime_input_capabilities_cache_test.cpp)
target_include_directories(runtime_input_capabilities_cache_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
add_test(NAME runtime_input_capabilities_cache COMMAND runtime_input_capabilities_cache_test)

add_executable(runtime_input_empty_slot_gate_test runtime_input_empty_slot_gate_test.cpp)
target_include_directories(runtime_input_empty_slot_gate_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_definitions(runtime_input_empty_slot_gate_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME runtime_input_empty_slot_gate COMMAND runtime_input_empty_slot_gate_test)

add_executable(runtime_input_slot_routing_test runtime_input_slot_routing_test.cpp)
target_include_directories(runtime_input_slot_routing_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_definitions(runtime_input_slot_routing_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME runtime_input_slot_routing COMMAND runtime_input_slot_routing_test)

add_executable(runtime_overlay_chord_test runtime_overlay_chord_test.cpp)
target_include_directories(runtime_overlay_chord_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_definitions(runtime_overlay_chord_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME runtime_overlay_chord COMMAND runtime_overlay_chord_test)

add_executable(runtime_button_prompts_test runtime_button_prompts_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_button_prompts.cpp")
target_include_directories(runtime_button_prompts_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
add_test(NAME runtime_button_prompts COMMAND runtime_button_prompts_test)

add_executable(runtime_job_poll_wake_test runtime_job_poll_wake_test.cpp)
target_include_directories(runtime_job_poll_wake_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_definitions(runtime_job_poll_wake_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME runtime_job_poll_wake COMMAND runtime_job_poll_wake_test)

add_executable(runtime_mouse_look_policy_test runtime_mouse_look_policy_test.cpp)
target_include_directories(runtime_mouse_look_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_definitions(runtime_mouse_look_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_compile_features(runtime_mouse_look_policy_test PRIVATE cxx_std_20)
add_test(NAME runtime_mouse_look_policy COMMAND runtime_mouse_look_policy_test)

add_executable(present_statistics_test present_statistics_test.cpp)
target_include_directories(present_statistics_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_features(present_statistics_test PRIVATE cxx_std_20)
add_test(NAME present_statistics COMMAND present_statistics_test)

add_executable(gpu_texture_heap_pool_test gpu_texture_heap_pool_test.cpp)
target_include_directories(gpu_texture_heap_pool_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_texture_heap_pool_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
target_compile_features(gpu_texture_heap_pool_test PRIVATE cxx_std_20)
add_test(NAME gpu_texture_heap_pool COMMAND gpu_texture_heap_pool_test)

add_executable(gpu_timing_window_test gpu_timing_window_test.cpp)
target_include_directories(gpu_timing_window_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_timing_window_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
target_compile_features(gpu_timing_window_test PRIVATE cxx_std_20)
add_test(NAME gpu_timing_window COMMAND gpu_timing_window_test)

add_executable(gpu_texture_scaled_lookup_test gpu_texture_scaled_lookup_test.cpp)
target_compile_definitions(gpu_texture_scaled_lookup_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
target_compile_features(gpu_texture_scaled_lookup_test PRIVATE cxx_std_20)
add_test(NAME gpu_texture_scaled_lookup COMMAND gpu_texture_scaled_lookup_test)

add_executable(gpu_pipeline_storage_seed_test gpu_pipeline_storage_seed_test.cpp)
target_include_directories(gpu_pipeline_storage_seed_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/thirdparty/xxHash")
target_compile_features(gpu_pipeline_storage_seed_test PRIVATE cxx_std_20)
add_test(NAME gpu_pipeline_storage_seed COMMAND gpu_pipeline_storage_seed_test)

add_executable(gpu_presenter_completion_policy_test gpu_presenter_completion_policy_test.cpp)
target_compile_definitions(gpu_presenter_completion_policy_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
add_test(NAME gpu_presenter_completion_policy COMMAND gpu_presenter_completion_policy_test)

add_executable(rex_executable_path_policy_test rex_executable_path_policy_test.cpp)
target_compile_definitions(rex_executable_path_policy_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
add_test(NAME rex_executable_path_policy COMMAND rex_executable_path_policy_test)

add_executable(gpu_vertex_fetch_bounds_test gpu_vertex_fetch_bounds_test.cpp)
target_compile_definitions(gpu_vertex_fetch_bounds_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
add_test(NAME gpu_vertex_fetch_bounds COMMAND gpu_vertex_fetch_bounds_test)

add_executable(gpu_shader_replacement_pack_test
    gpu_shader_replacement_pack_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/src/graphics/pipeline/shader/replacement_pack.cpp")
target_include_directories(gpu_shader_replacement_pack_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/thirdparty/simde"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/thirdparty/xxHash")
target_compile_features(gpu_shader_replacement_pack_test PRIVATE cxx_std_23)
target_link_libraries(gpu_shader_replacement_pack_test PRIVATE xxHash::xxhash)
add_test(NAME gpu_shader_replacement_pack COMMAND gpu_shader_replacement_pack_test)

add_executable(gpu_darkness_motion_blur_pack_test
    gpu_darkness_motion_blur_pack_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/src/graphics/pipeline/shader/replacement_pack.cpp")
target_include_directories(gpu_darkness_motion_blur_pack_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/thirdparty/simde"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/thirdparty/xxHash")
target_compile_definitions(gpu_darkness_motion_blur_pack_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_compile_features(gpu_darkness_motion_blur_pack_test PRIVATE cxx_std_23)
target_link_libraries(gpu_darkness_motion_blur_pack_test PRIVATE xxHash::xxhash)
darkness_local_test(gpu_darkness_motion_blur_pack gpu_darkness_motion_blur_pack_test
    build/runtime_data/TheDarkness.motion_blur_off.xsrp)

add_executable(xenos_shader_catalog
    "${CMAKE_CURRENT_LIST_DIR}/../../tools/xenos_shader_catalog.cpp")
target_include_directories(xenos_shader_catalog PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/thirdparty/xxHash")
target_compile_features(xenos_shader_catalog PRIVATE cxx_std_23)

add_executable(gpu_video_mode_test gpu_video_mode_test.cpp)
target_include_directories(gpu_video_mode_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_definitions(gpu_video_mode_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME gpu_video_mode COMMAND gpu_video_mode_test)

add_executable(gpu_guest_aspect_policy_test gpu_guest_aspect_policy_test.cpp)
target_include_directories(gpu_guest_aspect_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_guest_aspect_policy COMMAND gpu_guest_aspect_policy_test)

add_executable(gpu_window_input_policy_test gpu_window_input_policy_test.cpp)
target_include_directories(gpu_window_input_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_window_input_policy COMMAND gpu_window_input_policy_test)

add_executable(gpu_window_mode_policy_test gpu_window_mode_policy_test.cpp)
target_include_directories(gpu_window_mode_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_window_mode_policy COMMAND gpu_window_mode_policy_test)

add_executable(gpu_embedded_window_policy_test gpu_embedded_window_policy_test.cpp)
target_compile_definitions(gpu_embedded_window_policy_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
add_test(NAME gpu_embedded_window_policy COMMAND gpu_embedded_window_policy_test)

add_executable(gpu_effective_pc_settings_policy_test
    gpu_effective_pc_settings_policy_test.cpp)
target_compile_definitions(gpu_effective_pc_settings_policy_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
add_test(NAME gpu_effective_pc_settings_policy
    COMMAND gpu_effective_pc_settings_policy_test)

add_executable(gpu_mnk_state_policy_test gpu_mnk_state_policy_test.cpp)
target_include_directories(gpu_mnk_state_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_mnk_state_policy COMMAND gpu_mnk_state_policy_test)

add_executable(gpu_mnk_test_tap_policy_test gpu_mnk_test_tap_policy_test.cpp)
target_include_directories(gpu_mnk_test_tap_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_mnk_test_tap_policy_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
add_test(NAME gpu_mnk_test_tap_policy COMMAND gpu_mnk_test_tap_policy_test)

add_executable(gpu_mnk_test_script_policy_test gpu_mnk_test_script_policy_test.cpp)
target_include_directories(gpu_mnk_test_script_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_mnk_test_script_policy_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue"
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_compile_features(gpu_mnk_test_script_policy_test PRIVATE cxx_std_20)
add_test(NAME gpu_mnk_test_script_policy COMMAND gpu_mnk_test_script_policy_test)

add_executable(gpu_mnk_focus_attachment_policy_test
    gpu_mnk_focus_attachment_policy_test.cpp)
target_compile_definitions(gpu_mnk_focus_attachment_policy_test PRIVATE
    REXGLUE_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
add_test(NAME gpu_mnk_focus_attachment_policy
    COMMAND gpu_mnk_focus_attachment_policy_test)

add_executable(gpu_embedded_screenshot_policy_test
    gpu_embedded_screenshot_policy_test.cpp)
target_include_directories(gpu_embedded_screenshot_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_embedded_screenshot_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME gpu_embedded_screenshot_policy
    COMMAND gpu_embedded_screenshot_policy_test)

add_executable(gpu_manual_gameplay_capture_policy_test
    gpu_manual_gameplay_capture_policy_test.cpp)
target_include_directories(gpu_manual_gameplay_capture_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_manual_gameplay_capture_policy
    COMMAND gpu_manual_gameplay_capture_policy_test)

add_executable(gpu_resolve_boundary_capture_policy_test
    gpu_resolve_boundary_capture_policy_test.cpp)
target_include_directories(gpu_resolve_boundary_capture_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_resolve_boundary_capture_policy
    COMMAND gpu_resolve_boundary_capture_policy_test)

add_executable(gpu_target_writer_capture_policy_test
    gpu_target_writer_capture_policy_test.cpp)
target_include_directories(gpu_target_writer_capture_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_target_writer_capture_policy
    COMMAND gpu_target_writer_capture_policy_test)

add_executable(gpu_geometry_readback_policy_test gpu_geometry_readback_policy_test.cpp)
target_include_directories(gpu_geometry_readback_policy_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/ReXGlue/include)
add_test(NAME gpu_geometry_readback_policy COMMAND gpu_geometry_readback_policy_test)

add_executable(gpu_depth_resolve_capture_policy_test gpu_depth_resolve_capture_policy_test.cpp)
target_include_directories(gpu_depth_resolve_capture_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_depth_resolve_capture_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME gpu_depth_resolve_capture_policy COMMAND gpu_depth_resolve_capture_policy_test)

add_executable(gpu_camera_draw_capture_policy_test gpu_camera_draw_capture_policy_test.cpp)
target_include_directories(gpu_camera_draw_capture_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_camera_draw_capture_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME gpu_camera_draw_capture_policy COMMAND gpu_camera_draw_capture_policy_test)

add_executable(gpu_camera_geometry_capture_policy_test gpu_camera_geometry_capture_policy_test.cpp)
target_include_directories(gpu_camera_geometry_capture_policy_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_camera_geometry_capture_policy_test PRIVATE
  DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_test(NAME gpu_camera_geometry_capture_policy COMMAND gpu_camera_geometry_capture_policy_test)

add_executable(gpu_camera_depth_clear_policy_test gpu_camera_depth_clear_policy_test.cpp)
target_include_directories(gpu_camera_depth_clear_policy_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_camera_depth_clear_policy_test PRIVATE
  DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_test(NAME gpu_camera_depth_clear_policy COMMAND gpu_camera_depth_clear_policy_test)

add_executable(gpu_scene_resolve_capture_policy_test gpu_scene_resolve_capture_policy_test.cpp)
target_include_directories(gpu_scene_resolve_capture_policy_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_scene_resolve_capture_policy_test PRIVATE
  DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_test(NAME gpu_scene_resolve_capture_policy COMMAND gpu_scene_resolve_capture_policy_test)

add_executable(gpu_deferred_color_readback_test gpu_deferred_color_readback_test.cpp)
target_include_directories(gpu_deferred_color_readback_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_deferred_color_readback_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(gpu_deferred_color_readback_test PRIVATE d3d12 dxgi d3dcompiler)
target_compile_options(gpu_deferred_color_readback_test PRIVATE /EHsc)
add_test(NAME gpu_deferred_color_readback COMMAND gpu_deferred_color_readback_test)

add_executable(gpu_color_sample_readback_test gpu_color_sample_readback_test.cpp)
target_include_directories(gpu_color_sample_readback_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_color_sample_readback_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(gpu_color_sample_readback_test PRIVATE d3d12 dxgi d3dcompiler)
target_compile_options(gpu_color_sample_readback_test PRIVATE /EHsc)
add_test(NAME gpu_color_sample_readback COMMAND gpu_color_sample_readback_test)

add_executable(gpu_depth_sample_readback_test gpu_depth_sample_readback_test.cpp)
target_compile_definitions(gpu_depth_sample_readback_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(gpu_depth_sample_readback_test PRIVATE d3d12 dxgi d3dcompiler)
target_compile_options(gpu_depth_sample_readback_test PRIVATE /EHsc)
add_test(NAME gpu_depth_sample_readback COMMAND gpu_depth_sample_readback_test)

add_executable(gpu_resolve_raw_views_test gpu_resolve_raw_views_test.cpp)
target_link_libraries(gpu_resolve_raw_views_test PRIVATE d3d12 dxgi d3dcompiler)
target_compile_options(gpu_resolve_raw_views_test PRIVATE /EHsc)
add_test(NAME gpu_resolve_raw_views COMMAND gpu_resolve_raw_views_test)

add_executable(gpu_embedded_debug_overlay_policy_test
    gpu_embedded_debug_overlay_policy_test.cpp)
target_include_directories(gpu_embedded_debug_overlay_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_embedded_debug_overlay_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME gpu_embedded_debug_overlay_policy
    COMMAND gpu_embedded_debug_overlay_policy_test)

add_executable(gpu_mnk_mouse_button_policy_test gpu_mnk_mouse_button_policy_test.cpp)
target_include_directories(gpu_mnk_mouse_button_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_mnk_mouse_button_policy COMMAND gpu_mnk_mouse_button_policy_test)

add_executable(gpu_mnk_mouse_wheel_policy_test gpu_mnk_mouse_wheel_policy_test.cpp)
target_include_directories(gpu_mnk_mouse_wheel_policy_test PRIVATE
  "${CMAKE_SOURCE_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_mnk_mouse_wheel_policy COMMAND gpu_mnk_mouse_wheel_policy_test)

add_executable(gpu_mnk_mouse_motion_policy_test gpu_mnk_mouse_motion_policy_test.cpp)
target_include_directories(gpu_mnk_mouse_motion_policy_test PRIVATE
  "${CMAKE_SOURCE_DIR}/../../external/ReXGlue/include")
add_test(NAME gpu_mnk_mouse_motion_policy COMMAND gpu_mnk_mouse_motion_policy_test)

add_executable(pc_scene_color_history_test pc_scene_color_history_test.cpp)
target_include_directories(pc_scene_color_history_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME pc_scene_color_history COMMAND pc_scene_color_history_test)

add_executable(pc_draw_transform_history_test pc_draw_transform_history_test.cpp)
target_include_directories(pc_draw_transform_history_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME pc_draw_transform_history COMMAND pc_draw_transform_history_test)

add_executable(pc_sparse_projection_test pc_sparse_projection_test.cpp)
target_include_directories(pc_sparse_projection_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME pc_sparse_projection COMMAND pc_sparse_projection_test)

add_executable(pc_projection_depth_test pc_projection_depth_test.cpp)
target_include_directories(pc_projection_depth_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME pc_projection_depth COMMAND pc_projection_depth_test)

add_executable(pc_constant_writer_test pc_constant_writer_test.cpp)
target_include_directories(pc_constant_writer_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME pc_constant_writer COMMAND pc_constant_writer_test)

add_executable(pc_command_execution_test pc_command_execution_test.cpp)
target_include_directories(pc_command_execution_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME pc_command_execution COMMAND pc_command_execution_test)

add_executable(pc_ring_publication_test pc_ring_publication_test.cpp)
target_include_directories(pc_ring_publication_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
add_test(NAME pc_ring_publication COMMAND pc_ring_publication_test)

add_executable(runtime_owned_camera_source_test runtime_owned_camera_source_test.cpp)
target_include_directories(runtime_owned_camera_source_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
add_test(NAME runtime_owned_camera_source COMMAND runtime_owned_camera_source_test)

add_executable(runtime_guest_bulk_write_test runtime_guest_bulk_write_test.cpp
    generated_bulk_write_bridge_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_mmio.cpp")
target_include_directories(runtime_guest_bulk_write_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../../generated/ppc"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/thirdparty/simde")
target_compile_options(runtime_guest_bulk_write_test PRIVATE /arch:AVX /EHsc)
add_test(NAME runtime_guest_bulk_write COMMAND runtime_guest_bulk_write_test)

add_executable(runtime_guest_atomic_write_test runtime_guest_atomic_write_test.cpp
    generated_atomic_write_bridge_test.cpp)
target_include_directories(runtime_guest_atomic_write_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../../generated/ppc"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/thirdparty/simde")
target_compile_options(runtime_guest_atomic_write_test PRIVATE /arch:AVX /EHsc)
add_test(NAME runtime_guest_atomic_write COMMAND runtime_guest_atomic_write_test)

add_executable(host_write_scope_test host_write_scope_test.cpp)
target_include_directories(host_write_scope_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_options(host_write_scope_test PRIVATE /EHsc)
add_test(NAME host_write_scope COMMAND host_write_scope_test)

add_executable(runtime_allocation_identity_test runtime_allocation_identity_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_memory.cpp")
target_include_directories(runtime_allocation_identity_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_allocation_identity_test PRIVATE /EHsc)
add_test(NAME runtime_allocation_identity COMMAND runtime_allocation_identity_test)

add_executable(runtime_source_write_epochs_test runtime_source_write_epochs_test.cpp)
target_include_directories(runtime_source_write_epochs_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
add_test(NAME runtime_source_write_epochs COMMAND runtime_source_write_epochs_test)

add_executable(runtime_source_memory_coordinator_test runtime_source_memory_coordinator_test.cpp
    runtime_source_memory_coordinator_peer.cpp)
target_include_directories(runtime_source_memory_coordinator_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_source_memory_coordinator_test PRIVATE /EHsc)
add_test(NAME runtime_source_memory_coordinator COMMAND runtime_source_memory_coordinator_test)
add_test(NAME runtime_source_memory_coordinator_disabled COMMAND runtime_source_memory_coordinator_test --disabled)

add_executable(runtime_owned_source_write_watch_test runtime_owned_source_write_watch_test.cpp)
target_include_directories(runtime_owned_source_write_watch_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime")
target_compile_options(runtime_owned_source_write_watch_test PRIVATE /EHsc)
add_test(NAME runtime_owned_source_write_watch COMMAND runtime_owned_source_write_watch_test)

add_executable(runtime_camera_submission_test runtime_camera_submission_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_camera_submission.cpp")
target_include_directories(runtime_camera_submission_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../../generated/ppc"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/XenonRecomp/thirdparty/simde")
target_compile_options(runtime_camera_submission_test PRIVATE /arch:AVX /EHsc)
add_test(NAME runtime_camera_submission COMMAND runtime_camera_submission_test)
add_test(NAME runtime_camera_submission_disabled COMMAND runtime_camera_submission_test --disabled)

add_executable(runtime_owned_packet_write_lease_test runtime_owned_packet_write_lease_test.cpp)
target_include_directories(runtime_owned_packet_write_lease_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_owned_packet_write_lease_test PRIVATE /EHsc)
add_test(NAME runtime_owned_packet_write_lease COMMAND runtime_owned_packet_write_lease_test)
set_tests_properties(runtime_owned_packet_write_lease PROPERTIES TIMEOUT 15)

add_executable(runtime_owned_packet_read_test runtime_owned_packet_read_test.cpp)
target_include_directories(runtime_owned_packet_read_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_owned_packet_read_test PRIVATE /EHsc)
add_test(NAME runtime_owned_packet_read COMMAND runtime_owned_packet_read_test)

add_executable(runtime_owned_camera_constants_test runtime_owned_camera_constants_test.cpp)
target_include_directories(runtime_owned_camera_constants_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_owned_camera_constants_test PRIVATE /EHsc)
add_test(NAME runtime_owned_camera_constants COMMAND runtime_owned_camera_constants_test)

add_executable(runtime_camera_policy_test runtime_camera_policy_test.cpp)
add_test(NAME runtime_camera_policy COMMAND runtime_camera_policy_test)

add_executable(runtime_input_merge_policy_test runtime_input_merge_policy_test.cpp)
target_include_directories(runtime_input_merge_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
add_test(NAME runtime_input_merge_policy COMMAND runtime_input_merge_policy_test)

add_executable(runtime_pc_settings_test runtime_pc_settings_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_pc_settings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_user_data.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_game_setup.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/pc_settings_schema.cpp")
target_include_directories(runtime_pc_settings_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_pc_settings_test PRIVATE /EHsc)
target_compile_definitions(runtime_pc_settings_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(runtime_pc_settings_test PRIVATE bcrypt tomlplusplus::tomlplusplus)
add_test(NAME runtime_pc_settings COMMAND runtime_pc_settings_test)
# 0.9.1 saves and settings per user: layouts, the verified one-time copy from
# a 0.9.0 folder, imports, and paths beyond the system code page.
add_executable(runtime_user_data_test runtime_user_data_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_user_data.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_pc_settings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_game_setup.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/pc_settings_schema.cpp")
target_include_directories(runtime_user_data_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_user_data_test PRIVATE /EHsc)
target_link_libraries(runtime_user_data_test PRIVATE bcrypt tomlplusplus::tomlplusplus)
add_test(NAME runtime_user_data COMMAND runtime_user_data_test)
# 0.9.1 versions of default.xex: a synthetic encrypted XEX2 (no game data),
# damaged files, and the protected .rdata the runtime reads.
add_executable(runtime_xex_identity_test runtime_xex_identity_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_game_setup.cpp")
target_include_directories(runtime_xex_identity_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_definitions(runtime_xex_identity_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_compile_options(runtime_xex_identity_test PRIVATE /EHsc)
target_link_libraries(runtime_xex_identity_test PRIVATE bcrypt tomlplusplus::tomlplusplus)
add_test(NAME runtime_xex_identity COMMAND runtime_xex_identity_test)
# Language pack installs: patches, pack paths, stored zips, whole installs.
add_executable(runtime_language_install_test runtime_language_install_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_language_install.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_game_setup.cpp")
target_include_directories(runtime_language_install_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_language_install_test PRIVATE /EHsc)
target_compile_features(runtime_language_install_test PRIVATE cxx_std_17)
target_link_libraries(runtime_language_install_test PRIVATE bcrypt winhttp tomlplusplus::tomlplusplus)
add_test(NAME runtime_language_install COMMAND runtime_language_install_test)
# V338 measured automatic settings: decision logic with synthetic timings.
add_executable(runtime_gpu_calibration_test runtime_gpu_calibration_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_gpu_calibration_model.cpp")
target_include_directories(runtime_gpu_calibration_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_gpu_calibration_test PRIVATE /EHsc)
target_link_libraries(runtime_gpu_calibration_test PRIVATE tomlplusplus::tomlplusplus)
add_test(NAME runtime_gpu_calibration COMMAND runtime_gpu_calibration_test)

# This test uses the real ReXGlue registry/config loader (and GPU cvar
# registrations), without creating a device, window or guest runtime.
set(SNAPSHOT_REX_RELEASE "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/out/win-amd64/Release")
add_executable(runtime_pc_config_snapshot_test runtime_pc_config_snapshot_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_pc_settings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_user_data.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_game_setup.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_single_instance.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/pc_settings_schema.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_graphics_cache.cpp")
target_include_directories(runtime_pc_config_snapshot_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_features(runtime_pc_config_snapshot_test PRIVATE cxx_std_17)
target_compile_options(runtime_pc_config_snapshot_test PRIVATE /EHsc)
target_link_libraries(runtime_pc_config_snapshot_test PRIVATE
    bcrypt tomlplusplus::tomlplusplus "${SNAPSHOT_REX_RELEASE}/rexruntime.lib")
add_custom_command(TARGET runtime_pc_config_snapshot_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SNAPSHOT_REX_RELEASE}/rexruntime.dll" "$<TARGET_FILE_DIR:runtime_pc_config_snapshot_test>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SNAPSHOT_REX_RELEASE}/rexgpu-xenos.dll" "$<TARGET_FILE_DIR:runtime_pc_config_snapshot_test>")
add_test(NAME runtime_pc_config_snapshot COMMAND runtime_pc_config_snapshot_test)

add_executable(pc_settings_utility_model_test
    pc_settings_utility_model_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/pc_settings_utility_model.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/pc_settings_schema.cpp")
target_include_directories(pc_settings_utility_model_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(pc_settings_utility_model_test PRIVATE /EHsc)
add_test(NAME pc_settings_utility_model COMMAND pc_settings_utility_model_test)

add_executable(runtime_package_validation_test
    runtime_package_validation_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_package_validation.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_graphics_cache.cpp")
target_include_directories(runtime_package_validation_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_package_validation_test PRIVATE /EHsc)
target_link_libraries(runtime_package_validation_test PRIVATE
    bcrypt tomlplusplus::tomlplusplus)
add_test(NAME runtime_package_validation COMMAND runtime_package_validation_test)

add_executable(runtime_mods_test runtime_mods_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_mods.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_filesystem.cpp")
target_include_directories(runtime_mods_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_mods_test PRIVATE /EHsc)
target_link_libraries(runtime_mods_test PRIVATE tomlplusplus::tomlplusplus)
add_test(NAME runtime_mods COMMAND runtime_mods_test)

add_executable(pc_config_contract_test pc_config_contract_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/pc_settings_schema.cpp")
target_compile_definitions(pc_config_contract_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_include_directories(pc_config_contract_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_link_libraries(pc_config_contract_test PRIVATE tomlplusplus::tomlplusplus)
add_test(NAME pc_config_contract COMMAND pc_config_contract_test)

set(DARKNESS_TEST_REXGLUE "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue")
set(DARKNESS_TEST_IMGUI "${DARKNESS_TEST_REXGLUE}/thirdparty/imgui")
add_executable(settings_ui_schema_test settings_ui_schema_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/pc_settings_schema.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/pc_settings_ui.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/pc_settings_arabic.cpp"
    "${DARKNESS_TEST_REXGLUE}/src/ui/settings_schema.cpp"
    "${DARKNESS_TEST_REXGLUE}/src/ui/settings_panel.cpp"
    "${DARKNESS_TEST_IMGUI}/imgui.cpp" "${DARKNESS_TEST_IMGUI}/imgui_draw.cpp"
    "${DARKNESS_TEST_IMGUI}/imgui_tables.cpp" "${DARKNESS_TEST_IMGUI}/imgui_widgets.cpp"
    "${DARKNESS_TEST_IMGUI}/backends/imgui_impl_null.cpp")
target_compile_definitions(settings_ui_schema_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_include_directories(settings_ui_schema_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime" "${DARKNESS_TEST_REXGLUE}/include"
    "${DARKNESS_TEST_IMGUI}" "${DARKNESS_TEST_IMGUI}/backends")
target_compile_features(settings_ui_schema_test PRIVATE cxx_std_20)
target_link_libraries(settings_ui_schema_test PRIVATE tomlplusplus::tomlplusplus)
add_test(NAME settings_ui_schema COMMAND settings_ui_schema_test)

add_executable(runtime_graphics_cache_test runtime_graphics_cache_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_graphics_cache.cpp")
target_include_directories(runtime_graphics_cache_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_graphics_cache_test PRIVATE /EHsc)
target_link_libraries(runtime_graphics_cache_test PRIVATE bcrypt)
add_test(NAME runtime_graphics_cache COMMAND runtime_graphics_cache_test)

add_executable(gpu_graphics_cache_identity_policy_test
    gpu_graphics_cache_identity_policy_test.cpp)
target_compile_definitions(gpu_graphics_cache_identity_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME gpu_graphics_cache_identity_policy
    COMMAND gpu_graphics_cache_identity_policy_test)

add_executable(runtime_single_instance_test
    runtime_single_instance_test.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime/runtime_single_instance.cpp")
target_include_directories(runtime_single_instance_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_definitions(runtime_single_instance_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME runtime_single_instance COMMAND runtime_single_instance_test)

# Packaging an unoptimized nominal Release executable makes the guest CPU miss
# real-time audio and presentation deadlines. Protect the target-local build
# contract independently of toolchain-populated CMake cache defaults.
add_executable(runtime_release_policy_test runtime_release_policy_test.cpp)
target_compile_definitions(runtime_release_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
darkness_local_test(runtime_release_policy runtime_release_policy_test
    scripts/get-rexglue-source-identity.ps1 scripts/verify-pc-phase1-candidate.ps1)

add_executable(runtime_frame_cadence_policy_test runtime_frame_cadence_policy_test.cpp)
add_executable(gpu_cp_cadence_test gpu_cp_cadence_test.cpp)
target_include_directories(gpu_cp_cadence_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_definitions(gpu_cp_cadence_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME gpu_cp_cadence COMMAND gpu_cp_cadence_test)
target_include_directories(runtime_frame_cadence_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_definitions(runtime_frame_cadence_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_compile_options(runtime_frame_cadence_policy_test PRIVATE /EHsc)
add_test(NAME runtime_frame_cadence_policy COMMAND runtime_frame_cadence_policy_test)

add_executable(runtime_crash_reporting_policy_test runtime_crash_reporting_policy_test.cpp)
target_compile_definitions(runtime_crash_reporting_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
add_test(NAME runtime_crash_reporting_policy COMMAND runtime_crash_reporting_policy_test)

add_executable(runtime_validation_launcher_policy_test
    runtime_validation_launcher_policy_test.cpp)
target_compile_definitions(runtime_validation_launcher_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
darkness_local_test(runtime_validation_launcher_policy runtime_validation_launcher_policy_test
    scripts/start-pc-phase1-validation-probe.ps1)

add_executable(runtime_autonomous_matrix_policy_test
    runtime_autonomous_matrix_policy_test.cpp)
target_compile_definitions(runtime_autonomous_matrix_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
darkness_local_test(runtime_autonomous_matrix_policy runtime_autonomous_matrix_policy_test
    scripts/run-pc-phase1-autonomous-matrix.ps1)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../scripts/_probe_input_observation.ps1")
    add_test(NAME runtime_probe_input_observation
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_LIST_DIR}/runtime_probe_input_observation_test.ps1")
endif()

add_executable(runtime_visual_regression_policy_test
    runtime_visual_regression_policy_test.cpp)
target_compile_definitions(runtime_visual_regression_policy_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
darkness_local_test(runtime_visual_regression_policy runtime_visual_regression_policy_test
    config/pc_visual_references_v1.json scripts/compare-visual-capture.ps1
    scripts/detect-visual-artifact.ps1 scripts/detect-visual-checkpoint.ps1
    scripts/evaluate-visual-regression.ps1)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../scripts/evaluate-visual-regression.ps1")
    add_test(NAME runtime_visual_regression_behavior
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_LIST_DIR}/runtime_visual_regression_behavior_test.ps1")
endif()

add_executable(runtime_owned_camera_packets_test runtime_owned_camera_packets_test.cpp)
target_include_directories(runtime_owned_camera_packets_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_owned_camera_packets_test PRIVATE /EHsc)
add_test(NAME runtime_owned_camera_packets COMMAND runtime_owned_camera_packets_test)

add_executable(runtime_owned_retirement_test runtime_owned_retirement_test.cpp)
target_include_directories(runtime_owned_retirement_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_options(runtime_owned_retirement_test PRIVATE /EHsc)
add_test(NAME runtime_owned_retirement COMMAND runtime_owned_retirement_test)

add_executable(runtime_owned_camera_mode_test runtime_owned_camera_mode_test.cpp)
target_include_directories(runtime_owned_camera_mode_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
add_test(NAME runtime_owned_camera_mode COMMAND runtime_owned_camera_mode_test)

# V380: fatal stops leave a report; the guest printf family formats.
add_executable(runtime_fatal_exit_test runtime_fatal_exit_test.cpp)
target_compile_definitions(runtime_fatal_exit_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_compile_features(runtime_fatal_exit_test PRIVATE cxx_std_17)
darkness_local_test(runtime_fatal_exit runtime_fatal_exit_test
    scripts/start-isolated-gameplay-probe.ps1)

# V385: the frame-start wait hook still matches the generated sub_825A4278.
add_executable(runtime_frame_wait_test runtime_frame_wait_test.cpp)
target_compile_definitions(runtime_frame_wait_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_compile_features(runtime_frame_wait_test PRIVATE cxx_std_17)
add_test(NAME runtime_frame_wait COMMAND runtime_frame_wait_test)

# V390: right-stick look emitter hook (every frame up to 300 FPS, the title's
# turn per cadence window) still matches the generated emitter and look().
add_executable(runtime_stick_look_test runtime_stick_look_test.cpp)
target_compile_definitions(runtime_stick_look_test PRIVATE
    DARKNESS_SOURCE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_compile_features(runtime_stick_look_test PRIVATE cxx_std_17)
add_test(NAME runtime_stick_look COMMAND runtime_stick_look_test)

# V397: temporal AA policy (camera from the world draws' constants, vote,
# reprojection, cuts, jitter) against captured street camera constants.
add_executable(gpu_temporal_aa_policy_test gpu_temporal_aa_policy_test.cpp)
target_include_directories(gpu_temporal_aa_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_features(gpu_temporal_aa_policy_test PRIVATE cxx_std_17)
add_test(NAME gpu_temporal_aa_policy COMMAND gpu_temporal_aa_policy_test)

# V404: Arabic shaping and right-to-left order for the settings UI
# (rex/ui/rtl_text.h).
add_executable(ui_rtl_text_test ui_rtl_text_test.cpp)
target_include_directories(ui_rtl_text_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_features(ui_rtl_text_test PRIVATE cxx_std_17)
add_test(NAME ui_rtl_text COMMAND ui_rtl_text_test)

# V407: widescreen guest mode sizes (runtime/runtime_widescreen_policy.h).
add_executable(runtime_widescreen_policy_test runtime_widescreen_policy_test.cpp)
target_include_directories(runtime_widescreen_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_features(runtime_widescreen_policy_test PRIVATE cxx_std_17)
add_test(NAME runtime_widescreen_policy COMMAND runtime_widescreen_policy_test)

# V504 (#16): the title's MSAA mode switch (runtime/runtime_msaa_mode_policy.h).
add_executable(runtime_msaa_mode_policy_test runtime_msaa_mode_policy_test.cpp)
target_include_directories(runtime_msaa_mode_policy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
target_compile_features(runtime_msaa_mode_policy_test PRIVATE cxx_std_17)
add_test(NAME runtime_msaa_mode_policy COMMAND runtime_msaa_mode_policy_test)

# V409: in-game language-pack text (runtime/runtime_language_text.h).
add_executable(runtime_language_text_test runtime_language_text_test.cpp)
target_include_directories(runtime_language_text_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../../external/ReXGlue/include")
target_compile_features(runtime_language_text_test PRIVATE cxx_std_17)
add_test(NAME runtime_language_text COMMAND runtime_language_text_test)
