/*
 * SPDX-License-Identifier: MIT
 */

#include "viewmodel/i18n.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace {

using RequiredGlyphs = std::map<char32_t, std::string>;

bool add_text(RequiredGlyphs& glyphs, std::string_view text,
              const std::string& source) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[offset]);
        char32_t codepoint = 0;
        std::size_t length = 0;
        char32_t minimum = 0;

        if (lead <= 0x7FU) {
            codepoint = lead;
            length = 1;
        } else if (lead >= 0xC2U && lead <= 0xDFU) {
            codepoint = static_cast<char32_t>(lead & 0x1FU);
            length = 2;
            minimum = 0x80;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            codepoint = static_cast<char32_t>(lead & 0x0FU);
            length = 3;
            minimum = 0x800;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            codepoint = static_cast<char32_t>(lead & 0x07U);
            length = 4;
            minimum = 0x10000;
        } else {
            std::cerr << "invalid UTF-8 lead byte in " << source << '\n';
            return false;
        }

        if (offset + length > text.size()) {
            std::cerr << "truncated UTF-8 sequence in " << source << '\n';
            return false;
        }

        for (std::size_t index = 1; index < length; ++index) {
            const auto byte = static_cast<std::uint8_t>(text[offset + index]);
            if ((byte & 0xC0U) != 0x80U) {
                std::cerr << "invalid UTF-8 continuation byte in " << source << '\n';
                return false;
            }
            codepoint = static_cast<char32_t>((codepoint << 6U) |
                                               static_cast<char32_t>(byte & 0x3FU));
        }

        if (codepoint < minimum || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            std::cerr << "invalid UTF-8 scalar value in " << source << '\n';
            return false;
        }

        glyphs.emplace(codepoint, source);
        offset += length;
    }
    return true;
}

bool add_locale_catalog(RequiredGlyphs& glyphs,
                        lora::viewmodel::Locale locale) {
    using namespace lora::viewmodel;
    const auto code = locale_code(locale);
    const std::string locale_source{"locale name: "};
    if (!add_text(glyphs, locale_display_name(locale),
                  locale_source + std::string{code})) {
        return false;
    }

    for (std::size_t index = 0; index < kStringIdCount; ++index) {
        const auto id = static_cast<StringId>(index);
        const auto source = std::string{code} + ":StringId[" +
                            std::to_string(index) + "]";
        if (!add_text(glyphs, translate(locale, id), source)) {
            return false;
        }
    }
    return true;
}

void add_runtime_basics(RequiredGlyphs& glyphs) {
    for (char32_t codepoint = 0x20; codepoint <= 0x7E; ++codepoint) {
        glyphs.emplace(codepoint, "required ASCII");
    }
    glyphs.emplace(U'\u25A1', "unsupported-character display fallback");
}

bool add_catalog(RequiredGlyphs& glyphs) {
    using namespace lora::viewmodel;
    if (!translations_complete()) {
        std::cerr << "the production translation catalog contains an empty value\n";
        return false;
    }
    for (const auto locale : kSupportedLocales) {
        if (!add_locale_catalog(glyphs, locale)) {
            return false;
        }
    }
    add_runtime_basics(glyphs);
    return true;
}

bool check_font(FT_Library library, const std::filesystem::path& path,
                std::string_view expected_family,
                const RequiredGlyphs& required_glyphs) {
    FT_Face face = nullptr;
    const auto path_string = path.string();
    if (FT_New_Face(library, path_string.c_str(), 0, &face) != 0) {
        std::cerr << "cannot open font: " << path << '\n';
        return false;
    }

    bool passed = true;
    const std::string_view family = face->family_name ? face->family_name : "";
    if (family.find(expected_family) == std::string_view::npos) {
        std::cerr << path << ": unexpected family name: " << family << '\n';
        passed = false;
    }
    if (!FT_IS_SCALABLE(face)) {
        std::cerr << path << ": font is not scalable\n";
        passed = false;
    }
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
        std::cerr << path << ": Unicode cmap is unavailable\n";
        passed = false;
    } else {
        for (const auto& [codepoint, source] : required_glyphs) {
            const auto glyph_index = FT_Get_Char_Index(
                face, static_cast<FT_ULong>(codepoint));
            if (glyph_index == 0) {
                std::cerr << path << ": missing U+" << std::uppercase << std::hex
                          << std::setw(4) << std::setfill('0')
                          << static_cast<std::uint32_t>(codepoint) << std::dec
                          << " required by " << source << '\n';
                passed = false;
            }
        }
    }

    FT_Done_Face(face);
    return passed;
}

void write_scalar(std::ostream& output, char32_t codepoint) {
    if (codepoint <= 0x7F) {
        output.put(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.put(static_cast<char>(0xC0U |
                                     (static_cast<std::uint32_t>(codepoint) >> 6U)));
        output.put(static_cast<char>(0x80U |
                                     (static_cast<std::uint32_t>(codepoint) & 0x3FU)));
    } else if (codepoint <= 0xFFFF) {
        const auto scalar = static_cast<std::uint32_t>(codepoint);
        output.put(static_cast<char>(0xE0U | (scalar >> 12U)));
        output.put(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
        output.put(static_cast<char>(0x80U | (scalar & 0x3FU)));
    } else {
        const auto scalar = static_cast<std::uint32_t>(codepoint);
        output.put(static_cast<char>(0xF0U | (scalar >> 18U)));
        output.put(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3FU)));
        output.put(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3FU)));
        output.put(static_cast<char>(0x80U | (scalar & 0x3FU)));
    }
}

bool export_glyphs(const RequiredGlyphs& glyphs,
                   const std::filesystem::path& output_path) {
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "cannot create glyph input: " << output_path << '\n';
        return false;
    }
    for (const auto& [codepoint, source] : glyphs) {
        static_cast<void>(source);
        write_scalar(output, codepoint);
    }
    output.put('\n');
    output.close();
    if (!output) {
        std::cerr << "cannot finalize glyph input: " << output_path << '\n';
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string_view{argv[1]} == "--export-glyphs") {
        RequiredGlyphs required_glyphs;
        if (!add_catalog(required_glyphs) ||
            !export_glyphs(required_glyphs, argv[2])) {
            return EXIT_FAILURE;
        }
        std::cout << "exported " << required_glyphs.size()
                  << " unique Unicode scalars\n";
        return EXIT_SUCCESS;
    }
    if (argc != 2) {
        std::cerr << "usage: font_coverage_test <font-directory>\n"
                     "       font_coverage_test --export-glyphs <output-file>\n";
        return EXIT_FAILURE;
    }

    RequiredGlyphs required_glyphs;
    if (!add_catalog(required_glyphs)) {
        return EXIT_FAILURE;
    }
    RequiredGlyphs english_glyphs;
    if (!add_locale_catalog(english_glyphs,
                            lora::viewmodel::Locale::English)) {
        return EXIT_FAILURE;
    }
    add_runtime_basics(english_glyphs);

    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        std::cerr << "cannot initialize FreeType\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path font_directory{argv[1]};
    const bool japanese_ok = check_font(
        library, font_directory / "lora-ui-ja.otf", "Noto Sans CJK JP",
        required_glyphs);
    const bool simplified_chinese_ok = check_font(
        library, font_directory / "lora-ui-zh-hans.otf", "Noto Sans CJK SC",
        required_glyphs);
    const bool inter_regular_ok = check_font(
        library, font_directory / "inter-regular.ttf", "Inter 18pt",
        english_glyphs);
    const bool inter_medium_ok = check_font(
        library, font_directory / "inter-medium.ttf", "Inter 18pt",
        english_glyphs);

    FT_Done_FreeType(library);
    if (!japanese_ok || !simplified_chinese_ok || !inter_regular_ok ||
        !inter_medium_ok) {
        return EXIT_FAILURE;
    }

    std::cout << "font coverage passed for " << required_glyphs.size()
              << " unique Unicode scalars in both CJK UI fonts and "
              << english_glyphs.size() << " English scalars in both Inter faces\n";
    return EXIT_SUCCESS;
}
