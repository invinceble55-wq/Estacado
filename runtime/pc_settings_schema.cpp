#include "pc_settings_schema.h"
#include "runtime_camera_policy.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <locale>

const std::vector<PcEditableSettingSpec>& PcEditableSettingsSchema() {
    static const std::vector<PcEditableSettingSpec> schema = {
        {"monitor", L"Display", L"Display", PcSettingEditorKind::Integer,
         0.0, 16.0, 1.0, {}},
        {"output_resolution", L"Display", L"Output resolution",
         PcSettingEditorKind::Resolution, 0.0, 0.0, 0.0,
         {{"native", L"Native desktop"}, {"720p", L"1280 x 720"},
          {"1080p", L"1920 x 1080"}, {"1440p", L"2560 x 1440"},
          {"4k", L"3840 x 2160"}}},
        {"window_mode", L"Display", L"Window mode",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"auto", L"Automatic"}, {"windowed", L"Windowed"},
          {"borderless", L"Borderless"}}},
        // V407: fill non-16:9 screens (runtime_widescreen.h), next start.
        {"display.widescreen", L"Display", L"Fill wide and tall screens",
         PcSettingEditorKind::Boolean, 0.0, 1.0, 1.0, {}},
        {"display.present_mode", L"Performance / Frame Rate", L"VSync",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"vsync", L"On (recommended)"}, {"immediate", L"Off"},
          {"vrr", L"Off, falling back to On without tearing support", true}}},
        // V490: the frame-queue limit for players (rex/ui/frame_latency.h).
        {"display.low_latency", L"Performance / Frame Rate", L"Low latency",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"on", L"On (recommended)"}, {"off", L"Off"}}},
        {"display.max_frame_latency", L"Advanced", L"Frame queue",
         PcSettingEditorKind::Integer, 1.0, 3.0, 1.0, {}},
        // Surfaces replace this list with plain numbers built from the
        // display's refresh (rex/ui/settings_detection.h); these stay valid.
        {"display.frame_rate", L"Performance / Frame Rate", L"Frame rate",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"original", L"30 (Original)"},
          {"60", L"60"},
          {"half_refresh", L"Half the display refresh"},
          {"120", L"120"},
          {"144", L"144"},
          {"refresh", L"Display refresh"},
          {"uncapped", L"Uncapped (with VSync Off)"},
          {"custom", L"Custom cap (Advanced)", true}}},
        {"display.frame_limit", L"Performance / Frame Rate",
         L"Custom frame cap (FPS, 0 = off)",
         PcSettingEditorKind::Integer, 0.0, 240.0, 1.0, {}},
        {"display.menu_frame_rate", L"Performance / Frame Rate", L"Menu frame rate",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"reduced", L"Reduced (saves power)"}, {"full", L"Same as gameplay"}}},
        {"resolution_scale", L"Graphics Quality", L"Internal scale",
         PcSettingEditorKind::IntegerChoice, 0.0, 7.0, 1.0,
         {{"0", L"Automatic"},
          {"1", L"1x - 1280 x 720"},
          {"2", L"2x - 2560 x 1440"},
          {"3", L"3x - 3840 x 2160"},
          {"4", L"4x - 5120 x 2880", true},
          {"5", L"5x - 6400 x 3600", true},
          {"6", L"6x - 7680 x 4320", true},
          {"7", L"7x - 8960 x 5040", true}}},
        {"swap_post_effect", L"Graphics Quality", L"Anti-aliasing",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"none", L"Off"}, {"fxaa", L"FXAA"},
          {"fxaa_extreme", L"FXAA Extreme"}, {"smaa", L"SMAA"}}},
        // V504 (#16): the title's scene MSAA (runtime_msaa_mode_policy.h).
        {"graphics.msaa_mode", L"Graphics Quality", L"Multisampling (MSAA)",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"4x", L"Always 4x (recommended)"}, {"auto", L"Automatic (game)"},
          {"2x", L"Always 2x"}}},
        // V508 (#16): the image-filter native-grid rules (ReXGlue
        // graphics_glow_reconstruction, live in the overlay). 0.9.6: On is
        // the test builds' Test A ("dedicated", upgraded to "on").
        {"graphics.glow_reconstruction", L"Graphics Quality", L"Glow reconstruction",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"on", L"On (recommended)"}, {"off", L"Off"}}},
        // ReXGlue's present_effect (FSR 1 / CAS need no SDK).
        {"present.effect", L"Graphics Quality", L"Upscaling",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"auto", L"Automatic"}, {"fsr", L"AMD FSR 1"},
          {"bilinear", L"Off (bilinear)"},
          {"cas", L"AMD CAS sharpening", true}}},
        {"anisotropic_override", L"Graphics Quality", L"Anisotropic filtering",
         PcSettingEditorKind::IntegerChoice, -1.0, 5.0, 1.0,
         {{"-1", L"Title-selected"}, {"0", L"Off"}, {"1", L"1x"},
          {"2", L"2x"}, {"3", L"4x"}, {"4", L"8x"},
          {"5", L"16x"}}},
        {"graphics.motion_blur", L"Graphics Quality", L"Motion blur",
         PcSettingEditorKind::Boolean, 0.0, 1.0, 1.0, {}},
        // V374: offered only with a pack installed (settings_detection.h).
        {"graphics.hd_textures", L"Graphics Quality", L"HD texture packs",
         PcSettingEditorKind::Boolean, 0.0, 1.0, 1.0, {}},
        // V403: temporal AA on the scene (ReXGlue graphics_temporal_aa, next start).
        {"graphics.temporal_aa", L"Graphics Quality", L"Temporal anti-aliasing",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"off", L"Off"}, {"taa", L"TAA"}, {"dlss", L"NVIDIA DLAA"},
          {"fsr", L"AMD FSR 3.1 (native AA)"}, {"xess", L"Intel XeSS (native AA)"}}},
        // V376: horizontal degrees at 16:9 (the title's 70 at 4:3 = 86). The
        // pre-V376 camera.gameplay_fov never changed the view; old files keep
        // loading with it (ignored).
        {"camera.field_of_view", L"Camera", L"Field of view (degrees)",
         PcSettingEditorKind::Number, 60.0, 120.0, 1.0, {}, true,
         kOriginalGameplayFovDegrees, 1.0, L"degrees"},
        {"input.keyboard_mouse", L"Controls", L"Keyboard / mouse",
         PcSettingEditorKind::Boolean, 0.0, 1.0, 1.0, {}},
        {"input.mouse_look", L"Controls", L"Mouse look",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"native", L"Native (1:1)"}, {"stick", L"Right-stick bridge"}}},
        {"input.controller_sensitivity", L"Controls",
         L"Controller sensitivity", PcSettingEditorKind::Choice,
         0.0, 0.0, 0.0,
         {{"medium", L"Medium"}, {"low", L"Low"},
          {"high", L"High"}}},
        {"input.controller_invert_y", L"Controls", L"Invert controller Y",
         PcSettingEditorKind::Boolean, 0.0, 1.0, 1.0, {}},
        {"input.vibration_scale", L"Controls", L"Vibration strength",
         PcSettingEditorKind::Number, 0.0, 1.0, 0.1, {}, true, 1.0, 100.0, L"%"},
        {"audio.master_volume", L"Audio", L"Master volume",
         PcSettingEditorKind::Number, 0.0, 1.0, 0.01, {}, true, 1.0, 100.0, L"%"},
        {"input.mouse_sensitivity", L"Controls", L"Mouse sensitivity",
         PcSettingEditorKind::Number, 0.01, 10.0, 0.01, {}, true, 1.0},
        {"input.mouse_invert_y", L"Controls", L"Invert mouse Y",
         PcSettingEditorKind::Boolean, 0.0, 1.0, 1.0, {}},
        // V404: its own section (first), with Arabic for the interface (the
        // game itself stays English until an Arabic language pack exists).
        {"general.language", L"Language", L"Language (\u0627\u0644\u0644\u063a\u0629)",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"auto", L"Automatic (Windows language)"},
          {"english", L"English"}, {"german", L"German"},
          {"french", L"French"}, {"spanish", L"Spanish"},
          {"italian", L"Italian"},
          {"arabic", L"\u0627\u0644\u0639\u0631\u0628\u064a\u0629 (Arabic)"}}},
        {"input.keyboard_mouse_user_index", L"Controls", L"Mouse/keyboard slot (0-3)",
         PcSettingEditorKind::IntegerChoice, 0.0, 3.0, 1.0,
         {{"0", L"0 - Player 1"}, {"1", L"1 - Player 2"},
          {"2", L"2 - Player 3"}, {"3", L"3 - Player 4"}}},
        {"input.mouse_acceleration", L"Controls", L"Stick-bridge acceleration",
         PcSettingEditorKind::Number, 0.0, 1.0, 0.01, {}, true, 0.0},
        {"input.mouse_smoothing", L"Controls", L"Stick-bridge smoothing",
         PcSettingEditorKind::Number, 0.0, 1.0, 0.01, {}, true, 0.0},
        {"input.overlay_key", L"Controls", L"Settings overlay key",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"F1", L"F1"}, {"F2", L"F2"}, {"F4", L"F4"}, {"F5", L"F5"},
          {"F6", L"F6"}, {"F7", L"F7"}, {"F8", L"F8"}, {"F11", L"F11"}}},
        // 0.9.1: prompts follow the device of the latest input (ReXGlue
        // prompt_icons.h, runtime_button_prompts.h).
        {"input.button_prompts", L"Controls", L"Button prompts",
         PcSettingEditorKind::Choice, 0.0, 0.0, 0.0,
         {{"auto", L"Automatic"}, {"xbox", L"Xbox buttons"},
          {"keyboard", L"Keyboard keys"}}},
        // Keyboard/mouse bindings of the controller the game sees (its
        // prompts show Xbox buttons). Values: rex::ui::settings binding names.
        {"input.bind.lstick_up", L"Key bindings", L"Move forward (left stick up)",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.lstick_down", L"Key bindings", L"Move back (left stick down)",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.lstick_left", L"Key bindings", L"Move left (left stick left)",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.lstick_right", L"Key bindings", L"Move right (left stick right)",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.a", L"Key bindings", L"A button: use", PcSettingEditorKind::Key,
         0.0, 0.0, 0.0, {}},
        {"input.bind.b", L"Key bindings", L"B button: reload", PcSettingEditorKind::Key,
         0.0, 0.0, 0.0, {}},
        {"input.bind.x", L"Key bindings", L"X button: redirect", PcSettingEditorKind::Key,
         0.0, 0.0, 0.0, {}},
        {"input.bind.y", L"Key bindings", L"Y button: jump", PcSettingEditorKind::Key,
         0.0, 0.0, 0.0, {}},
        {"input.bind.right_trigger", L"Key bindings", L"Right trigger (RT): fire right hand",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.left_trigger", L"Key bindings", L"Left trigger (LT): fire left hand",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.right_shoulder", L"Key bindings", L"Right bumper (RB): Darkness power",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.left_shoulder", L"Key bindings", L"Left bumper (LB): manifest the Darkness",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.lstick_press", L"Key bindings", L"Left stick click (LS): crouch",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.rstick_press", L"Key bindings", L"Right stick click (RS): aim",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.dpad_up", L"Key bindings", L"D-pad up: Darkness powers",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.dpad_down", L"Key bindings", L"D-pad down: Darkness powers",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.dpad_left", L"Key bindings", L"D-pad left: weapons",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.dpad_right", L"Key bindings", L"D-pad right: weapons",
         PcSettingEditorKind::Key, 0.0, 0.0, 0.0, {}},
        {"input.bind.back", L"Key bindings", L"Back button: journal", PcSettingEditorKind::Key,
         0.0, 0.0, 0.0, {}},
        {"input.bind.start", L"Key bindings", L"Start button: pause", PcSettingEditorKind::Key,
         0.0, 0.0, 0.0, {}},
        {"input.bind.guide", L"Key bindings", L"Guide button", PcSettingEditorKind::Key,
         0.0, 0.0, 0.0, {}},
    };
    return schema;
}

const PcEditableSettingSpec* FindPcEditableSetting(std::string_view key) {
    const auto& schema = PcEditableSettingsSchema();
    const auto iterator = std::find_if(
        schema.begin(), schema.end(),
        [&](const PcEditableSettingSpec& setting) { return setting.key == key; });
    return iterator == schema.end() ? nullptr : &*iterator;
}

const PcSettingPresentation& PcSettingPresentationFor(std::string_view key) {
    struct Entry {
        std::string_view key;
        PcSettingPresentation presentation;
    };
    static const Entry entries[] = {
        {"monitor", {L"Which display the game opens on.", {}, {}, false, false}},
        {"output_resolution",
         {L"Size of the game image on screen. Native uses your desktop resolution.",
          {}, {}, false, false}},
        {"window_mode",
         {L"Borderless fills the screen without changing display modes; Windowed opens a "
          L"movable window.", {}, {}, false, false}},
        {"display.widescreen",
         {L"Off keeps the original 16:9 picture, with black bars on wider or taller screens. "
          L"On renders a wider picture on 21:9 and 32:9 screens (more view at the sides) or a "
          L"taller one on 16:10, so it fills the screen. No change on 16:9 screens. Applies at "
          L"the next start.", {}, {}, false, false}},
        {"display.present_mode",
         {L"On waits for the display: no tearing, and G-SYNC/FreeSync displays still follow "
          L"the frame rate. Off shows each frame at once: lowest latency, may tear; needed "
          L"for Uncapped.", {}, {}, false, false}},
        {"display.low_latency",
         {L"On keeps the game at most two frames ahead of the display, so the picture "
          L"answers the controls sooner: about a third less delay at 144 Hz with the same "
          L"frame rate. At 60 FPS the game is never further ahead, so nothing changes there, "
          L"and with VSync Off frames are shown at once, so it does nothing. Off lets more "
          L"frames queue up.", {}, {}, false, true}},
        {"display.max_frame_latency",
         {L"Frames that may wait for the display. 1 responds fastest; 2 evens out uneven "
          L"frames.", {}, {}, true, false}},
        {"display.frame_rate",
         {L"Game speed is the same at every frame rate. With VSync, a rate that divides your "
          L"monitor's refresh moves evenly; other rates are even only on G-SYNC/FreeSync "
          L"displays. 30 (Original) keeps the console's pacing.",
          {}, {}, false, false}},
        {"display.frame_limit",
         {L"Highest frame rate for the Custom setting (0 = no cap).",
          "display.frame_rate", "custom", true, false}},
        {"display.menu_frame_rate",
         {L"Menus, the title screen and loading run at up to about 60 FPS (48 on a 144 Hz "
          L"monitor). The menu backdrop costs more graphics work per frame than gameplay, so "
          L"this saves power, heat and fan noise.", {}, {}, false, true}},
        {"resolution_scale",
         {L"Renders the 3D scene at a multiple of the console's 1280 x 720, then fits it to "
          L"your screen (2x on a 1080p screen is supersampling). Automatic times your graphics "
          L"card on the title screen once, then picks the highest scale that keeps the busiest "
          L"street at your recommended frame rate (60-120 FPS); until then it guesses from video "
          L"memory. With Upscaling on Automatic, a scale below your screen uses AMD FSR 1. 3x "
          L"costs about twice 2x on the graphics card (about 100 FPS on a current high-end card "
          L"in the busiest street).",
          {}, {}, false, false}},
        {"swap_post_effect",
         {L"Smooths jagged edges after rendering. FXAA Extreme is smoother but softer; SMAA "
          L"finds real edges and keeps textures and text sharper.",
          {}, {}, false, false}},
        {"present.effect",
         {L"How the rendered image is fitted to your screen. AMD FSR 1 upscales with sharp "
          L"edges and runs on any graphics card; it also sharpens an image that already "
          L"matches the screen. Automatic uses FSR 1 only when upscaling (for example 1x "
          L"internal scale on a 1440p screen) and leaves a 1:1 image untouched. Off stretches "
          L"with plain bilinear filtering.",
          {}, {}, false, true}},
        {"anisotropic_override",
         {L"Keeps textures sharp at glancing angles. 16x is recommended.",
          {}, {}, false, false}},
        {"graphics.msaa_mode",
         {L"The game's own edge smoothing of the 3D scene. The console dropped from 4x to 2x "
          L"when it ran short of graphics time; on PC that switch also reacts to loading "
          L"hitches and slow menus (37.5 FPS on a 75 Hz screen) and can stay on 2x. Always 4x "
          L"keeps the console's quality and, above internal scale 1x, is also the faster mode "
          L"on PC. Always 2x is faster only at 1x. Automatic is the game's own switch.",
          {}, {}, false, true}},
        {"graphics.glow_reconstruction",
         {L"Above internal scale 1x, keeps the glow around lights and in bright flashes as "
          L"smooth as on the console. Off draws the glow as in 0.9.0, with a little banding in "
          L"bright flashes; use it if the glow looks wrong on your graphics card.",
          {}, {}, false, true}},
        {"graphics.motion_blur", {L"The game's camera motion blur.", {}, {}, false, false}},
        {"graphics.temporal_aa",
         {L"Removes shimmering and jagged edges by combining several frames, at the internal "
          L"scale (no upscaling). TAA is built in and works on any graphics card. NVIDIA DLAA "
          L"uses NVIDIA's AI model on GeForce RTX cards. AMD FSR 3.1 and Intel XeSS run on "
          L"most modern cards. If the chosen one can't start, TAA is used. Best with "
          L"Anti-aliasing set to Off. Applies at the next start.",
          {}, {}, false, false}},
        {"graphics.hd_textures",
         {L"Shows an installed HD texture pack instead of the game's own textures. Packs are "
          L"made by players and don't come with the game. The whole pack loads in the "
          L"background from the start (540 MB took under 2 seconds from an NVMe SSD; each "
          L"original shows until its replacement is ready) and then stays in video memory. "
          L"Off loads nothing: the game looks and runs as before.",
          {}, {}, false, false}},
        {"camera.field_of_view",
         {L"How wide the view is, in horizontal degrees on a 16:9 screen (wider screens see "
          L"more at the sides). The original is 86. Zooming keeps its magnification.",
          {}, {}, false, false}},
        {"input.keyboard_mouse",
         {L"Play with keyboard and mouse. A connected controller keeps working.",
          {}, {}, false, false}},
        {"input.mouse_look",
         {L"Native turns the camera directly with the mouse (recommended). Right-stick "
          L"bridge sends the mouse through the controller stick, with its dead zone and "
          L"ramp.", "input.keyboard_mouse", "true", false, false}},
        {"input.mouse_sensitivity",
         {L"Mouse turning speed. 1.0 turns 0.066 degrees per mouse count.",
          "input.keyboard_mouse", "true", false, true}},
        {"input.mouse_invert_y",
         {L"Moving the mouse forward looks down.", "input.keyboard_mouse", "true",
          false, true}},
        {"input.mouse_acceleration",
         {L"Extra speed for fast flicks through the right-stick bridge.",
          "input.mouse_look", "stick", true, false}},
        {"input.mouse_smoothing",
         {L"Blends each mouse step with the previous one in the right-stick bridge.",
          "input.mouse_look", "stick", true, false}},
        {"input.keyboard_mouse_user_index",
         {L"Which controller slot the keyboard and mouse play as.",
          "input.keyboard_mouse", "true", true, false}},
        {"input.controller_sensitivity",
         {L"The game's own controller look sensitivity.", {}, {}, false, false}},
        {"input.controller_invert_y",
         {L"Pushing the right stick forward looks down.", {}, {}, false, false}},
        {"input.vibration_scale", {L"Controller rumble strength.", {}, {}, false, true}},
        {"input.overlay_key",
         {L"Opens these settings during play (Esc also closes them).", {}, {}, false, false}},
        {"input.button_prompts",
         {L"The buttons the game's prompts show. Automatic shows the keys you bound while you "
          L"play with keyboard and mouse, and Xbox buttons again as soon as you use a "
          L"controller.",
          {}, {}, false, true}},
        {"audio.master_volume", {L"Overall game volume.", {}, {}, false, true}},
        {"general.language",
         {L"Language of the game's text and speech and of these settings. Automatic follows "
          L"the Windows display language, like the console's dashboard language (English "
          L"when the game has no such language). Arabic shows these settings in Arabic; the "
          L"game's speech stays English, and its text becomes Arabic when an Arabic language "
          L"pack is installed.", {}, {}, false, false}},
    };
    static const PcSettingPresentation none{};
    for (const Entry& entry : entries) {
        if (entry.key == key) return entry.presentation;
    }
    // Key bindings: shown while keyboard/mouse is on, applied at the next
    // start (the input thread reads them; no live string swap).
    static const PcSettingPresentation firstBinding{
        L"The game's prompts show Xbox buttons; these are the keys and mouse buttons that "
        L"press them, each named with its action in the game's default controller "
        L"layout. Click a binding, then press a key or mouse button (F1-F12 are "
        L"reserved).",
        "input.keyboard_mouse", "true", false, false};
    static const PcSettingPresentation binding{{}, "input.keyboard_mouse", "true", false, false};
    static const PcSettingPresentation guideBinding{
        L"The Xbox Guide button; the game does not use it.", "input.keyboard_mouse", "true",
        true, false};
    if (key == "input.bind.lstick_up") return firstBinding;
    if (key == "input.bind.guide") return guideBinding;
    if (key.substr(0, 11) == "input.bind.") return binding;
    return none;
}

std::vector<std::wstring_view> PcEditableSettingsSections() {
    std::vector<std::wstring_view> sections{L"Language", L"Display", L"Graphics Quality",
        L"Performance / Frame Rate", L"Camera", L"Controls", L"Key bindings", L"Audio",
        L"Advanced"};
    for (const auto& setting : PcEditableSettingsSchema()) {
        if (std::find(sections.begin(), sections.end(), setting.section) == sections.end()) {
            sections.push_back(setting.section);
        }
    }
    return sections;
}

std::optional<double> ParsePcDisplayedNumber(const PcEditableSettingSpec& spec,
                                            std::wstring_view text) {
    std::wistringstream stream{std::wstring(text)};
    stream.imbue(std::locale::classic());
    double displayed = 0;
    stream >> std::noskipws >> displayed;
    if (!stream || stream.peek() != std::char_traits<wchar_t>::eof() ||
        !std::isfinite(displayed) || spec.displayMultiplier <= 0) return {};
    const double value = displayed / spec.displayMultiplier;
    if (value < spec.minimum || value > spec.maximum) return {};
    return value;
}

int PcSliderStepCount(const PcEditableSettingSpec& spec) {
    if (spec.step <= 0 || spec.maximum < spec.minimum) return 0;
    return static_cast<int>(std::ceil((spec.maximum - spec.minimum) / spec.step));
}
double PcSliderValue(const PcEditableSettingSpec& spec, int position) {
    return std::min(spec.maximum, spec.minimum +
        std::clamp(position, 0, PcSliderStepCount(spec)) * spec.step);
}
int PcSliderPosition(const PcEditableSettingSpec& spec, double value) {
    if (!std::isfinite(value) || spec.step <= 0) return 0;
    return std::clamp(static_cast<int>(std::lround((value - spec.minimum) / spec.step)),
                      0, PcSliderStepCount(spec));
}
std::wstring FormatPcDisplayedNumber(const PcEditableSettingSpec& spec, double value) {
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(10) << value * spec.displayMultiplier;
    return stream.str();
}
