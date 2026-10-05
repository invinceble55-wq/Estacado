#include "pc_settings_ui.h"

#include "pc_settings_arabic.h"
#include "pc_settings_schema.h"

#include <rex/ui/settings_detection.h>

#include <cstdint>
#include <cstdlib>
#include <system_error>

namespace {

// general.language without Arabic: the settings stay English.
constexpr std::wstring_view kLanguageDescription =
    L"Language of the game's text and speech. Automatic follows the Windows display language, "
    L"like the console's dashboard language (English when the game has no such language).";

}  // namespace

bool PcExperimentalFeature(std::string_view name) {
    const char* list = std::getenv("DARKNESS_EXPERIMENTAL");
    if (!list || name.empty()) return false;
    std::string_view rest(list);
    while (!rest.empty()) {
        const size_t end = rest.find_first_of(", ;");
        if (rest.substr(0, end) == name) return true;
        if (end == std::string_view::npos) break;
        rest.remove_prefix(end + 1);
    }
    return false;
}

PcSettingsOffer PcSettingsOfferFor(const std::filesystem::path& gameFolder, bool downloadable) {
    return PcSettingsOfferFor(gameFolder, gameFolder, downloadable);
}

PcSettingsOffer PcSettingsOfferFor(const std::filesystem::path& dataFolder,
                                   const std::filesystem::path& programFolder,
                                   bool downloadable) {
    PcSettingsOffer offer;
    offer.temporalAa = PcExperimentalFeature("temporal_aa");
    rex::ui::settings::LanguagePackFolder installed =
        rex::ui::settings::ScanLanguagePackFolder(dataFolder / L"language_packs" / L"arabic");
    if (!(installed.strings && installed.fonts) && programFolder != dataFolder) {
        // A pack installed beside the executables by an earlier version.
        installed = rex::ui::settings::ScanLanguagePackFolder(programFolder / L"language_packs" /
                                                              L"arabic");
    }
    std::error_code error;
    offer.arabic = (installed.strings && installed.fonts) || downloadable ||
                   std::filesystem::is_regular_file(
                       programFolder / L"language_packs" / L"arabic_language_pack.zip", error) ||
                   PcExperimentalFeature("arabic");
    return offer;
}

std::string PcSettingsUtf8(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t code = uint32_t(text[i]);
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.size()) {
            const uint32_t low = uint32_t(text[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (code < 0x80) {
            out.push_back(char(code));
        } else if (code < 0x800) {
            out.push_back(char(0xC0 | (code >> 6)));
            out.push_back(char(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(char(0xE0 | (code >> 12)));
            out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(char(0x80 | (code & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (code >> 18)));
            out.push_back(char(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(char(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

rex::ui::settings::Schema BuildPcSettingsUiSchema(bool arabic, const PcSettingsOffer& offer) {
    using rex::ui::settings::Editor;
    rex::ui::settings::Schema schema;
    arabic = arabic && offer.arabic;
    // Arabic: every text through the table (untranslated technical names
    // stay), right-to-left layout, and the panel's own words.
    const auto text = [arabic](std::wstring_view english) {
        std::string utf8 = PcSettingsUtf8(english);
        if (arabic) {
            const std::string_view translated = PcSettingsArabicText(utf8);
            if (!translated.empty()) return std::string(translated);
        }
        return utf8;
    };
    if (arabic) {
        schema.right_to_left = true;
        schema.text = PcSettingsArabicTable();
    }
    for (const std::wstring_view section : PcEditableSettingsSections()) {
        schema.sections.push_back(text(section));
    }
    for (const PcEditableSettingSpec& spec : PcEditableSettingsSchema()) {
        // Shelved (V440): temporal AA and its upscalers.
        if (spec.key == "graphics.temporal_aa" && !offer.temporalAa) continue;
        // Without Arabic the language setting names only the game's languages.
        const bool englishOnly = spec.key == "general.language" && !offer.arabic;
        const PcSettingPresentation& presentation = PcSettingPresentationFor(spec.key);
        rex::ui::settings::Setting setting;
        setting.key = std::string(spec.key);
        setting.section = text(spec.section);
        setting.label = englishOnly ? std::string("Language") : text(spec.label);
        setting.description =
            englishOnly ? PcSettingsUtf8(kLanguageDescription) : text(presentation.description);
        setting.units = text(spec.units);
        switch (spec.editor) {
            case PcSettingEditorKind::Boolean: setting.editor = Editor::kBoolean; break;
            case PcSettingEditorKind::Integer:
                setting.editor = spec.key == "monitor" ? Editor::kDisplay : Editor::kInteger;
                break;
            case PcSettingEditorKind::Number: setting.editor = Editor::kNumber; break;
            case PcSettingEditorKind::IntegerChoice:
            case PcSettingEditorKind::Choice:
            case PcSettingEditorKind::Resolution: setting.editor = Editor::kChoice; break;
            case PcSettingEditorKind::Key: setting.editor = Editor::kKey; break;
        }
        setting.minimum = spec.minimum;
        setting.maximum = spec.maximum;
        setting.step = spec.step;
        setting.display_multiplier = spec.displayMultiplier;
        for (const PcSettingChoice& choice : spec.choices) {
            if (englishOnly && choice.value == "arabic") continue;
            setting.choices.push_back(
                {std::string(choice.value), text(choice.label), choice.advanced});
        }
        setting.visible_when_key = std::string(presentation.visibleWhenKey);
        setting.visible_when_value = std::string(presentation.visibleWhenValue);
        setting.advanced = presentation.advanced;
        setting.live = presentation.liveInGame;
        schema.settings.push_back(std::move(setting));
    }
    return schema;
}
