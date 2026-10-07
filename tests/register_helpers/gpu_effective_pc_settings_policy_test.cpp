#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  const std::string source_root = REXGLUE_SOURCE_ROOT;
  std::ifstream source_file(source_root + "/src/graphics/plugin_main.cpp",
                            std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(source_file)),
                           std::istreambuf_iterator<char>());
  if (source.empty()) {
    std::cerr << "embedded GPU plugin source was not readable\n";
    return 1;
  }

  const size_t apply_environment = source.find("rex::graphics::LoadEmbeddedPcConfig(");
  const size_t record = source.find("REX_PC_SETTINGS_EFFECTIVE");
  const size_t finalize = source.find("rex::cvar::FinalizeInit();", record);
  const size_t replacement_path_resolver =
      source.find("ResolveEmbeddedShaderReplacementPackPath");
  const size_t packaged_preset_fallback =
      source.find("asset_root_utf8");
  const size_t replacement_success_record =
      source.find("REX_MOTION_BLUR_PACK result=1");
  const size_t replacement_failure_record =
      source.find("REX_MOTION_BLUR_PACK result=0");
  const bool exactly_one_record =
      record != std::string::npos &&
      source.find("REX_PC_SETTINGS_EFFECTIVE", record + 1) == std::string::npos;
  const char* required_settings[] = {
      "window_mode", "output_resolution", "resolution_scale",
      "draw_resolution_scale_threshold",
      "swap_post_effect", "anisotropic_override", "graphics_motion_blur",
      "graphics_glow_reconstruction",
      "display_present_mode", "display_max_frame_latency",
      "display_frame_limit", "display_frame_rate", "display_vsync_interval",
      "camera_field_of_view", "input_keyboard_mouse",
      "input_keyboard_mouse_user_index", "input_mouse_acceleration",
      "input_mouse_smoothing", "input_mouse_look"};
  bool fields_present = true;
  for (const char* setting : required_settings) {
    const std::string lookup =
        std::string("effective_setting(\"") + setting + "\")";
    fields_present &= source.find(lookup, record) != std::string::npos;
  }

  const bool passed = apply_environment != std::string::npos &&
                      record != std::string::npos &&
                      finalize != std::string::npos && exactly_one_record &&
                      apply_environment < record && record < finalize &&
                      fields_present && replacement_path_resolver != std::string::npos &&
                      packaged_preset_fallback != std::string::npos &&
                      replacement_success_record != std::string::npos &&
                      replacement_failure_record != std::string::npos;
  if (!passed) {
    std::cerr << "effective PC settings snapshot ordering or coverage regressed\n";
    return 1;
  }

  std::cout << "Effective PC settings snapshot policy passed\n";
  return 0;
}
