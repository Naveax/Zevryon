#include "font_load_locator.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace zevryon::text;

constexpr std::size_t kIterations = 1000000U;

void append_field(std::string* identity, std::string_view value) {
    identity->append(std::to_string(value.size()));
    identity->push_back(':');
    identity->append(value);
    identity->push_back('|');
}

std::string fontconfig_identity() {
    std::string value = "fontconfig|";
    append_field(&value, "");
    append_field(&value, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    append_field(&value, "0");
    append_field(&value, "DejaVuSans");
    append_field(&value, "");
    return value;
}

std::string directwrite_identity() {
    std::string value = "directwrite|";
    append_field(&value, "1");
    append_field(&value, "C:\\Windows\\Fonts\\arial.ttf");
    append_field(&value, "1");
    append_field(&value, "0");
    append_field(&value, "400");
    append_field(&value, "5");
    append_field(&value, "0");
    append_field(&value, "ArialMT");
    return value;
}

std::string coretext_identity() {
    std::string value = "coretext|";
    append_field(&value, "/System/Library/Fonts/Helvetica.ttc");
    append_field(&value, "Helvetica");
    append_field(&value, "400");
    append_field(&value, "5");
    append_field(&value, "0");
    append_field(&value, "2");
    append_field(&value, "2003265652");
    append_field(&value, "4645744490609377280");
    append_field(&value, "2003072104");
    append_field(&value, "4636737291354636288");
    return value;
}

} // namespace

int main() {
    const std::string fontconfig = fontconfig_identity();
    const std::string directwrite = directwrite_identity();
    const std::string coretext = coretext_identity();
    const std::string_view identities[]{fontconfig, directwrite, coretext};

    std::uint64_t checksum = 0U;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t index = 0U; index < kIterations; ++index) {
        FontLoadLocator locator;
        FontLoadLocatorStats stats;
        FontLoadLocatorError error;
        if (!parse_font_load_locator(
                identities[index % 3U],
                &locator,
                &stats,
                &error)) {
            std::cerr << "locator benchmark parse failed: "
                      << font_load_locator_error_kind_name(error.kind) << '\n';
            return 1;
        }
        checksum += static_cast<std::uint64_t>(locator.face_index);
        checksum += static_cast<std::uint64_t>(locator.file_count);
        checksum += static_cast<std::uint64_t>(stats.fields_parsed);
        checksum += static_cast<std::uint64_t>(locator.kind);
        checksum += static_cast<std::uint64_t>(locator.capability);
    }
    const auto end = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - begin).count();
    const double identities_per_second = elapsed_ms == 0.0
        ? 0.0
        : static_cast<double>(kIterations) / (elapsed_ms / 1000.0);

    std::cout << std::fixed << std::setprecision(6)
              << "{\"schema\":\"zevryon.font-load-locator-benchmark.v1\","
              << "\"iterations\":" << kIterations << ','
              << "\"platforms\":3,"
              << "\"elapsed_ms\":" << elapsed_ms << ','
              << "\"identities_per_second\":" << identities_per_second << ','
              << "\"checksum\":" << checksum
              << "}\n";
    return checksum == 0U ? 1 : 0;
}
