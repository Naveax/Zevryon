#include "unicode_search_normalizer.hpp"

#include <array>
#include <cstdint>
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

constexpr std::array<std::uint32_t, 6> kDecompPool{
    0x0041U, 0x030aU, // 00C5
    0x0061U, 0x030aU, // 00E5
    0x0061U, 0x0300U, // 00E0
};
constexpr std::array<UnicodeSearchMapEntry, 3> kDecomp{
    UnicodeSearchMapEntry{0x00c5U, 0U, 2U},
    UnicodeSearchMapEntry{0x00e0U, 4U, 2U},
    UnicodeSearchMapEntry{0x00e5U, 2U, 2U},
};
constexpr std::array<std::uint32_t, 29> kFoldPool{
    0x0061U, 0x0062U, 0x0063U, 0x0064U, 0x0065U, 0x0066U, 0x0067U,
    0x0068U, 0x0069U, 0x006aU, 0x006bU, 0x006cU, 0x006dU, 0x006eU,
    0x006fU, 0x0070U, 0x0071U, 0x0072U, 0x0073U, 0x0074U, 0x0075U,
    0x0076U, 0x0077U, 0x0078U, 0x0079U, 0x007aU,
    0x0073U, 0x0073U, // sharp s
    0x00e5U, // A-ring fold
};
constexpr std::array<UnicodeSearchMapEntry, 28> kFold = [] {
    std::array<UnicodeSearchMapEntry, 28> entries{};
    for (std::uint32_t index = 0U; index < 26U; ++index) {
        entries[static_cast<std::size_t>(index)] =
            UnicodeSearchMapEntry{0x0041U + index, index, 1U};
    }
    entries[26] = UnicodeSearchMapEntry{0x00c5U, 28U, 1U};
    entries[27] = UnicodeSearchMapEntry{0x00dfU, 26U, 2U};
    return entries;
}();
constexpr std::array<UnicodeSearchCombiningClassRange, 3> kCcc{
    UnicodeSearchCombiningClassRange{0x0300U, 0x0300U, 230U},
    UnicodeSearchCombiningClassRange{0x030aU, 0x030aU, 230U},
    UnicodeSearchCombiningClassRange{0x0315U, 0x0315U, 232U},
};
constexpr std::array<UnicodeSearchCompositionEntry, 4> kCompose{
    UnicodeSearchCompositionEntry{0x0041U, 0x030aU, 0x00c5U},
    UnicodeSearchCompositionEntry{0x0061U, 0x0300U, 0x00e0U},
    UnicodeSearchCompositionEntry{0x0061U, 0x030aU, 0x00e5U},
    UnicodeSearchCompositionEntry{0x0100U, 0x0101U, 0x0102U},
};
constexpr UnicodeSearchNormalizationTables kTables{
    "17.0.0-fixture",
    "fixture-sha256",
    kDecomp,
    kDecompPool,
    kFold,
    kFoldPool,
    kCcc,
    kCompose,
};

std::vector<NormalizedSearchCodePoint> normalize(
    std::span<const SearchSourceCodePoint> source,
    std::size_t cap = 32U) {
    UnicodeSearchNormalizer normalizer(kTables, UnicodeSearchNormalizerConfig{cap});
    UnicodeSearchNormalizationError error;
    std::vector<NormalizedSearchCodePoint> output;
    const auto consumer = [&output](std::span<const NormalizedSearchCodePoint> values) {
        output.insert(output.end(), values.begin(), values.end());
        return true;
    };
    require(normalizer.feed(source, consumer, &error), "feed failed");
    require(normalizer.finish(consumer, &error), "finish failed");
    require(normalizer.finished(), "normalizer did not finish");
    return output;
}

void test_canonical_casefold_equivalence() {
    const std::array<SearchSourceCodePoint, 1> composed{
        SearchSourceCodePoint{0x00c5U, 10U, 12U},
    };
    const std::array<SearchSourceCodePoint, 2> decomposed{
        SearchSourceCodePoint{0x0041U, 20U, 21U},
        SearchSourceCodePoint{0x030aU, 21U, 23U},
    };
    const auto left = normalize(composed);
    const auto right = normalize(decomposed);
    require(left.size() == 1U && right.size() == 1U, "A-ring output size");
    require(left[0].value == 0x00e5U && right[0].value == 0x00e5U, "A-ring equivalence");
    require(right[0].source_start == 20U && right[0].source_end == 23U, "composed source span union");
}

void test_full_fold_expansion() {
    const std::array<SearchSourceCodePoint, 1> source{
        SearchSourceCodePoint{0x00dfU, 4U, 6U},
    };
    const auto output = normalize(source);
    require(output.size() == 2U, "sharp-s must expand");
    require(output[0].value == 0x0073U && output[1].value == 0x0073U, "sharp-s fold values");
    require(output[0].source_start == 4U && output[1].source_end == 6U, "fold expansion source span");
}

void test_canonical_order_and_composition() {
    const std::array<SearchSourceCodePoint, 3> source{
        SearchSourceCodePoint{0x0041U, 0U, 1U},
        SearchSourceCodePoint{0x0315U, 1U, 3U},
        SearchSourceCodePoint{0x0300U, 3U, 5U},
    };
    const auto output = normalize(source);
    require(output.size() == 2U, "reordered composed output size");
    require(output[0].value == 0x00e0U && output[1].value == 0x0315U, "canonical order/composition");
    require(output[0].source_start == 0U && output[0].source_end == 5U, "composition source span union");
}

void test_hangul_roundtrip() {
    const std::array<SearchSourceCodePoint, 1> precomposed{
        SearchSourceCodePoint{0xac01U, 100U, 103U},
    };
    const auto output = normalize(precomposed);
    require(output.size() == 1U && output[0].value == 0xac01U, "Hangul NFC roundtrip");
    require(output[0].source_start == 100U && output[0].source_end == 103U, "Hangul source span");
}

void test_zero_ccc_composition_boundary() {
    const std::array<SearchSourceCodePoint, 2> source{
        SearchSourceCodePoint{0x0100U, 0U, 2U},
        SearchSourceCodePoint{0x0101U, 2U, 4U},
    };
    const auto output = normalize(source);
    require(output.size() == 1U && output[0].value == 0x0102U, "CCC=0 composition pair");
    require(output[0].source_start == 0U && output[0].source_end == 4U, "CCC=0 composition source span");
}

void test_normalization_only_modes() {
    const std::array<SearchSourceCodePoint, 2> source{
        SearchSourceCodePoint{0x0041U, 0U, 1U},
        SearchSourceCodePoint{0x030aU, 1U, 3U},
    };
    UnicodeSearchNormalizationError error;
    std::vector<NormalizedSearchCodePoint> output;
    const auto consumer = [&output](std::span<const NormalizedSearchCodePoint> values) {
        output.insert(output.end(), values.begin(), values.end());
        return true;
    };

    UnicodeSearchNormalizerConfig nfd_config;
    nfd_config.full_case_fold = false;
    nfd_config.compose = false;
    UnicodeSearchNormalizer nfd(kTables, nfd_config);
    require(nfd.feed(source, consumer, &error) && nfd.finish(consumer, &error), "NFD mode failed");
    require(output.size() == 2U && output[0].value == 0x0041U && output[1].value == 0x030aU, "NFD mode output");

    output.clear();
    UnicodeSearchNormalizerConfig nfc_config;
    nfc_config.full_case_fold = false;
    nfc_config.compose = true;
    UnicodeSearchNormalizer nfc(kTables, nfc_config);
    require(nfc.feed(source, consumer, &error) && nfc.finish(consumer, &error), "NFC mode failed");
    require(output.size() == 1U && output[0].value == 0x00c5U, "NFC mode output");
}

void test_pending_bound_is_hard_failure() {
    UnicodeSearchNormalizer normalizer(kTables, UnicodeSearchNormalizerConfig{2U});
    UnicodeSearchNormalizationError error;
    const std::array<SearchSourceCodePoint, 3> source{
        SearchSourceCodePoint{0x0061U, 0U, 1U},
        SearchSourceCodePoint{0x030aU, 1U, 3U},
        SearchSourceCodePoint{0x0315U, 3U, 5U},
    };
    const auto consumer = [](std::span<const NormalizedSearchCodePoint>) { return true; };
    require(!normalizer.feed(source, consumer, &error), "pending cap must fail");
    require(
        error.kind == UnicodeSearchNormalizationErrorKind::PendingSequenceLimitExceeded,
        "pending cap error kind");
    require(normalizer.failed(), "pending cap must latch failure");
}

void test_consumer_stop_is_not_success() {
    UnicodeSearchNormalizer normalizer(kTables);
    UnicodeSearchNormalizationError error;
    const std::array<SearchSourceCodePoint, 2> source{
        SearchSourceCodePoint{0x0061U, 0U, 1U},
        SearchSourceCodePoint{0x0062U, 1U, 2U},
    };
    const auto consumer = [](std::span<const NormalizedSearchCodePoint>) { return false; };
    require(!normalizer.feed(source, consumer, &error), "consumer stop must fail");
    require(error.kind == UnicodeSearchNormalizationErrorKind::ConsumerStopped, "consumer stop kind");
}

void test_invalid_table_fails_closed() {
    constexpr std::array<UnicodeSearchMapEntry, 1> bad_map{
        UnicodeSearchMapEntry{0x0041U, 99U, 1U},
    };
    const UnicodeSearchNormalizationTables bad{
        "17.0.0-fixture",
        "bad",
        bad_map,
        kDecompPool,
        kFold,
        kFoldPool,
        kCcc,
        kCompose,
    };
    require(!validate_unicode_search_normalization_tables(bad), "bad table validation");
    UnicodeSearchNormalizer normalizer(bad);
    UnicodeSearchNormalizationError error;
    const std::array<SearchSourceCodePoint, 1> source{SearchSourceCodePoint{0x0061U, 0U, 1U}};
    const auto consumer = [](std::span<const NormalizedSearchCodePoint>) { return true; };
    require(!normalizer.feed(source, consumer, &error), "bad table feed must fail");
    require(error.kind == UnicodeSearchNormalizationErrorKind::InvalidTable, "bad table error kind");
}

} // namespace

int main() {
    require(validate_unicode_search_normalization_tables(kTables), "fixture tables invalid");
    test_canonical_casefold_equivalence();
    test_full_fold_expansion();
    test_canonical_order_and_composition();
    test_hangul_roundtrip();
    test_zero_ccc_composition_boundary();
    test_normalization_only_modes();
    test_pending_bound_is_hard_failure();
    test_consumer_stop_is_not_success();
    test_invalid_table_fails_closed();
    std::cout << "Zevryon Unicode search normalizer tests passed\n";
    return 0;
}
