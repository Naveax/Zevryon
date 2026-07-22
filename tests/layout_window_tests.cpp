#include "compact_document.hpp"
#include "layout_window.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> output;
    output.reserve(text.size());
    for (const char value : text) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return output;
}

bool is_utf8_continuation(std::byte value) {
    return (std::to_integer<unsigned int>(value) & 0xc0U) == 0x80U;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "zevryon-layout-window-tests";
    std::error_code error_code;
    std::filesystem::remove_all(root, error_code);

    std::string error;
    zevryon::massivedoc::StoreWriter writer(root, {.segment_bytes = 1024U * 1024U, .records_per_search_block = 64U});
    const auto first = bytes("first line\nsecond line\nUTF-8: \xc4\xb0stanbul \xf0\x9f\x9a\x80");
    const auto second = bytes("small cached record with several words");
    if (!require(writer.append(100U, first, &error), error) ||
        !require(writer.append(101U, second, &error), error)) {
        return 1;
    }

    constexpr std::uint64_t kGiantBytes = 16ULL * 1024ULL * 1024ULL;
    std::uint64_t generated = 0U;
    if (!require(writer.append_stream(
                     102U,
                     kGiantBytes,
                     [&generated](std::span<std::byte> target) {
                         std::fill(
                             target.begin(),
                             target.end(),
                             static_cast<std::byte>(static_cast<unsigned char>('x')));
                         generated += static_cast<std::uint64_t>(target.size());
                         return target.size();
                     },
                     &error),
                 error) ||
        !require(generated == kGiantBytes, "giant token generated completely")) {
        return 1;
    }

    zevryon::massivedoc::CorpusMetadata metadata;
    metadata.logical_utf8_bytes = static_cast<std::uint64_t>(first.size()) +
                                  static_cast<std::uint64_t>(second.size()) + kGiantBytes;
    metadata.logical_records = 3U;
    metadata.logical_nodes = 24U;
    metadata.style_runs = 12U;
    metadata.resource_references = 1U;
    metadata.largest_record_bytes = kGiantBytes;
    zevryon::massivedoc::StoreStats store_stats;
    if (!require(writer.finalize(metadata, &store_stats, &error), error)) {
        return 1;
    }

    zevryon::massivedoc::ArenaConfig arena_config;
    arena_config.records_per_block = 2U;
    arena_config.estimated_bytes_per_line = 96U;
    arena_config.line_height_q8 = 18U * 256U;
    arena_config.vertical_padding_q8 = 12U * 256U;
    zevryon::massivedoc::ArenaStats arena_stats;
    if (!require(zevryon::massivedoc::build_compact_arena(root, arena_config, &arena_stats, &error), error)) {
        return 1;
    }

    zevryon::massivedoc::LayoutConfig layout_config;
    layout_config.max_cache_bytes = 256U * 1024U;
    zevryon::massivedoc::LayoutWindowEngine engine(root, layout_config);
    if (!require(engine.open(&error), error)) {
        return 1;
    }

    zevryon::massivedoc::LayoutWindowResult top;
    if (!require(engine.layout(0U, 800U * 256U, 720U * 256U, 360U * 256U, 256U, &top, &error), error) ||
        !require(!top.fragments.empty(), "top layout has fragments") ||
        !require(top.cache_misses != 0U, "first layout records cache misses") ||
        !require(top.source_bytes_read != 0U, "first layout reads bounded source") ||
        !require(top.cache_bytes != 0U, "cache reports conservative resident charge") ||
        !require(top.cache_bytes <= layout_config.max_cache_bytes, "cache respects byte budget")) {
        return 1;
    }
    bool saw_hard_break = false;
    for (const auto& fragment : top.fragments) {
        saw_hard_break = saw_hard_break || fragment.hard_break;
        if (!require(fragment.source_start <= fragment.source_end, "fragment source range ordered") ||
            !require(fragment.height_q8 == layout_config.line_height_q8, "fragment line height stable")) {
            return 1;
        }
    }
    if (!require(saw_hard_break, "newline creates hard-break fragment")) {
        return 1;
    }

    const std::size_t cache_after_top = top.cache_bytes;
    zevryon::massivedoc::LayoutWindowResult cached_top;
    if (!require(engine.layout(0U, 800U * 256U, 720U * 256U, 360U * 256U, 256U, &cached_top, &error), error) ||
        !require(cached_top.cache_hits != 0U, "second layout reuses cache") ||
        !require(cached_top.cache_bytes == engine.cache_bytes(), "reported cache charge matches engine") ||
        !require(cached_top.cache_bytes <= layout_config.max_cache_bytes, "reused cache stays bounded") ||
        !require(cached_top.cache_bytes >= cache_after_top, "cache reuse does not lose resident accounting")) {
        return 1;
    }

    zevryon::massivedoc::LayoutWindowResult narrow;
    if (!require(engine.layout(0U, 40U * 256U, 2000U * 256U, 0U, 256U, &narrow, &error), error) ||
        !require(!narrow.fragments.empty(), "narrow UTF-8 layout has fragments")) {
        return 1;
    }
    for (const auto& fragment : narrow.fragments) {
        if (fragment.record_index != 0U) {
            continue;
        }
        if (!require(fragment.source_start <= first.size(), "UTF-8 fragment start in record") ||
            !require(fragment.source_end <= first.size(), "UTF-8 fragment end in record")) {
            return 1;
        }
        if (fragment.source_start < first.size() &&
            !require(!is_utf8_continuation(first[static_cast<std::size_t>(fragment.source_start)]),
                     "fragment start never splits UTF-8 sequence")) {
            return 1;
        }
        if (fragment.source_end < first.size() &&
            !require(!is_utf8_continuation(first[static_cast<std::size_t>(fragment.source_end)]),
                     "fragment end never splits UTF-8 sequence")) {
            return 1;
        }
    }

    zevryon::massivedoc::CompactArenaReader arena(root);
    if (!require(arena.open(&error), error)) {
        return 1;
    }
    const std::uint64_t end_scroll = arena.stats().total_height_q8 > 720U * 256U
                                         ? arena.stats().total_height_q8 - 720U * 256U
                                         : 0U;
    zevryon::massivedoc::LayoutWindowResult giant;
    if (!require(engine.layout(end_scroll, 800U * 256U, 720U * 256U, 360U * 256U, 128U, &giant, &error), error) ||
        !require(giant.scroll_anchor_adjusted, "width correction preserves proportional giant-record anchor") ||
        !require(giant.scroll_clamped, "near-end anchor clamps only to corrected document end") ||
        !require(!giant.fragments.empty(), "giant token produces viewport fragments") ||
        !require(giant.fragments.size() <= 128U, "giant token respects fragment cap") ||
        !require(giant.cache_bytes <= layout_config.max_cache_bytes, "giant token cannot expand cache") ||
        !require(giant.source_bytes_read >= kGiantBytes, "giant token is measured by streaming")) {
        return 1;
    }
    for (const auto& fragment : giant.fragments) {
        if (!require(fragment.record_index == 2U, "end viewport remains on giant record") ||
            !require(fragment.source_end > fragment.source_start, "giant fragment has source progress")) {
            return 1;
        }
    }

    zevryon::massivedoc::CompactArenaReader corrected_arena(root);
    if (!require(corrected_arena.open(&error), error)) {
        return 1;
    }
    const std::uint64_t corrected_end_scroll = corrected_arena.stats().total_height_q8 > 720U * 256U
                                                   ? corrected_arena.stats().total_height_q8 - 720U * 256U
                                                   : 0U;
    const std::size_t cache_before_giant_reuse = engine.cache_bytes();
    zevryon::massivedoc::LayoutWindowResult giant_reused;
    if (!require(engine.layout(
                     corrected_end_scroll,
                     800U * 256U,
                     720U * 256U,
                     360U * 256U,
                     128U,
                     &giant_reused,
                     &error),
                 error) ||
        !require(!giant_reused.scroll_anchor_adjusted, "stable record geometry does not re-anchor") ||
        !require(!giant_reused.scroll_clamped, "stable end scroll does not clamp repeatedly") ||
        !require(giant_reused.cache_hits != 0U, "giant record reuses measured-height metadata") ||
        !require(!giant_reused.fragments.empty(), "reused giant viewport remains materialized") ||
        !require(giant_reused.fragments.size() <= 128U, "reused giant viewport respects fragment cap") ||
        !require(giant_reused.source_bytes_read >= kGiantBytes, "reused giant record streams visible fragments") ||
        !require(giant_reused.cache_bytes == cache_before_giant_reuse,
                 "metadata-only giant cache entry has stable resident charge") ||
        !require(giant_reused.cache_bytes <= layout_config.max_cache_bytes,
                 "metadata-only giant cache remains bounded")) {
        return 1;
    }

    zevryon::massivedoc::CompactArenaReader reopened(root);
    if (!require(reopened.open(&error), error) ||
        !require(reopened.stats().total_height_q8 != arena_stats.total_height_q8,
                 "measured layout heights persist to arena")) {
        return 1;
    }

    error_code.clear();
    std::filesystem::remove_all(root, error_code);
    if (!require(!error_code, "layout test cleanup failed: " + error_code.message())) {
        return 1;
    }
    std::cout << "layout window tests passed\n";
    return 0;
}
