#include "unicode_script.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ScriptRangeCase {
    std::uint32_t start{0};
    std::uint32_t end{0};
    zevryon::text::ScriptId script{zevryon::text::ScriptId::Zzzz};
};

struct ExtensionRangeCase {
    std::uint32_t start{0};
    std::uint32_t end{0};
    std::vector<zevryon::text::ScriptId> scripts;
};

std::string_view trim(std::string_view value) noexcept {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

bool parse_hex(std::string_view value, std::uint32_t* output) noexcept {
    if (output == nullptr || value.empty()) {
        return false;
    }
    const char* first = value.data();
    const char* last = first + value.size();
    const auto result = std::from_chars(first, last, *output, 16);
    return result.ec == std::errc{} && result.ptr == last &&
           *output <= 0x10ffffU;
}

bool parse_range(
    std::string_view value,
    std::uint32_t* start,
    std::uint32_t* end) noexcept {
    const std::size_t separator = value.find("..");
    const std::string_view first = trim(value.substr(0U, separator));
    const std::string_view second = separator == std::string_view::npos
        ? first
        : trim(value.substr(separator + 2U));
    return parse_hex(first, start) && parse_hex(second, end) && *start <= *end;
}

bool split_data_line(
    const std::string& line,
    std::string_view* range,
    std::string_view* value) noexcept {
    const std::string_view view(line);
    const std::size_t comment = view.find('#');
    const std::string_view content = trim(view.substr(0U, comment));
    if (content.empty()) {
        return false;
    }
    const std::size_t separator = content.find(';');
    if (separator == std::string_view::npos) {
        return false;
    }
    *range = trim(content.substr(0U, separator));
    *value = trim(content.substr(separator + 1U));
    return !range->empty() && !value->empty();
}

bool load_scripts(
    const char* path,
    std::vector<ScriptRangeCase>* ranges) {
    std::ifstream input(path);
    if (!input) {
        std::cerr << "failed to open Scripts.txt\n";
        return false;
    }
    std::string line;
    while (std::getline(input, line)) {
        std::string_view range_text;
        std::string_view script_text;
        if (!split_data_line(line, &range_text, &script_text)) {
            continue;
        }
        ScriptRangeCase item;
        if (!parse_range(range_text, &item.start, &item.end) ||
            !zevryon::text::script_id_from_name(script_text, &item.script)) {
            std::cerr << "invalid Scripts.txt line: " << line << '\n';
            return false;
        }
        ranges->push_back(item);
    }
    std::sort(
        ranges->begin(),
        ranges->end(),
        [](const ScriptRangeCase& left, const ScriptRangeCase& right) {
            return left.start < right.start;
        });
    for (std::size_t index = 1U; index < ranges->size(); ++index) {
        if ((*ranges)[index - 1U].end >= (*ranges)[index].start) {
            std::cerr << "overlapping Scripts.txt ranges\n";
            return false;
        }
    }
    return !ranges->empty();
}

bool parse_script_list(
    std::string_view text,
    std::vector<zevryon::text::ScriptId>* scripts) {
    scripts->clear();
    while (!text.empty()) {
        const std::size_t separator = text.find_first_of(" \t");
        const std::string_view token = text.substr(0U, separator);
        zevryon::text::ScriptId script;
        if (!zevryon::text::script_id_from_name(token, &script)) {
            return false;
        }
        scripts->push_back(script);
        if (separator == std::string_view::npos) {
            break;
        }
        text = trim(text.substr(separator + 1U));
    }
    std::sort(scripts->begin(), scripts->end());
    scripts->erase(std::unique(scripts->begin(), scripts->end()), scripts->end());
    return !scripts->empty();
}

bool load_extensions(
    const char* path,
    std::vector<ExtensionRangeCase>* ranges) {
    std::ifstream input(path);
    if (!input) {
        std::cerr << "failed to open ScriptExtensions.txt\n";
        return false;
    }
    std::string line;
    while (std::getline(input, line)) {
        std::string_view range_text;
        std::string_view scripts_text;
        if (!split_data_line(line, &range_text, &scripts_text)) {
            continue;
        }
        ExtensionRangeCase item;
        if (!parse_range(range_text, &item.start, &item.end) ||
            !parse_script_list(scripts_text, &item.scripts)) {
            std::cerr << "invalid ScriptExtensions.txt line: " << line << '\n';
            return false;
        }
        ranges->push_back(std::move(item));
    }
    std::sort(
        ranges->begin(),
        ranges->end(),
        [](const ExtensionRangeCase& left, const ExtensionRangeCase& right) {
            return left.start < right.start;
        });
    for (std::size_t index = 1U; index < ranges->size(); ++index) {
        if ((*ranges)[index - 1U].end >= (*ranges)[index].start) {
            std::cerr << "overlapping ScriptExtensions.txt ranges\n";
            return false;
        }
    }
    return !ranges->empty();
}

bool same_scripts(
    const zevryon::text::ScriptSetView& actual,
    const std::vector<zevryon::text::ScriptId>& expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    std::vector<zevryon::text::ScriptId> actual_values;
    actual_values.reserve(actual.size());
    for (std::size_t index = 0U; index < actual.size(); ++index) {
        actual_values.push_back(actual[index]);
    }
    std::sort(actual_values.begin(), actual_values.end());
    return actual_values == expected;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: zevryon-script-conformance Scripts.txt ScriptExtensions.txt\n";
        return 2;
    }

    std::vector<ScriptRangeCase> script_ranges;
    std::vector<ExtensionRangeCase> extension_ranges;
    if (!load_scripts(argv[1], &script_ranges) ||
        !load_extensions(argv[2], &extension_ranges)) {
        return 1;
    }

    std::size_t script_range_index = 0U;
    std::size_t extension_range_index = 0U;
    std::uint64_t explicit_extension_codepoints = 0U;
    for (std::uint32_t codepoint = 0U; codepoint <= 0x10ffffU; ++codepoint) {
        while (script_range_index < script_ranges.size() &&
               script_ranges[script_range_index].end < codepoint) {
            ++script_range_index;
        }
        const zevryon::text::ScriptId expected_script =
            script_range_index < script_ranges.size() &&
                    script_ranges[script_range_index].start <= codepoint &&
                    codepoint <= script_ranges[script_range_index].end
                ? script_ranges[script_range_index].script
                : zevryon::text::ScriptId::Zzzz;
        const zevryon::text::ScriptId actual_script =
            zevryon::text::script_of(codepoint);
        if (actual_script != expected_script) {
            std::cerr << "Script mismatch at U+" << std::hex << codepoint
                      << ": expected "
                      << zevryon::text::script_short_name(expected_script)
                      << " got "
                      << zevryon::text::script_short_name(actual_script)
                      << '\n';
            return 1;
        }

        while (extension_range_index < extension_ranges.size() &&
               extension_ranges[extension_range_index].end < codepoint) {
            ++extension_range_index;
        }
        const bool explicit_extensions =
            extension_range_index < extension_ranges.size() &&
            extension_ranges[extension_range_index].start <= codepoint &&
            codepoint <= extension_ranges[extension_range_index].end;
        const zevryon::text::ScriptSetView actual_extensions =
            zevryon::text::script_extensions(codepoint);
        if (explicit_extensions) {
            ++explicit_extension_codepoints;
            if (!actual_extensions.has_explicit_extensions() ||
                !same_scripts(
                    actual_extensions,
                    extension_ranges[extension_range_index].scripts)) {
                std::cerr << "Script_Extensions mismatch at U+"
                          << std::hex << codepoint << '\n';
                return 1;
            }
        } else if (actual_extensions.has_explicit_extensions() ||
                   actual_extensions.size() != 1U ||
                   actual_extensions[0] != expected_script) {
            std::cerr << "default Script_Extensions mismatch at U+"
                      << std::hex << codepoint << '\n';
            return 1;
        }
    }

    std::cout << '{'
              << "\"schema\":\"zevryon.script-conformance.v1\","
              << "\"unicode_version\":\""
              << zevryon::text::kUnicodeScriptDataVersion << "\","
              << "\"data_fingerprint\":\""
              << zevryon::text::kUnicodeScriptDataFingerprint << "\","
              << "\"codepoints\":1114112,"
              << "\"script_ranges\":" << script_ranges.size() << ','
              << "\"script_extension_ranges\":" << extension_ranges.size() << ','
              << "\"explicit_extension_codepoints\":"
              << explicit_extension_codepoints << ','
              << "\"passed\":true}\n";
    return 0;
}
