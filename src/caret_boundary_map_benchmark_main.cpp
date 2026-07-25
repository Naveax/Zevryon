#include "caret_boundary_map.hpp"
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
constexpr std::size_t kBoundaryBytes =
    static_cast<std::size_t>(kClusterCount) + 1U;

ShapedGlyph make_glyph(
    std::uint32_t id,
    std::uint32_t cluster,
    std::uint32_t flags) {
    return ShapedGlyph{id, cluster, 64, 0, 0, 0, flags};
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
                const std::uint64_t global_group =
                    static_cast<std::uint64_t>(segment_index) * 16U +
                    logical_group;
                const std::uint32_t flags =
                    global_group % 8U == 0U
                    ? kShapedGlyphUnsafeToBreak
                    : 0U;
                const std::uint32_t id = static_cast<std::uint32_t>(
                    100U + segment_index * 32U + logical_group);
                segment.glyphs.glyphs.push_back(make_glyph(id, owner, flags));
                if ((logical_group & 3U) == 0U) {
                    segment.glyphs.glyphs.push_back(
                        make_glyph(id + 10'000U, owner, flags));
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

std::uint64_t checksum(const CaretBoundaryMap& map) {
    std::uint64_t value = 1469598103934665603ULL;
    for (std::uint8_t flags : map.flags) {
        value ^= flags;
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

    GlyphClusterMap cluster_map;
    GlyphClusterMapStats cluster_stats;
    GlyphClusterMapError cluster_error;
    if (!build_glyph_cluster_map(
            shaped_text,
            kClusterCount,
            &cluster_map,
            &cluster_stats,
            &cluster_error)) {
        return 1;
    }

    ResourceLedger ledger;
    ledger.set_hard_limit(ResourceClass::CaretBoundaryMap, kBoundaryBytes);
    LedgerMemoryResource memory(ledger, ResourceClass::CaretBoundaryMap);
    CaretBoundaryMap map(&memory);
    CaretBoundaryMapStats stats;
    CaretBoundaryMapError error;

    auto run_once = [&]() {
        return build_caret_boundary_map(
            CaretBoundaryMapRequest{
                &shaped_text,
                &cluster_map,
                kClusterCount},
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
        samples.push_back(
            std::chrono::duration<double, std::milli>(end - start).count() /
            static_cast<double>(kBatch));
    }
    std::sort(samples.begin(), samples.end());

    const ResourceSnapshot snapshot =
        ledger.snapshot(ResourceClass::CaretBoundaryMap);
    if (cluster_map.records.size() != kClusterCount ||
        cluster_stats.output_records != kClusterCount ||
        map.flags.size() != kBoundaryBytes ||
        stats.input_segments != kSegmentCount ||
        stats.input_glyphs != 40'960U ||
        stats.input_clusters != kClusterCount ||
        stats.output_boundaries != kBoundaryBytes ||
        stats.glyph_groups != 32'768U ||
        stats.safe_boundaries != 24'578U ||
        stats.unsafe_boundaries != 40'959U ||
        stats.text_edge_boundaries != 2U ||
        stats.run_edge_boundaries != 2'049U ||
        stats.glyph_edge_boundaries != 32'769U ||
        stats.merged_interior_boundaries != 32'768U ||
        stats.unsafe_to_break_boundaries != 8'191U ||
        snapshot.current_bytes != kBoundaryBytes ||
        snapshot.peak_bytes != kBoundaryBytes ||
        snapshot.rejected_reservations != 0U ||
        snapshot.accounting_errors != 0U ||
        !ledger.within_hard_limits() ||
        !ledger.accounting_clean()) {
        return 1;
    }

    std::cout << std::fixed << std::setprecision(9)
              << "{\"schema\":\"zevryon.caret-boundary-map-benchmark.v1\","
              << "\"input_clusters\":" << kClusterCount << ','
              << "\"input_segments\":" << kSegmentCount << ','
              << "\"input_glyphs\":" << stats.input_glyphs << ','
              << "\"glyph_groups\":" << stats.glyph_groups << ','
              << "\"output_boundaries\":" << map.flags.size() << ','
              << "\"record_bytes\":1,"
              << "\"current_bytes\":" << snapshot.current_bytes << ','
              << "\"peak_bytes\":" << snapshot.peak_bytes << ','
              << "\"safe_boundaries\":" << stats.safe_boundaries << ','
              << "\"unsafe_boundaries\":" << stats.unsafe_boundaries << ','
              << "\"merged_interior_boundaries\":"
              << stats.merged_interior_boundaries << ','
              << "\"unsafe_to_break_boundaries\":"
              << stats.unsafe_to_break_boundaries << ','
              << "\"checksum\":" << checksum(map) << ','
              << "\"p50_ms\":" << percentile(samples, 0.50) << ','
              << "\"p95_ms\":" << percentile(samples, 0.95) << ','
              << "\"p99_ms\":" << percentile(samples, 0.99) << ','
              << "\"maximum_ms\":" << samples.back() << ','
              << "\"accounting_clean\":true,"
              << "\"within_hard_limits\":true}\n";
    return 0;
}
