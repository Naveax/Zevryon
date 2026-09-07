#include "compact_document.hpp"
#include "full_document_export.hpp"
#include "full_document_selection.hpp"
#include "layout_window.hpp"
#include "massivedoc_profile_policy.hpp"
#include "massivedoc_progressive_import.hpp"
#include "massivedoc_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using zevryon::massivedoc::CompactArenaReader;
using zevryon::massivedoc::FullDocumentExportFormat;
using zevryon::massivedoc::FullDocumentExportOptions;
using zevryon::massivedoc::FullDocumentExportStats;
using zevryon::massivedoc::LayoutWindowEngine;
using zevryon::massivedoc::LayoutWindowResult;
using zevryon::massivedoc::MassiveDocDeviceProfile;
using zevryon::massivedoc::MassiveDocProfileRuntimeConfig;
using zevryon::massivedoc::ProgressiveImportConfig;
using zevryon::massivedoc::ProgressivePreviewInfo;
using zevryon::massivedoc::SearchHit;
using zevryon::massivedoc::SequencePosition;
using zevryon::massivedoc::StoreReader;
using zevryon::massivedoc::StoreStats;

constexpr std::uint32_t kViewportWidthQ8 = 800U * 256U;
constexpr std::uint64_t kViewportHeightQ8 = 720ULL * 256ULL;
constexpr std::uint64_t kOverscanQ8 = 720ULL * 256ULL;
constexpr std::size_t kMaxFragments = 2048U;
constexpr std::size_t kWarmups = 16U;
constexpr std::size_t kMeasuredSamples = 257U;
constexpr std::string_view kTailMarker = "ZEVRYON_M8_TITAN_TAIL";

struct DeterministicCoordinateGenerator {
    std::uint32_t state{0x6a09e667U};

    std::uint64_t next(std::uint64_t maximum) noexcept {
        state = state * 1664525U + 1013904223U;
        if (maximum == 0U) {
            return 0U;
        }
        return (static_cast<std::uint64_t>(state) * maximum) /
               static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    }
};

double milliseconds_since(Clock::time_point started) {
    return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

double microseconds_since(Clock::time_point started) {
    return std::chrono::duration<double, std::micro>(Clock::now() - started).count();
}

bool require(bool condition, std::string_view message, std::string* error) {
    if (!condition) {
        *error = std::string(message);
        return false;
    }
    return true;
}

void write_number_array(std::ostream& output, const std::vector<double>& values) {
    output << '[';
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << values[index];
    }
    output << ']';
}

void write_u64_array(std::ostream& output, const std::vector<std::uint64_t>& values) {
    output << '[';
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << values[index];
    }
    output << ']';
}

bool same_hits(const std::vector<SearchHit>& left, const std::vector<SearchHit>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].record_index != right[index].record_index ||
            left[index].logical_id != right[index].logical_id ||
            left[index].byte_offset != right[index].byte_offset) {
            return false;
        }
    }
    return true;
}

std::uint32_t alternate_height(std::uint32_t current) noexcept {
    constexpr std::uint32_t delta = 256U;
    if (current <= std::numeric_limits<std::uint32_t>::max() - delta) {
        return current + delta;
    }
    return current > delta ? current - delta : current - 1U;
}

bool run_mutation(
    CompactArenaReader* arena,
    std::uint64_t index,
    double* elapsed_us,
    std::string* error) {
    SequencePosition position;
    if (!arena->logical_snapshot().at(index, &position, error)) {
        return false;
    }
    const std::uint32_t original = position.record.height_q8;
    const std::uint32_t replacement = alternate_height(original);
    zevryon::massivedoc::HeightUpdateResult update;
    const auto started = Clock::now();
    if (!arena->update_height(index, replacement, &update, error)) {
        return false;
    }
    *elapsed_us = microseconds_since(started);
    if (!require(
            update.record_index == index && update.old_height_q8 == original &&
                update.new_height_q8 == replacement,
            "mutation receipt drifted",
            error)) {
        return false;
    }
    zevryon::massivedoc::HeightUpdateResult restore;
    if (!arena->update_height(index, original, &restore, error)) {
        return false;
    }
    return require(
        restore.record_index == index && restore.old_height_q8 == replacement &&
            restore.new_height_q8 == original,
        "mutation restore receipt drifted",
        error);
}

bool ensure_new_work_dir(const std::filesystem::path& root, std::string* error) {
    std::error_code fs_error;
    const bool exists = std::filesystem::exists(root, fs_error);
    if (fs_error) {
        *error = "cannot inspect work directory: " + fs_error.message();
        return false;
    }
    if (exists) {
        *error = "profile probe refuses an existing work directory";
        return false;
    }
    std::filesystem::create_directories(root, fs_error);
    if (fs_error) {
        *error = "cannot create work directory: " + fs_error.message();
        return false;
    }
    return true;
}

bool run_probe(
    MassiveDocDeviceProfile profile,
    const std::filesystem::path& titan_path,
    const std::filesystem::path& work_dir,
    std::string* rendered,
    std::string* error) {
    const MassiveDocProfileRuntimeConfig runtime =
        zevryon::massivedoc::make_massive_doc_profile_runtime_config(profile);
    if (!require(runtime.budgets.within_profile_budgets, "profile runtime exceeds cache budgets", error)) {
        return false;
    }
    if (!ensure_new_work_dir(work_dir, error)) {
        return false;
    }

    const std::filesystem::path store_root = work_dir / "store";
    const std::filesystem::path preview_root = work_dir / "preview";
    const std::filesystem::path copy_output = work_dir / "copy-output.txt";

    double streaming_ms = 0.0;
    std::uint64_t streaming_remaining_records = 0U;
    std::uint64_t streaming_preview_records = 0U;
    std::size_t streaming_fragments = 0U;
    bool streaming_truncated = false;
    bool preview_seen = false;

    ProgressiveImportConfig progressive;
    progressive.preview_records = 1U;
    const auto progressive_started = Clock::now();
    StoreStats final_store{};
    const bool imported = zevryon::massivedoc::import_zmdoc_corpus_progressive(
        titan_path,
        store_root,
        preview_root,
        progressive,
        [&](const ProgressivePreviewInfo& info, std::string* callback_error) {
            if (preview_seen) {
                *callback_error = "progressive import emitted multiple first previews";
                return false;
            }
            if (info.remaining_records == 0U || info.store.corpus.logical_records == 0U) {
                *callback_error = "progressive preview was not produced before import completion";
                return false;
            }
            LayoutWindowEngine preview_engine(info.root, runtime.layout, runtime.store_read);
            if (!preview_engine.open(callback_error)) {
                return false;
            }
            LayoutWindowResult preview_layout;
            if (!preview_engine.layout(
                    0U,
                    kViewportWidthQ8,
                    kViewportHeightQ8,
                    kOverscanQ8,
                    kMaxFragments,
                    &preview_layout,
                    callback_error)) {
                return false;
            }
            if (preview_layout.fragments.empty() || preview_layout.truncated) {
                *callback_error = "progressive first viewport was empty or truncated";
                return false;
            }
            streaming_ms = milliseconds_since(progressive_started);
            streaming_remaining_records = info.remaining_records;
            streaming_preview_records = info.store.corpus.logical_records;
            streaming_fragments = preview_layout.fragments.size();
            streaming_truncated = preview_layout.truncated;
            preview_seen = true;
            return true;
        },
        &final_store,
        error);
    if (!imported || !preview_seen) {
        return false;
    }

    zevryon::massivedoc::ArenaStats arena_stats;
    if (!zevryon::massivedoc::build_compact_arena(store_root, {}, &arena_stats, error)) {
        return false;
    }
    if (!require(
            arena_stats.logical_records == final_store.corpus.logical_records &&
                final_store.corpus.logical_records > 3U,
            "full-store compact arena identity drifted",
            error)) {
        return false;
    }

    double preindexed_ms = 0.0;
    LayoutWindowResult first_layout;
    std::vector<double> scroll_samples;
    std::vector<std::uint64_t> scroll_coordinates;
    scroll_samples.reserve(kMeasuredSamples);
    scroll_coordinates.reserve(kMeasuredSamples);
    {
        LayoutWindowEngine engine(store_root, runtime.layout, runtime.store_read);
        const auto started = Clock::now();
        if (!engine.open(error) || !engine.layout(
                0U,
                kViewportWidthQ8,
                kViewportHeightQ8,
                kOverscanQ8,
                kMaxFragments,
                &first_layout,
                error)) {
            return false;
        }
        preindexed_ms = milliseconds_since(started);
        if (!require(
                !first_layout.fragments.empty() && !first_layout.truncated,
                "preindexed first viewport was empty or truncated",
                error)) {
            return false;
        }

        const std::uint64_t maximum_scroll =
            first_layout.total_height_q8 > kViewportHeightQ8
                ? first_layout.total_height_q8 - kViewportHeightQ8
                : 0U;
        DeterministicCoordinateGenerator generator;
        for (std::size_t index = 0U; index < kWarmups + kMeasuredSamples; ++index) {
            const std::uint64_t coordinate = generator.next(maximum_scroll);
            LayoutWindowResult result;
            const auto query_started = Clock::now();
            if (!engine.layout(
                    coordinate,
                    kViewportWidthQ8,
                    kViewportHeightQ8,
                    kOverscanQ8,
                    kMaxFragments,
                    &result,
                    error)) {
                return false;
            }
            const double elapsed = milliseconds_since(query_started);
            if (!require(!result.fragments.empty() && !result.truncated, "scroll layout was empty or truncated", error)) {
                return false;
            }
            if (index >= kWarmups) {
                scroll_coordinates.push_back(coordinate);
                scroll_samples.push_back(elapsed);
            }
        }
    }
    if (!require(
            scroll_samples.size() == kMeasuredSamples &&
                scroll_coordinates.size() == kMeasuredSamples,
            "scroll sample count drifted",
            error)) {
        return false;
    }

    double cold_search_ms = 0.0;
    double warm_search_ms = 0.0;
    std::vector<SearchHit> cold_hits;
    std::vector<SearchHit> warm_hits;
    zevryon::massivedoc::ImmutableBlockCacheStats cache_before{};
    zevryon::massivedoc::ImmutableBlockCacheStats cache_after_cold{};
    zevryon::massivedoc::ImmutableBlockCacheStats cache_after_warm{};
    {
        StoreReader reader(store_root, runtime.store_read);
        if (!reader.open(error)) {
            return false;
        }
        reader.evict_block_cache_to_cold();
        cache_before = reader.block_cache_stats();
        auto started = Clock::now();
        cold_hits = reader.find(kTailMarker, 16U, error);
        cold_search_ms = milliseconds_since(started);
        if (!error->empty()) {
            return false;
        }
        cache_after_cold = reader.block_cache_stats();
        started = Clock::now();
        warm_hits = reader.find(kTailMarker, 16U, error);
        warm_search_ms = milliseconds_since(started);
        if (!error->empty()) {
            return false;
        }
        cache_after_warm = reader.block_cache_stats();
    }
    if (!require(
            !cold_hits.empty() && same_hits(cold_hits, warm_hits),
            "cold/warm exact search hit identity drifted",
            error)) {
        return false;
    }
    const SearchHit terminal_hit = cold_hits.back();
    if (!require(
            terminal_hit.record_index + 1U == final_store.corpus.logical_records &&
                terminal_hit.logical_id + 1U == final_store.corpus.logical_records,
            "Titan tail marker did not resolve to the final logical record",
            error)) {
        return false;
    }

    std::vector<double> mutation_samples;
    std::vector<std::uint64_t> mutation_indices;
    mutation_samples.reserve(kMeasuredSamples);
    mutation_indices.reserve(kMeasuredSamples);
    std::uint64_t mutation_initial_total_height = 0U;
    {
        CompactArenaReader arena(store_root);
        if (!arena.open(error)) {
            return false;
        }
        mutation_initial_total_height = arena.stats().total_height_q8;
        const std::uint64_t records = arena.stats().logical_records;
        if (!require(records != 0U, "mutation arena is empty", error)) {
            return false;
        }
        DeterministicCoordinateGenerator generator;
        for (std::size_t sample = 0U; sample < kWarmups + kMeasuredSamples; ++sample) {
            const std::uint64_t index = generator.next(records - 1U);
            double elapsed_us = 0.0;
            if (!run_mutation(&arena, index, &elapsed_us, error)) {
                return false;
            }
            if (sample >= kWarmups) {
                mutation_indices.push_back(index);
                mutation_samples.push_back(elapsed_us);
            }
        }
    }
    if (!require(
            mutation_samples.size() == kMeasuredSamples &&
                mutation_indices.size() == kMeasuredSamples,
            "mutation sample count drifted",
            error)) {
        return false;
    }
    {
        CompactArenaReader reopened(store_root);
        if (!reopened.open(error)) {
            return false;
        }
        if (!require(
                reopened.stats().total_height_q8 == mutation_initial_total_height,
                "mutation phase did not restore original total height",
                error)) {
            return false;
        }
    }

    FullDocumentExportStats copy_stats;
    double copy_seconds = 0.0;
    {
        StoreReader reader(store_root, runtime.store_read);
        if (!reader.open(error)) {
            return false;
        }
        CompactArenaReader arena(store_root);
        if (!arena.open(error)) {
            return false;
        }
        const auto selection =
            zevryon::massivedoc::full_document_selection(arena.logical_snapshot());
        if (!require(
                selection.text_bytes() == final_store.corpus.logical_utf8_bytes &&
                    selection.record_count == final_store.corpus.logical_records,
                "full-document selection does not match Titan store",
                error)) {
            return false;
        }
        if (std::filesystem::exists(copy_output)) {
            *error = "copy output unexpectedly already exists";
            return false;
        }
        const auto started = Clock::now();
        if (!zevryon::massivedoc::export_full_document(
                reader,
                selection,
                copy_output,
                FullDocumentExportFormat::Text,
                [] { return false; },
                FullDocumentExportOptions{},
                &copy_stats,
                error)) {
            return false;
        }
        copy_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    }
    if (!require(
            copy_seconds > 0.0 &&
                copy_stats.source_bytes == final_store.corpus.logical_utf8_bytes &&
                copy_stats.output_bytes == final_store.corpus.logical_utf8_bytes &&
                !copy_stats.cancelled,
            "full-document copy receipt drifted",
            error)) {
        return false;
    }

    const auto& limits = runtime.limits;
    const auto& budgets = runtime.budgets;
    const std::size_t hot_allocated =
        budgets.hot_block_cache_bytes + budgets.hot_layout_cache_bytes;
    const std::size_t warm_allocated =
        budgets.warm_block_cache_bytes + budgets.warm_checkpoint_cache_bytes +
        budgets.warm_source_window_cache_bytes;
    const std::size_t cold_allocated = budgets.cold_mapped_window_bytes;

    std::ostringstream output;
    output.precision(17);
    output << '{'
           << "\"schema\":\"zevryon.m8.profile-probe.v1\","
           << "\"device_class\":\"" << limits.name << "\","
           << "\"runtime_policy\":{"
           << "\"profile\":\"" << limits.name << "\","
           << "\"within_profile_budgets\":" << (budgets.within_profile_budgets ? "true" : "false") << ','
           << "\"layout_store_read_policy_applied\":true,"
           << "\"hot_budget_bytes\":" << budgets.hot_budget_bytes << ','
           << "\"hot_allocated_bytes\":" << hot_allocated << ','
           << "\"warm_budget_bytes\":" << budgets.warm_budget_bytes << ','
           << "\"warm_allocated_bytes\":" << warm_allocated << ','
           << "\"cold_budget_bytes\":" << budgets.cold_budget_bytes << ','
           << "\"cold_allocated_bytes\":" << cold_allocated << "},"
           << "\"store\":{"
           << "\"logical_utf8_bytes\":" << final_store.corpus.logical_utf8_bytes << ','
           << "\"logical_records\":" << final_store.corpus.logical_records << ','
           << "\"logical_nodes\":" << final_store.corpus.logical_nodes << ','
           << "\"style_runs\":" << final_store.corpus.style_runs << ','
           << "\"resource_references\":" << final_store.corpus.resource_references << ','
           << "\"largest_record_bytes\":" << final_store.corpus.largest_record_bytes << ','
           << "\"payload_sha256\":\"" << final_store.payload_sha256 << "\"},"
           << "\"streaming\":{"
           << "\"milliseconds\":" << streaming_ms << ','
           << "\"preview_records\":" << streaming_preview_records << ','
           << "\"remaining_records\":" << streaming_remaining_records << ','
           << "\"fragment_count\":" << streaming_fragments << ','
           << "\"truncated\":" << (streaming_truncated ? "true" : "false") << "},"
           << "\"preindexed\":{"
           << "\"milliseconds\":" << preindexed_ms << ','
           << "\"fragment_count\":" << first_layout.fragments.size() << ','
           << "\"total_height_q8\":" << first_layout.total_height_q8 << ','
           << "\"truncated\":" << (first_layout.truncated ? "true" : "false") << "},"
           << "\"scroll\":{"
           << "\"warmup_count\":" << kWarmups << ','
           << "\"measured_count\":" << kMeasuredSamples << ','
           << "\"coordinates_q8\":";
    write_u64_array(output, scroll_coordinates);
    output << ",\"samples_ms\":";
    write_number_array(output, scroll_samples);
    output << "},\"search\":{"
           << "\"query\":\"ZEVRYON_M8_TITAN_TAIL\","
           << "\"cold_ms\":" << cold_search_ms << ','
           << "\"warm_ms\":" << warm_search_ms << ','
           << "\"terminal_record_index\":" << terminal_hit.record_index << ','
           << "\"terminal_logical_id\":" << terminal_hit.logical_id << ','
           << "\"terminal_byte_offset\":" << terminal_hit.byte_offset << ','
           << "\"hit_count\":" << cold_hits.size() << ','
           << "\"cache_before_cold_misses\":" << cache_before.cold_misses << ','
           << "\"cache_after_cold_misses\":" << cache_after_cold.cold_misses << ','
           << "\"cache_after_warm_hot_hits\":" << cache_after_warm.hot_hits << ','
           << "\"cache_after_warm_warm_hits\":" << cache_after_warm.warm_hits << "},"
           << "\"mutation\":{"
           << "\"warmup_count\":" << kWarmups << ','
           << "\"measured_count\":" << kMeasuredSamples << ','
           << "\"indices\":";
    write_u64_array(output, mutation_indices);
    output << ",\"samples_us\":";
    write_number_array(output, mutation_samples);
    output << ",\"restored\":true,\"initial_total_height_q8\":"
           << mutation_initial_total_height << "},"
           << "\"copy\":{"
           << "\"source_bytes\":" << copy_stats.source_bytes << ','
           << "\"output_bytes\":" << copy_stats.output_bytes << ','
           << "\"elapsed_seconds\":" << copy_seconds << ','
           << "\"records_exported\":" << copy_stats.records_exported << ','
           << "\"peak_input_block_bytes\":" << copy_stats.peak_input_block_bytes << ','
           << "\"peak_output_buffer_bytes\":" << copy_stats.peak_output_buffer_bytes << ','
           << "\"peak_validation_codepoints\":" << copy_stats.peak_validation_codepoints << ','
           << "\"cancelled\":" << (copy_stats.cancelled ? "true" : "false") << "}"
           << '}';
    *rendered = output.str();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: zevryon-m8-profile-case-probe <device-profile> <titan.zmdoc> <new-work-dir>\n";
        return 2;
    }
    const auto* limits = zevryon::massivedoc::find_massive_doc_profile_limits(argv[1]);
    if (limits == nullptr) {
        std::cerr << "unknown M8 device profile\n";
        return 2;
    }
    std::string rendered;
    std::string error;
    try {
        if (!run_probe(limits->profile, argv[2], argv[3], &rendered, &error)) {
            std::cerr << "M8 profile case probe failed: " << error << '\n';
            return 1;
        }
    } catch (const std::exception& exc) {
        std::cerr << "M8 profile case probe exception: " << exc.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "M8 profile case probe unknown exception\n";
        return 1;
    }
    std::cout << rendered << '\n';
    return 0;
}
