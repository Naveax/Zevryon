#include "unicode_search_normalization_data.generated.hpp"
#include "unicode_search_normalizer.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {
using namespace zevryon::text;

[[noreturn]] void die(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) {
        die(message);
    }
}

std::vector<NormalizedSearchCodePoint> normalize(
    std::span<const SearchSourceCodePoint> input) {
    UnicodeSearchNormalizer normalizer(kUnicodeSearchNormalizationTables);
    UnicodeSearchNormalizationError error;
    std::vector<NormalizedSearchCodePoint> output;
    const auto consumer = [&output](std::span<const NormalizedSearchCodePoint> values) {
        output.insert(output.end(), values.begin(), values.end());
        return true;
    };
    require(normalizer.feed(input, consumer, &error), "production table feed failed");
    require(normalizer.finish(consumer, &error), "production table finish failed");
    return output;
}

void test_authority_identity() {
    require(kUnicodeSearchNormalizationVersion == "17.0.0", "Unicode version mismatch");
    require(
        kUnicodeSearchNormalizationFingerprint ==
            "4194e1873cf16402211f44a847617746ad975a599c3a206e88c1a9f43cf3b70c",
        "normalization fingerprint mismatch");
    require(
        kUnicodeSearchUnicodeDatatxtSha256 ==
            "2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c",
        "UnicodeData SHA mismatch");
    require(
        kUnicodeSearchDerivedNormalizationPropstxtSha256 ==
            "71fd6a206a2c0cdd41feb6b7f656aa31091db45e9cedc926985d718397f9e488",
        "DerivedNormalizationProps SHA mismatch");
    require(
        kUnicodeSearchCaseFoldingtxtSha256 ==
            "ff8d8fefbf123574205085d6714c36149eb946d717a0c585c27f0f4ef58c4183",
        "CaseFolding SHA mismatch");
    require(
        validate_unicode_search_normalization_tables(kUnicodeSearchNormalizationTables),
        "production tables failed structural validation");
}

void test_search_transform_examples() {
    const std::array<SearchSourceCodePoint, 2> ring{
        SearchSourceCodePoint{0x0041U, 0U, 1U},
        SearchSourceCodePoint{0x030aU, 1U, 3U},
    };
    const auto ring_output = normalize(ring);
    require(ring_output.size() == 1U && ring_output[0].value == 0x00e5U, "A-ring search transform");

    const std::array<SearchSourceCodePoint, 1> sharp_s{
        SearchSourceCodePoint{0x00dfU, 10U, 12U},
    };
    const auto sharp_s_output = normalize(sharp_s);
    require(
        sharp_s_output.size() == 2U &&
            sharp_s_output[0].value == 0x0073U &&
            sharp_s_output[1].value == 0x0073U,
        "sharp-s full case fold");

    const std::array<SearchSourceCodePoint, 2> sinhala{
        SearchSourceCodePoint{0x0dddU, 20U, 23U},
        SearchSourceCodePoint{0x0334U, 23U, 25U},
    };
    UnicodeSearchNormalizerConfig config;
    config.full_case_fold = false;
    UnicodeSearchNormalizer nfc(kUnicodeSearchNormalizationTables, config);
    UnicodeSearchNormalizationError error;
    std::vector<NormalizedSearchCodePoint> output;
    const auto consumer = [&output](std::span<const NormalizedSearchCodePoint> values) {
        output.insert(output.end(), values.begin(), values.end());
        return true;
    };
    require(nfc.feed(sinhala, consumer, &error) && nfc.finish(consumer, &error), "Sinhala NFC failed");
    require(
        output.size() == 2U && output[0].value == 0x0dddU && output[1].value == 0x0334U,
        "CCC=0 non-Hangul composition regression");
}

} // namespace

int main() {
    test_authority_identity();
    test_search_transform_examples();
    std::cout << "Zevryon Unicode 17 production table tests passed\n";
    return 0;
}
