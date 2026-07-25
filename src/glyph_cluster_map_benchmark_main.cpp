#include "glyph_cluster_map.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <vector>

namespace {
using namespace zevryon::core;
using namespace zevryon::text;

constexpr std::uint32_t kClusterCount = 65'536U;
constexpr std::size_t kSegmentCount = 2'048U;
constexpr std::uint32_t kClustersPerSegment = 32U;
constexpr std::size_t kSamples = 64U;
constexpr std::size_t kBatch = 4U;
constexpr std::size_t kMapBytes =
    static_cast<std::size_t>(kClusterCount) * sizeof(GlyphClusterRecord);

ShapedGlyph make_glyph(std::uint32_t id, std::uint32_t cluster) {
    return ShapedGlyph{id, cluster, 64, 0, 0, 0, 0U};
}

bool build_fixture(MultiRunShapedText* text) {
    try {
        text->segments.reserve(kSegmentCount);
        for (std::size_t segment_index = 0U;
             segment_index < kSegmentCount;
             ++segment_index) {
            const std::uint32_t first = static_cast<std::uint32_t>(
                segment_index * kClustersPerSegment);
            const bool rtl = (segment_index & 1U) != 0U;
            text->segments.emplace_back(text->glyph_resource());
            MultiRunShapedSegment& segment = text->segments.back();
            segment.run = ShapingRunBoundary{
                first,
                static_cast<FontFaceId>(rtl ? 1U : 0U),
                rtl ? ScriptId::Arab : ScriptId::Latn,
                rtl ? ShapingDirection::RightToLeft
                    : ShapingDirection::LeftToRight,
                FontFallbackSource::Primary,
                static_cast<std::uint8_t>(rtl ? 1U : 0U),
                0U};
            segment.glyphs.first_cluster = first;
            segment.glyphs.cluster_limit = first + kClustersPerSegment;
            segment.glyphs.script = segment.run.script;
            segment.glyphs.direction = segment.run.direction;
            segment.glyphs.glyphs.reserve(20U);

            for (std::size_t group = 0U; group < 16U; ++group) {
                const std::size_t logical_group = rtl ? 15U - group : group;
                const std::uint32_t owner = first +
                    static_cast<std::uint32_t>(logical_group * 2U);
                const std::uint32_t id = static_cast<std::uint32_t>(
                    100U + segment_index * 32U + logical_group);
                segment.glyphs.glyphs.push_back(make_glyph(id, owner));
                if ((logical_group & 3U) == 0U) {
                    segment.glyphs.glyphs.push_back(
                        make_glyph(id + 10'000U, owner));
                }
            }
        }
    } catch (...) {
        return false;
    }
    return text->segments.size() == kSegmentCount &&
           text->segments.back().glyphs.cluster_limit == kClusterCount;
}

double percentile(const std::vector<double>& sorted, double probability) {
    const double position = probability *
        static_cast<double>(sorted.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1U, sorted.size() - 1U);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

std::uint64_t checksum(const GlyphClusterMap& map) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const GlyphClusterRecord& record : map.records) {
        value ^= record.segment_index;
        value *= 1099511628211ULL;
        value ^= record.owner_cluster;
        value *= 1099511628211ULL;
        value ^= record.first_glyph;
        value *= 1099511628211ULL;
        value ^= record.glyph_count;
        value *= 1099511628211ULL;
    }
    return value;
}

} // namespace

int main() {
    MultiRunShapedText shaped_text;
    if (!build_fixture(&shaped_text)) {
        return 2;
    }

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::GlyphClusterMap, kMapBytes);
    LedgerMemoryResource memory(ledger, ResourceClass::GlyphClusterMap);
    GlyphClusterMap map(&memory);
    GlyphClusterMapStats stats;
    GlyphClusterMapError error;

    auto run_once = [&]() {
        return build_glyph_cluster_map(
            shaped_text,
            kClusterCount,
            &map,
            &stats,
            &error);
    };
    for (std::size_t warmup = 0U; warmup < 8U; ++warmup) {
        if (!run_once()) {
            return 1;
        }
    }

    std::vector<double> samples;
    samples.reserve(kSamples);
    for (std::size_t sample = 0U; sample < kSamples; ++sample) {
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t batch = 0U; batch < kBatch; ++batch) {
            if (!run_once()) {
                return 1;
            }
        }
        const auto end = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double, std::milli>(end - start).count() /
            static_cast<double>(kBatch);
        samples.push_back(elapsed);
    }
    std::sort(samples.begin(), samples.end());

    const ResourceSnapshot snapshot =
        ledger.snapshot(ResourceClass::GlyphClusterMap);
    if (map.records.size() != kClusterCount ||
        stats.input_segments != kSegmentCount ||
        stats.input_glyphs != 40'960U ||
        stats.input_clusters != kClusterCount ||
        stats.output_records != kClusterCount ||
        stats.owner_clusters != 32'768U ||
        stats.continuation_clusters != 32'768U ||
        stats.left_to_right_segments != 1'024U ||
        stats.right_to_left_segments != 1'024U ||
        stats.maximum_group_glyphs != 2U ||
        stats.maximum_owner_span_clusters != 2U ||
        snapshot.current_bytes != kMapBytes ||
        snapshot.peak_bytes != kMapBytes ||
        snapshot.rejected_reservations != 0U ||
        snapshot.accounting_errors != 0U ||
        !ledger.within_hard_limits() ||
        !ledger.accounting_clean()) {
        return 1;
    }

    std::cout << std::fixed << std::setprecision(9)
              << "{\"schema\":\"zevryon.glyph-cluster-map-benchmark.v1\","
              << "\"input_clusters\":" << kClusterCount << ','
              << "\"input_segments\":" << kSegmentCount << ','
              << "\"input_glyphs\":" << stats.input_glyphs << ','
              << "\"owner_clusters\":" << stats.owner_clusters << ','
              << "\"continuation_clusters\":"
              << stats.continuation_clusters << ','
              << "\"output_records\":" << map.records.size() << ','
              << "\"record_bytes\":" << sizeof(GlyphClusterRecord) << ','
              << "\"current_bytes\":" << snapshot.current_bytes << ','
              << "\"peak_bytes\":" << snapshot.peak_bytes << ','
              << "\"checksum\":" << checksum(map) << ','
              << "\"p50_ms\":" << percentile(samples, 0.50) << ','
              << "\"p95_ms\":" << percentile(samples, 0.95) << ','
              << "\"p99_ms\":" << percentile(samples, 0.99) << ','
              << "\"maximum_ms\":" << samples.back() << ','
              << "\"accounting_clean\":true,"
              << "\"within_hard_limits\":true}\n";
    return 0;
}
