#include "bidi_explicit.hpp"
#include "grapheme_segmenter.hpp"
#include "ledger_memory_resource.hpp"
#include "resource_ledger.hpp"
#include "script_run.hpp"
#include "unicode_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace {

using zevryon::core::LedgerMemoryResource;
using zevryon::core::ResourceClass;
using zevryon::core::ResourceLedger;

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

void checksum_byte(std::uint64_t& checksum, std::uint8_t value) noexcept {
    checksum ^= value;
    checksum *= kFnvPrime;
}

void checksum_u64(std::uint64_t& checksum, std::uint64_t value) noexcept {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        checksum_byte(checksum, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void checksum_text(std::uint64_t& checksum, std::string_view text) noexcept {
    for (const char character : text) {
        checksum_byte(checksum, static_cast<std::uint8_t>(character));
    }
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::vector<std::byte> make_fixture(std::size_t target_bytes) {
    const std::vector<std::uint8_t> pattern{
        0x5aU, 0x65U, 0x76U, 0x72U, 0x79U, 0x6fU, 0x6eU, 0x20U,
        0xc5U, 0x9fU, 0xc4U, 0x9fU, 0xc4U, 0xb1U, 0x20U,
        0x65U, 0xccU, 0x81U, 0x20U,
        0xe4U, 0xb8U, 0xadU, 0x20U,
        0xd7U, 0xa9U, 0x20U,
        0xd8U, 0xb9U, 0x20U,
        0xf0U, 0x9fU, 0x91U, 0xa9U,
        0xe2U, 0x80U, 0x8dU,
        0xf0U, 0x9fU, 0x9aU, 0x80U, 0x20U,
        0xe2U, 0x81U, 0xa6U, 0x61U, 0x62U, 0xe2U, 0x81U, 0xa9U, 0x20U,
        0xe2U, 0x81U, 0xa7U, 0xd7U, 0x90U, 0xd7U, 0x91U, 0xe2U, 0x81U, 0xa9U,
        0x0aU,
    };
    std::vector<std::byte> fixture;
    fixture.reserve(target_bytes);
    while (fixture.size() < target_bytes) {
        const std::size_t count = std::min(target_bytes - fixture.size(), pattern.size());
        for (std::size_t index = 0U; index < count; ++index) {
            fixture.push_back(static_cast<std::byte>(pattern[index]));
        }
    }

    std::size_t suffix = fixture.size();
    while (suffix > 0U) {
        const std::uint8_t value = static_cast<std::uint8_t>(
            std::to_integer<unsigned int>(fixture[suffix - 1U]));
        if ((value & 0xc0U) != 0x80U) {
            break;
        }
        fixture[suffix - 1U] = static_cast<std::byte>('x');
        --suffix;
    }
    if (suffix > 0U) {
        const std::uint8_t value = static_cast<std::uint8_t>(
            std::to_integer<unsigned int>(fixture[suffix - 1U]));
        if (value >= 0xc2U) {
            fixture[suffix - 1U] = static_cast<std::byte>('x');
        }
    }
    return fixture;
}

struct StageEvidence {
    std::string name;
    double wall_ms{0.0};
    std::uint64_t input_items{0};
    std::uint64_t output_items{0};
    std::uint64_t semantic_checksum{0};
    std::size_t current_bytes{0};
    std::size_t peak_bytes{0};
    std::uint64_t shadow_operations{0};
    std::uint64_t shadow_verifications{0};
    std::uint64_t shadow_mismatches{0};
    bool shadow_enabled{false};
    bool shadow_exact{false};
    bool shadow_healthy{false};
    bool within_hard_limits{false};
    bool accounting_clean{false};
    std::string shadow_json;
};

bool finalize_stage(
    ResourceLedger& ledger,
    StageEvidence* evidence,
    std::string* error) {
    if (evidence == nullptr || error == nullptr) {
        return false;
    }
    evidence->shadow_enabled = ledger.rust_shadow_enabled();
    evidence->shadow_exact =
        !evidence->shadow_enabled || ledger.verify_rust_shadow();
    evidence->shadow_healthy =
        !evidence->shadow_enabled || ledger.rust_shadow_healthy();
    evidence->shadow_operations = ledger.rust_shadow_operations();
    evidence->shadow_verifications = ledger.rust_shadow_verifications();
    evidence->shadow_mismatches = ledger.rust_shadow_mismatches();
    evidence->current_bytes = ledger.total_current_bytes();
    evidence->peak_bytes = ledger.total_peak_bytes();
    evidence->within_hard_limits = ledger.within_hard_limits();
    evidence->accounting_clean = ledger.accounting_clean();
    evidence->shadow_json = ledger.rust_shadow_json();

    if (!evidence->shadow_exact || !evidence->shadow_healthy ||
        evidence->shadow_mismatches != 0U || evidence->current_bytes != 0U ||
        !evidence->within_hard_limits || !evidence->accounting_clean) {
        *error = evidence->name + " ledger certification failed";
        return false;
    }
    if (evidence->shadow_enabled && evidence->shadow_operations == 0U) {
        *error = evidence->name + " did not reach the Rust shadow";
        return false;
    }
    return true;
}

void write_stage_json(std::ostream& output, const StageEvidence& stage) {
    output << "{\"name\":\"" << stage.name << "\""
           << ",\"wall_ms\":" << stage.wall_ms
           << ",\"input_items\":" << stage.input_items
           << ",\"output_items\":" << stage.output_items
           << ",\"semantic_checksum\":\"" << hex_u64(stage.semantic_checksum) << "\""
           << ",\"current_bytes\":" << stage.current_bytes
           << ",\"peak_bytes\":" << stage.peak_bytes
           << ",\"within_hard_limits\":"
           << (stage.within_hard_limits ? "true" : "false")
           << ",\"accounting_clean\":"
           << (stage.accounting_clean ? "true" : "false")
           << ",\"shadow_enabled\":"
           << (stage.shadow_enabled ? "true" : "false")
           << ",\"shadow_exact\":"
           << (stage.shadow_exact ? "true" : "false")
           << ",\"shadow_healthy\":"
           << (stage.shadow_healthy ? "true" : "false")
           << ",\"shadow_operations\":" << stage.shadow_operations
           << ",\"shadow_verifications\":" << stage.shadow_verifications
           << ",\"shadow_mismatches\":" << stage.shadow_mismatches
           << ",\"shadow\":" << stage.shadow_json << '}';
}

} // namespace

int main() {
    constexpr std::size_t kFixtureBytes = 64U * 1024U;
    const std::vector<std::byte> fixture = make_fixture(kFixtureBytes);
    std::vector<zevryon::text::DecodedCodePoint> codepoints;
    std::vector<zevryon::text::GraphemeBoundary> graphemes;
    std::vector<StageEvidence> stages;
    stages.reserve(4U);
    std::string error;
    std::uint64_t pipeline_checksum = kFnvOffset;

    {
        ResourceLedger ledger;
        ledger.set_hard_limit(ResourceClass::UnicodeBuffer, 4U * 1024U * 1024U);
        StageEvidence stage;
        stage.name = "unicode";
        stage.input_items = fixture.size();
        const auto started = std::chrono::steady_clock::now();
        {
            LedgerMemoryResource memory(ledger, ResourceClass::UnicodeBuffer);
            std::pmr::vector<zevryon::text::DecodedCodePoint> output(&memory);
            zevryon::text::Utf8StreamDecoder decoder(zevryon::text::Utf8ErrorPolicy::Strict);
            zevryon::text::Utf8DecodeError decode_error;
            std::size_t consumed = 0U;
            while (consumed < fixture.size()) {
                const std::size_t count = std::min<std::size_t>(4096U, fixture.size() - consumed);
                if (!decoder.feed(
                        std::span<const std::byte>(fixture.data() + consumed, count),
                        static_cast<std::uint64_t>(consumed),
                        &output,
                        &decode_error)) {
                    std::cerr << "Unicode decode failed: " << decode_error.message << '\n';
                    return 1;
                }
                consumed += count;
            }
            if (!decoder.finish(&output, &decode_error) ||
                decoder.stats().source_bytes != fixture.size() ||
                decoder.stats().invalid_sequences != 0U || output.empty()) {
                std::cerr << "Unicode decode contract failed\n";
                return 1;
            }
            codepoints.assign(output.begin(), output.end());
        }
        stage.wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        stage.output_items = codepoints.size();
        stage.semantic_checksum = kFnvOffset;
        for (const auto& item : codepoints) {
            checksum_u64(stage.semantic_checksum, item.source_start);
            checksum_u64(stage.semantic_checksum, item.value);
            checksum_u64(stage.semantic_checksum, item.source_length);
            checksum_u64(stage.semantic_checksum, item.replacement ? 1U : 0U);
        }
        checksum_text(pipeline_checksum, stage.name);
        checksum_u64(pipeline_checksum, stage.semantic_checksum);
        if (!finalize_stage(ledger, &stage, &error)) {
            std::cerr << error << '\n';
            return 1;
        }
        stages.push_back(std::move(stage));
    }

    {
        ResourceLedger ledger;
        ledger.set_hard_limit(ResourceClass::GraphemeCluster, 2U * 1024U * 1024U);
        StageEvidence stage;
        stage.name = "grapheme";
        stage.input_items = codepoints.size();
        zevryon::text::GraphemeSegmentStats stats;
        const auto started = std::chrono::steady_clock::now();
        {
            LedgerMemoryResource memory(ledger, ResourceClass::GraphemeCluster);
            std::pmr::vector<zevryon::text::GraphemeBoundary> output(&memory);
            zevryon::text::GraphemeError grapheme_error;
            if (!zevryon::text::segment_graphemes(
                    codepoints, &output, &stats, &grapheme_error) || output.size() < 2U) {
                std::cerr << "Grapheme segmentation failed: " << grapheme_error.message << '\n';
                return 1;
            }
            graphemes.assign(output.begin(), output.end());
        }
        stage.wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        stage.output_items = stats.output_clusters;
        stage.semantic_checksum = kFnvOffset;
        for (const auto& item : graphemes) {
            checksum_u64(stage.semantic_checksum, item.source_offset);
            checksum_u64(stage.semantic_checksum, item.codepoint_index);
        }
        checksum_u64(stage.semantic_checksum, stats.suppressed_breaks);
        checksum_u64(stage.semantic_checksum, stats.maximum_cluster_codepoints);
        checksum_text(pipeline_checksum, stage.name);
        checksum_u64(pipeline_checksum, stage.semantic_checksum);
        if (!finalize_stage(ledger, &stage, &error)) {
            std::cerr << error << '\n';
            return 1;
        }
        stages.push_back(std::move(stage));
    }

    {
        ResourceLedger ledger;
        ledger.set_hard_limit(ResourceClass::ScriptRun, 1U * 1024U * 1024U);
        StageEvidence stage;
        stage.name = "script";
        stage.input_items = graphemes.size() > 0U ? graphemes.size() - 1U : 0U;
        zevryon::text::ScriptRunStats stats;
        const auto started = std::chrono::steady_clock::now();
        {
            LedgerMemoryResource memory(ledger, ResourceClass::ScriptRun);
            std::pmr::vector<zevryon::text::ScriptRunBoundary> output(&memory);
            zevryon::text::ScriptRunError script_error;
            if (!zevryon::text::resolve_script_runs(
                    codepoints, graphemes, &output, &stats, &script_error) || output.size() < 2U) {
                std::cerr << "Script-run resolution failed: " << script_error.message << '\n';
                return 1;
            }
            stage.semantic_checksum = kFnvOffset;
            for (const auto& item : output) {
                checksum_u64(stage.semantic_checksum, item.source_offset);
                checksum_u64(stage.semantic_checksum, item.cluster_index);
                checksum_u64(stage.semantic_checksum, static_cast<std::uint64_t>(item.script));
            }
            stage.output_items = stats.output_runs;
        }
        stage.wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        checksum_u64(stage.semantic_checksum, stats.neutral_clusters);
        checksum_u64(stage.semantic_checksum, stats.explicit_extension_lookups);
        checksum_u64(stage.semantic_checksum, stats.internal_cluster_conflicts);
        checksum_text(pipeline_checksum, stage.name);
        checksum_u64(pipeline_checksum, stage.semantic_checksum);
        if (!finalize_stage(ledger, &stage, &error)) {
            std::cerr << error << '\n';
            return 1;
        }
        stages.push_back(std::move(stage));
    }

    {
        ResourceLedger ledger;
        ledger.set_hard_limit(ResourceClass::BidiRun, 2U * 1024U * 1024U);
        StageEvidence stage;
        stage.name = "bidi";
        stage.input_items = codepoints.size();
        zevryon::text::BidiExplicitStats stats;
        const auto started = std::chrono::steady_clock::now();
        {
            LedgerMemoryResource memory(ledger, ResourceClass::BidiRun);
            std::pmr::vector<zevryon::text::BidiExplicitUnit> output(&memory);
            zevryon::text::BidiExplicitError bidi_error;
            if (!zevryon::text::resolve_bidi_explicit(
                    codepoints,
                    zevryon::text::BidiParagraphDirection::Auto,
                    &output,
                    &stats,
                    &bidi_error) || output.size() != codepoints.size()) {
                std::cerr << "Bidi explicit resolution failed: " << bidi_error.message << '\n';
                return 1;
            }
            stage.semantic_checksum = kFnvOffset;
            for (const auto& item : output) {
                checksum_u64(stage.semantic_checksum, item.source_offset);
                checksum_u64(stage.semantic_checksum, item.codepoint_index);
                checksum_u64(stage.semantic_checksum, static_cast<std::uint64_t>(item.original_class));
                checksum_u64(stage.semantic_checksum, static_cast<std::uint64_t>(item.resolved_class));
                checksum_u64(stage.semantic_checksum, item.level);
                checksum_u64(stage.semantic_checksum, item.flags);
            }
            stage.output_items = stats.output_units;
        }
        stage.wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        checksum_u64(stage.semantic_checksum, stats.explicit_controls);
        checksum_u64(stage.semantic_checksum, stats.isolate_initiators);
        checksum_u64(stage.semantic_checksum, stats.fsi_resolutions);
        checksum_u64(stage.semantic_checksum, stats.maximum_level);
        checksum_text(pipeline_checksum, stage.name);
        checksum_u64(pipeline_checksum, stage.semantic_checksum);
        if (!finalize_stage(ledger, &stage, &error)) {
            std::cerr << error << '\n';
            return 1;
        }
        stages.push_back(std::move(stage));
    }

    const bool shadow_enabled = std::all_of(
        stages.begin(), stages.end(), [](const StageEvidence& stage) {
            return stage.shadow_enabled;
        });
    const bool shadow_disabled = std::all_of(
        stages.begin(), stages.end(), [](const StageEvidence& stage) {
            return !stage.shadow_enabled;
        });
    if (!shadow_enabled && !shadow_disabled) {
        std::cerr << "Mixed Rust-shadow state across stages\n";
        return 1;
    }

    std::uint64_t operations = 0U;
    std::uint64_t verifications = 0U;
    std::uint64_t mismatches = 0U;
    std::size_t peak_bytes = 0U;
    double wall_ms = 0.0;
    for (const auto& stage : stages) {
        operations += stage.shadow_operations;
        verifications += stage.shadow_verifications;
        mismatches += stage.shadow_mismatches;
        peak_bytes += stage.peak_bytes;
        wall_ms += stage.wall_ms;
    }

    std::cout << std::fixed << std::setprecision(6)
              << "{\"schema\":\"zevryon.rust-shadow-text-probe.v1\""
              << ",\"fixture_bytes\":" << fixture.size()
              << ",\"pipeline_checksum\":\"" << hex_u64(pipeline_checksum) << "\""
              << ",\"wall_ms\":" << wall_ms
              << ",\"rust_shadow_enabled\":" << (shadow_enabled ? "true" : "false")
              << ",\"exact_verification\":true"
              << ",\"rust_shadow_operations\":" << operations
              << ",\"rust_shadow_verifications\":" << verifications
              << ",\"rust_shadow_mismatches\":" << mismatches
              << ",\"total_peak_bytes\":" << peak_bytes
              << ",\"stages\":[";
    for (std::size_t index = 0U; index < stages.size(); ++index) {
        if (index != 0U) {
            std::cout << ',';
        }
        write_stage_json(std::cout, stages[index]);
    }
    std::cout << "]}\n";
    return 0;
}
